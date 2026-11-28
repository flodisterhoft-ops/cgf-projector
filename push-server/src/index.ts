// CGF Projector push server: tells church laptops about a new library release
// the moment it is published. One Durable Object per church holds the laptops'
// WebSocket connections, the recent releases (for laptops that were offline)
// and the device list. Protocol: docs/song-push.md.
//
// The server never sees the master library and never needs the signing key:
// releases arrive signed from the publisher's PC and the projector app checks
// the signature itself, so this server can only relay, never forge.

import { DurableObject } from "cloudflare:workers";
import {
	appSupports,
	type AppReleaseRow,
	FILE_CHUNK,
	inflateQt,
	isInt,
	KEEP_APP_RELEASES,
	KEEP_RELEASES,
	parseAppEnvelope,
	parseEnvelope,
	type ReleaseRow,
	SHA256,
} from "./release";

export interface PushEnv {
	CHURCH: DurableObjectNamespace<ChurchPush>;
	PUBLISH_TOKEN?: string;
}

const CHURCH_ID = /^[a-z0-9][a-z0-9-]{0,39}$/;
const DEVICE_NAME = /^[\p{L}\p{N} ._-]{1,60}$/u;

interface Attachment {
	church: string;
	device: string;
}

function json(body: unknown, status = 200): Response {
	return new Response(JSON.stringify(body), {
		status,
		headers: { "Content-Type": "application/json; charset=utf-8" },
	});
}

function now(): string {
	return new Date().toISOString();
}

async function sha256Hex(text: string): Promise<string> {
	const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
	return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// Constant-time comparison of two secrets (their digests have equal length).
async function sameSecret(given: string, expected: string): Promise<boolean> {
	const enc = new TextEncoder();
	const [a, b] = await Promise.all([
		crypto.subtle.digest("SHA-256", enc.encode(given)),
		crypto.subtle.digest("SHA-256", enc.encode(expected)),
	]);
	return crypto.subtle.timingSafeEqual(a, b);
}

function bearer(request: Request): string | null {
	const h = request.headers.get("Authorization") ?? "";
	const m = /^Bearer\s+(\S+)$/i.exec(h);
	return m ? m[1] : null;
}

export default {
	async fetch(request: Request, env: PushEnv): Promise<Response> {
		const url = new URL(request.url);
		const path = url.pathname.split("/").filter(Boolean);
		if (path.length === 0) return new Response("CGF Projector push server\n");
		if (path[0] !== "v1" || path.length < 3 || !CHURCH_ID.test(path[1])) return json({ error: "not found" }, 404);
		const church = path[1];
		const route = path.slice(2);
		const stub = env.CHURCH.getByName(church);

		// A laptop's connection. The object checks the device token itself
		// (one call instead of two); only its hash travels on.
		if (route.length === 1 && route[0] === "ws") {
			if (request.method !== "GET" || request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
				return json({ error: "expected a WebSocket upgrade" }, 426);
			const token = bearer(request);
			if (!token) return json({ error: "unauthorized" }, 401);
			const headers = new Headers(request.headers);
			headers.delete("Authorization");
			headers.set("X-CGF-Church", church);
			headers.set("X-CGF-Token-Hash", await sha256Hex(token));
			return stub.fetch(new Request(request.url, { method: "GET", headers }));
		}

		const token = bearer(request);
		const isPublisher = !!env.PUBLISH_TOKEN && !!token && (await sameSecret(token, env.PUBLISH_TOKEN));

		// An app file, by content hash: for laptops (device token) and the publisher.
		if (route.length === 3 && route[0] === "app" && route[1] === "files" && request.method === "GET") {
			if (!token || !SHA256.test(route[2])) return json({ error: "not found" }, 404);
			return stub.fetch(
				new Request(request.url, {
					headers: {
						"X-CGF-Op": "get-file",
						"X-CGF-Church": church,
						"X-CGF-Token-Hash": isPublisher ? "" : await sha256Hex(token),
						"X-CGF-Publisher": isPublisher ? "1" : "0",
						"X-CGF-Sha256": route[2],
					},
				}),
			);
		}

		// Everything else is the publisher's. A missing secret fails closed.
		if (!isPublisher) return json({ error: "unauthorized" }, 401);

		if (route[0] === "app") {
			if (route.length === 3 && route[1] === "files" && route[2] === "missing" && request.method === "POST") {
				let body: { sha256?: unknown };
				try {
					body = await request.json();
				} catch {
					return json({ error: "body is not JSON" }, 400);
				}
				const list = Array.isArray(body.sha256) ? body.sha256.filter((s) => typeof s === "string" && SHA256.test(s)) : [];
				return json({ missing: await stub.missingFiles(church, list as string[]) });
			}
			if (route.length === 3 && route[1] === "files" && request.method === "PUT") {
				const sha = route[2];
				if (!SHA256.test(sha)) return json({ error: "bad hash" }, 400);
				const stored = await request.arrayBuffer();
				// Check the upload before keeping it: a laptop must never get a bad file.
				const original = await inflateQt(stored);
				if (!original) return json({ error: "not a qCompress stream" }, 400);
				const digest = await crypto.subtle.digest("SHA-256", original);
				const actual = [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
				if (actual !== sha) return json({ error: "the file does not match its hash" }, 400);
				return stub.fetch(
					new Request(request.url, {
						method: "PUT",
						body: stored,
						headers: {
							"X-CGF-Op": "put-file",
							"X-CGF-Church": church,
							"X-CGF-Sha256": sha,
							"X-CGF-Bytes": String(original.byteLength),
						},
					}),
				);
			}
			if (route.length === 2 && route[1] === "releases" && request.method === "POST") {
				let body: { release?: unknown };
				try {
					body = await request.json();
				} catch {
					return json({ error: "body is not JSON" }, 400);
				}
				const r = parseAppEnvelope(body.release, church);
				if (typeof r === "string") return json({ error: r }, 400);
				const result = await stub.publishApp(church, r);
				return json(result, "error" in result ? 400 : 200);
			}
			return json({ error: "not found" }, 404);
		}

		if (route.length === 1 && route[0] === "releases" && request.method === "POST") {
			let body: { parts?: unknown };
			try {
				body = await request.json();
			} catch {
				return json({ error: "body is not JSON" }, 400);
			}
			if (!Array.isArray(body.parts) || body.parts.length === 0) return json({ error: "no parts" }, 400);
			const rows: ReleaseRow[] = [];
			for (const e of body.parts) {
				const r = parseEnvelope(e, church);
				if (typeof r === "string") return json({ error: r }, 400);
				rows.push(r);
			}
			// Every release arrives whole, so a laptop never sees half of one.
			const byVersion = new Map<number, ReleaseRow[]>();
			for (const r of rows) byVersion.set(r.to, [...(byVersion.get(r.to) ?? []), r]);
			for (const [to, list] of byVersion) {
				const nums = new Set(list.map((r) => r.part));
				const { parts, from } = list[0];
				if (list.some((r) => r.parts !== parts || r.from !== from) || nums.size !== parts || list.length !== parts)
					return json({ error: `release ${to} is incomplete` }, 400);
			}
			return json(await stub.publish(church, rows));
		}
		if (route.length === 1 && route[0] === "devices" && request.method === "POST") {
			let body: { name?: unknown };
			try {
				body = await request.json();
			} catch {
				return json({ error: "body is not JSON" }, 400);
			}
			if (typeof body.name !== "string" || !DEVICE_NAME.test(body.name))
				return json({ error: "a device needs a name (letters, digits, space . _ -)" }, 400);
			return json(await stub.createDevice(church, body.name));
		}
		if (route.length === 2 && route[0] === "devices" && request.method === "DELETE") {
			const name = decodeURIComponent(route[1]);
			return json(await stub.deleteDevice(church, name));
		}
		if (route.length === 1 && route[0] === "status" && request.method === "GET") {
			return json(await stub.status(church));
		}
		return json({ error: "not found" }, 404);
	},
} satisfies ExportedHandler<PushEnv>;

export class ChurchPush extends DurableObject<PushEnv> {
	private sql: SqlStorage;

	constructor(ctx: DurableObjectState, env: PushEnv) {
		super(ctx, env);
		this.sql = ctx.storage.sql;
		// Runs again whenever the object wakes from hibernation: keep it cheap.
		ctx.blockConcurrencyWhile(async () => {
			this.sql.exec(`
				CREATE TABLE IF NOT EXISTS releases (
					church_id TEXT NOT NULL,
					to_version INTEGER NOT NULL,
					part INTEGER NOT NULL,
					parts INTEGER NOT NULL,
					from_version INTEGER NOT NULL,
					envelope TEXT NOT NULL,
					received_at TEXT NOT NULL,
					PRIMARY KEY (church_id, to_version, part)
				);
				CREATE TABLE IF NOT EXISTS devices (
					church_id TEXT NOT NULL,
					name TEXT NOT NULL,
					token_sha256 TEXT NOT NULL UNIQUE,
					created_at TEXT NOT NULL,
					last_seen_at TEXT,
					last_version INTEGER,
					PRIMARY KEY (church_id, name)
				);
				CREATE TABLE IF NOT EXISTS app_releases (
					church_id TEXT NOT NULL,
					seq INTEGER NOT NULL,
					folder TEXT NOT NULL,
					envelope TEXT NOT NULL,
					received_at TEXT NOT NULL,
					PRIMARY KEY (church_id, seq)
				);
				CREATE TABLE IF NOT EXISTS app_file_info (
					church_id TEXT NOT NULL,
					sha256 TEXT NOT NULL,
					chunks INTEGER NOT NULL,
					bytes INTEGER NOT NULL,
					stored_bytes INTEGER NOT NULL,
					created_at TEXT NOT NULL,
					PRIMARY KEY (church_id, sha256)
				);
				CREATE TABLE IF NOT EXISTS app_files (
					church_id TEXT NOT NULL,
					sha256 TEXT NOT NULL,
					chunk INTEGER NOT NULL,
					data BLOB NOT NULL,
					PRIMARY KEY (church_id, sha256, chunk)
				);
			`);
			// Added with app updates (2026-11-23): the app version each laptop runs.
			const cols = this.sql.exec<{ name: string }>("PRAGMA table_info(devices)").toArray().map((c) => c.name);
			if (!cols.includes("app_folder")) this.sql.exec("ALTER TABLE devices ADD COLUMN app_folder TEXT");
			// Added with Bible releases (2026-11-27): what a release needs, comma-separated.
			const rcols = this.sql.exec<{ name: string }>("PRAGMA table_info(releases)").toArray().map((c) => c.name);
			if (!rcols.includes("needs")) this.sql.exec("ALTER TABLE releases ADD COLUMN needs TEXT NOT NULL DEFAULT ''");
		});
		// Keep-alives are answered without waking the object.
		ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
	}

	async fetch(request: Request): Promise<Response> {
		const op = request.headers.get("X-CGF-Op");
		if (op === "put-file") return this.putFile(request);
		if (op === "get-file") return this.getFile(request);

		const church = request.headers.get("X-CGF-Church") ?? "";
		const hash = request.headers.get("X-CGF-Token-Hash") ?? "";
		const device = this.sql
			.exec<{ name: string }>("SELECT name FROM devices WHERE church_id = ? AND token_sha256 = ?", church, hash)
			.toArray()[0];
		if (!device) return json({ error: "unauthorized" }, 401);

		const [client, server] = Object.values(new WebSocketPair());
		this.ctx.acceptWebSocket(server, [device.name]);
		server.serializeAttachment({ church, device: device.name } satisfies Attachment);
		this.touch(church, device.name, null);
		return new Response(null, { status: 101, webSocket: client });
	}

	async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
		if (typeof message !== "string") return;
		const a = ws.deserializeAttachment() as Attachment | null;
		if (!a) {
			ws.close(1008, "unknown connection");
			return;
		}
		let m: Record<string, unknown>;
		try {
			m = JSON.parse(message);
		} catch {
			return;
		}
		// A laptop without the projector app open has no library version: it
		// gets app releases only, and its songs when the app opens.
		const version = isInt(m.version) && m.version >= 0 ? m.version : null;
		if (typeof m.app === "string" && m.app.length <= 60) this.setAppFolder(a.church, a.device, m.app);
		if (m.type === "hello") {
			this.touch(a.church, a.device, null);
			this.replay(ws, a, version);
			const app = this.latestApp(a.church);
			if (app) ws.send(app.envelope);
		} else if (m.type === "ack" && version !== null) {
			this.touch(a.church, a.device, version);
		} else if (m.type === "status") {
			this.touch(a.church, a.device, null);
		}
	}

	async webSocketClose(ws: WebSocket, code: number, reason: string): Promise<void> {
		try {
			if (code >= 1000 && code < 5000 && ![1004, 1005, 1006, 1015].includes(code)) ws.close(code, reason);
			else ws.close();
		} catch {
			// already closed
		}
	}

	async webSocketError(_ws: WebSocket, error: unknown): Promise<void> {
		console.error("WebSocket error", error);
	}

	// Store a published release and send it to every connected laptop.
	async publish(church: string, rows: ReleaseRow[]) {
		const received = now();
		const sorted = [...rows].sort((x, y) => x.to - y.to || x.part - y.part);
		for (const r of sorted)
			this.sql.exec(
				`INSERT OR REPLACE INTO releases (church_id, to_version, part, parts, from_version, envelope, received_at, needs)
				 VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
				church, r.to, r.part, r.parts, r.from, r.envelope, received, r.needs.join(","),
			);
		const latest = this.latest(church);
		this.sql.exec("DELETE FROM releases WHERE church_id = ? AND to_version <= ?", church, latest - KEEP_RELEASES);

		// A laptop at another version notices the gap and asks for a replay.
		// One whose app cannot take a release gets nothing from it on; its
		// next hello after the app update brings it.
		const sockets = this.ctx.getWebSockets();
		for (const ws of sockets) {
			const a = ws.deserializeAttachment() as Attachment | null;
			const folder = a ? this.appFolder(a.church, a.device) : null;
			for (const r of sorted) {
				if (!appSupports(folder, r.needs)) break;
				try {
					ws.send(r.envelope);
				} catch {
					// a closing connection; it catches up when it reconnects
				}
			}
		}
		return { stored: rows.length, latest, connected: sockets.length };
	}

	async createDevice(church: string, name: string) {
		const bytes = crypto.getRandomValues(new Uint8Array(32));
		const token = btoa(String.fromCharCode(...bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
		this.sql.exec(
			`INSERT INTO devices (church_id, name, token_sha256, created_at) VALUES (?, ?, ?, ?)
			 ON CONFLICT (church_id, name) DO UPDATE SET token_sha256 = excluded.token_sha256,
			   created_at = excluded.created_at, last_seen_at = NULL, last_version = NULL, app_folder = NULL`,
			church, name, await sha256Hex(token), now(),
		);
		// A new token replaces the old one: drop connections made with it.
		for (const ws of this.ctx.getWebSockets(name)) ws.close(1008, "device token replaced");
		return { name, token };
	}

	async deleteDevice(church: string, name: string) {
		const deleted = this.sql.exec("DELETE FROM devices WHERE church_id = ? AND name = ?", church, name).rowsWritten;
		for (const ws of this.ctx.getWebSockets(name)) ws.close(1008, "device removed");
		return { deleted };
	}

	async status(church: string) {
		const devices = this.sql
			.exec<{ name: string; created_at: string; last_seen_at: string | null; last_version: number | null; app_folder: string | null }>(
				"SELECT name, created_at, last_seen_at, last_version, app_folder FROM devices WHERE church_id = ? ORDER BY name",
				church,
			)
			.toArray()
			.map((d) => ({ ...d, connected: this.ctx.getWebSockets(d.name).length > 0 }));
		const r = this.sql
			.exec<{ versions: number; oldest_from: number | null; latest_at: string | null }>(
				`SELECT COUNT(DISTINCT to_version) AS versions, MIN(from_version) AS oldest_from,
				        MAX(received_at) AS latest_at FROM releases WHERE church_id = ?`,
				church,
			)
			.one();
		const app = this.latestApp(church);
		const appFiles = this.sql
			.exec<{ files: number; stored: number | null }>(
				"SELECT COUNT(*) AS files, SUM(stored_bytes) AS stored FROM app_file_info WHERE church_id = ?",
				church,
			)
			.one();
		return {
			church,
			latest: this.latest(church),
			...r,
			app: app ? { seq: app.seq, folder: app.folder, received_at: app.received_at } : null,
			app_files: { count: appFiles.files, stored_bytes: appFiles.stored ?? 0 },
			devices,
		};
	}

	// ----------------------------------------------------------------------
	// App releases

	async missingFiles(church: string, shas: string[]): Promise<string[]> {
		const missing: string[] = [];
		for (const sha of new Set(shas)) {
			const row = this.sql
				.exec("SELECT 1 FROM app_file_info WHERE church_id = ? AND sha256 = ?", church, sha)
				.toArray()[0];
			if (!row) missing.push(sha);
		}
		return missing;
	}

	private async putFile(request: Request): Promise<Response> {
		const church = request.headers.get("X-CGF-Church") ?? "";
		const sha = request.headers.get("X-CGF-Sha256") ?? "";
		const bytes = Number(request.headers.get("X-CGF-Bytes") ?? "0");
		const stored = new Uint8Array(await request.arrayBuffer());
		const chunks = Math.max(1, Math.ceil(stored.byteLength / FILE_CHUNK));
		this.sql.exec("DELETE FROM app_files WHERE church_id = ? AND sha256 = ?", church, sha);
		for (let i = 0; i < chunks; i++)
			this.sql.exec(
				"INSERT INTO app_files (church_id, sha256, chunk, data) VALUES (?, ?, ?, ?)",
				church, sha, i, stored.slice(i * FILE_CHUNK, (i + 1) * FILE_CHUNK),
			);
		this.sql.exec(
			`INSERT OR REPLACE INTO app_file_info (church_id, sha256, chunks, bytes, stored_bytes, created_at)
			 VALUES (?, ?, ?, ?, ?, ?)`,
			church, sha, chunks, bytes, stored.byteLength, now(),
		);
		return json({ stored: sha, chunks });
	}

	private async getFile(request: Request): Promise<Response> {
		const church = request.headers.get("X-CGF-Church") ?? "";
		const sha = request.headers.get("X-CGF-Sha256") ?? "";
		if (request.headers.get("X-CGF-Publisher") !== "1") {
			const hash = request.headers.get("X-CGF-Token-Hash") ?? "";
			const device = this.sql
				.exec("SELECT 1 FROM devices WHERE church_id = ? AND token_sha256 = ?", church, hash)
				.toArray()[0];
			if (!device) return json({ error: "unauthorized" }, 401);
		}
		const info = this.sql
			.exec<{ chunks: number; bytes: number; stored_bytes: number }>(
				"SELECT chunks, bytes, stored_bytes FROM app_file_info WHERE church_id = ? AND sha256 = ?",
				church, sha,
			)
			.toArray()[0];
		if (!info) return json({ error: "not found" }, 404);
		const out = new Uint8Array(info.stored_bytes);
		let offset = 0;
		for (const row of this.sql.exec<{ data: ArrayBuffer }>(
			"SELECT data FROM app_files WHERE church_id = ? AND sha256 = ? ORDER BY chunk",
			church, sha,
		)) {
			const part = new Uint8Array(row.data);
			out.set(part, offset);
			offset += part.byteLength;
		}
		if (offset !== info.stored_bytes) return json({ error: "file is incomplete" }, 500);
		return new Response(out, {
			headers: { "Content-Type": "application/octet-stream", "X-CGF-Bytes": String(info.bytes) },
		});
	}

	// Store an app release whose files are all here, tell every laptop, and
	// delete files no recent release needs.
	async publishApp(church: string, release: AppReleaseRow) {
		const latest = this.latestApp(church);
		if (latest && release.seq <= latest.seq) return { error: `seq must be above ${latest.seq}` };
		const missing = await this.missingFiles(church, release.shas);
		if (missing.length) return { error: `${missing.length} files are not uploaded`, missing };
		this.sql.exec(
			"INSERT INTO app_releases (church_id, seq, folder, envelope, received_at) VALUES (?, ?, ?, ?, ?)",
			church, release.seq, release.folder, release.envelope, now(),
		);
		const keep = this.sql
			.exec<{ seq: number; envelope: string }>(
				"SELECT seq, envelope FROM app_releases WHERE church_id = ? ORDER BY seq DESC LIMIT ?",
				church, KEEP_APP_RELEASES,
			)
			.toArray();
		this.sql.exec("DELETE FROM app_releases WHERE church_id = ? AND seq < ?", church, keep[keep.length - 1].seq);
		const needed = new Set<string>();
		for (const k of keep) {
			const parsed = parseAppEnvelope(JSON.parse(k.envelope), church);
			if (typeof parsed !== "string") parsed.shas.forEach((s) => needed.add(s));
		}
		let removed = 0;
		for (const row of this.sql
			.exec<{ sha256: string }>("SELECT sha256 FROM app_file_info WHERE church_id = ?", church)
			.toArray())
			if (!needed.has(row.sha256)) {
				this.sql.exec("DELETE FROM app_files WHERE church_id = ? AND sha256 = ?", church, row.sha256);
				this.sql.exec("DELETE FROM app_file_info WHERE church_id = ? AND sha256 = ?", church, row.sha256);
				removed++;
			}
		const sockets = this.ctx.getWebSockets();
		for (const ws of sockets)
			try {
				ws.send(release.envelope);
			} catch {
				// it gets the release with its next hello
			}
		return { seq: release.seq, folder: release.folder, connected: sockets.length, removed_files: removed };
	}

	private latestApp(church: string) {
		return this.sql
			.exec<{ seq: number; folder: string; envelope: string; received_at: string }>(
				"SELECT seq, folder, envelope, received_at FROM app_releases WHERE church_id = ? ORDER BY seq DESC LIMIT 1",
				church,
			)
			.toArray()[0];
	}

	private setAppFolder(church: string, device: string, folder: string) {
		this.sql.exec("UPDATE devices SET app_folder = ? WHERE church_id = ? AND name = ?", folder, church, device);
	}

	private latest(church: string): number {
		const row = this.sql
			.exec<{ latest: number | null }>("SELECT MAX(to_version) AS latest FROM releases WHERE church_id = ?", church)
			.one();
		return row.latest ?? 0;
	}

	private touch(church: string, device: string, version: number | null) {
		this.sql.exec(
			"UPDATE devices SET last_seen_at = ?, last_version = COALESCE(?, last_version) WHERE church_id = ? AND name = ?",
			now(), version, church, device,
		);
	}

	private appFolder(church: string, device: string): string | null {
		const row = this.sql
			.exec<{ app_folder: string | null }>("SELECT app_folder FROM devices WHERE church_id = ? AND name = ?", church, device)
			.toArray()[0];
		return row?.app_folder ?? null;
	}

	// Send a laptop every release from its version onward, in order. If the
	// chain is broken (it was offline longer than the releases kept here), it
	// is told to take the full snapshot instead.
	private replay(ws: WebSocket, a: Attachment, version: number | null) {
		const latest = this.latest(a.church);
		ws.send(JSON.stringify({ type: "welcome", device: a.device, latest }));
		if (version === null || version >= latest) return;

		const rows = this.sql
			.exec<{ to_version: number; from_version: number; part: number; parts: number; envelope: string; needs: string }>(
				`SELECT to_version, from_version, part, parts, envelope, needs FROM releases
				 WHERE church_id = ? AND to_version > ? ORDER BY to_version, part`,
				a.church, version,
			)
			.toArray();
		const folder = this.appFolder(a.church, a.device);
		let current = version;
		let i = 0;
		let waiting = false;
		while (i < rows.length) {
			const to = rows[i].to_version;
			const group = [];
			while (i < rows.length && rows[i].to_version === to) group.push(rows[i++]);
			if (group[0].from_version !== current || group.length !== group[0].parts) break;
			// The laptop's app is too old for this release: hold it and all
			// after it until the app update is installed (no resync: the
			// releases are here, the laptop is not behind).
			if (!appSupports(folder, group[0].needs ? group[0].needs.split(",") : [])) {
				waiting = true;
				break;
			}
			for (const r of group) ws.send(r.envelope);
			current = to;
		}
		if (current < latest && !waiting) ws.send(JSON.stringify({ type: "resync", have: current, latest }));
	}
}
