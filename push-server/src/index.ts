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
import {
	describe as describeProposal,
	keyboard,
	laptopState,
	NOTIFY_DELAY_MS,
	parseProposal,
	type Proposal,
	PROPOSALS_PER_DAY,
	stateLine,
	STATES,
	telegram,
	UUID,
} from "./proposals";

export interface PushEnv {
	CHURCH: DurableObjectNamespace<ChurchPush>;
	PUBLISH_TOKEN?: string;
	// Telegram (docs/proposals-up.md). Secrets, set by tools/telegram_setup.py.
	// Without them proposals are stored and nobody is told.
	TELEGRAM_BOT_TOKEN?: string;
	TELEGRAM_CHAT_ID?: string;
	TELEGRAM_WEBHOOK_SECRET?: string;
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

		// Telegram's calls when Florian presses Undo or Keep under a message.
		if (route.length === 1 && route[0] === "telegram" && request.method === "POST") {
			const given = request.headers.get("X-Telegram-Bot-Api-Secret-Token") ?? "";
			if (!env.TELEGRAM_WEBHOOK_SECRET || !given || !(await sameSecret(given, env.TELEGRAM_WEBHOOK_SECRET)))
				return json({ error: "unauthorized" }, 401);
			let update: Record<string, unknown>;
			try {
				update = await request.json();
			} catch {
				return json({ ok: true });
			}
			await stub.telegramUpdate(church, update);
			return json({ ok: true });
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
		if (route.length === 1 && route[0] === "proposals" && request.method === "GET") {
			const state = url.searchParams.get("state");
			return json({ proposals: await stub.listProposals(church, state) });
		}
		if (route.length === 2 && route[0] === "proposals" && request.method === "POST") {
			if (!UUID.test(route[1])) return json({ error: "bad uuid" }, 400);
			let body: { state?: unknown; undo?: unknown; note?: unknown };
			try {
				body = await request.json();
			} catch {
				return json({ error: "body is not JSON" }, 400);
			}
			const state = typeof body.state === "string" ? body.state : null;
			if (state !== null && !STATES.includes(state as never)) return json({ error: "bad state" }, 400);
			const undo = typeof body.undo === "boolean" ? body.undo : null;
			const note = typeof body.note === "string" ? body.note.slice(0, 500) : null;
			const result = await stub.setProposal(church, route[1], state, undo, note);
			return json(result, "error" in result ? 404 : 200);
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
			// Added with proposals (2026-11-28): operators' songs on their way to the master.
			this.sql.exec(`
				CREATE TABLE IF NOT EXISTS proposals (
					church_id TEXT NOT NULL,
					uuid TEXT NOT NULL,
					device TEXT NOT NULL,
					kind TEXT NOT NULL,
					song_id INTEGER NOT NULL,
					title TEXT NOT NULL,
					payload TEXT NOT NULL,
					received_at TEXT NOT NULL,
					updated_at TEXT NOT NULL,
					state TEXT NOT NULL DEFAULT 'new',
					undo INTEGER NOT NULL DEFAULT 0,
					note TEXT,
					notified INTEGER NOT NULL DEFAULT 0,
					notify_tries INTEGER NOT NULL DEFAULT 0,
					tg_message_id INTEGER,
					tg_text TEXT,
					laptop_state TEXT,
					PRIMARY KEY (church_id, uuid)
				);
				CREATE INDEX IF NOT EXISTS proposals_device ON proposals (church_id, device, song_id);
			`);
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
			// Only a laptop with the app open can act on Florian's choices.
			if (version !== null) this.sendStates(ws, a);
		} else if (m.type === "proposal") {
			ws.send(JSON.stringify(await this.storeProposal(a, m)));
		} else if (m.type === "proposalStateAck" && typeof m.uuid === "string" && typeof m.state === "string") {
			this.sql.exec(
				"UPDATE proposals SET laptop_state = ? WHERE church_id = ? AND uuid = ? AND device = ?",
				m.state.slice(0, 20), a.church, m.uuid, a.device,
			);
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

	// ----------------------------------------------------------------------
	// Proposals (docs/proposals-up.md)

	private async storeProposal(a: Attachment, m: Record<string, unknown>) {
		const uuid = typeof m.uuid === "string" ? m.uuid : "";
		const p = parseProposal(m);
		if (typeof p === "string") return { type: "proposalStored", uuid, status: "rejected", reason: p };
		const have = this.sql
			.exec("SELECT 1 FROM proposals WHERE church_id = ? AND uuid = ?", a.church, p.uuid)
			.toArray()[0];
		if (have) return { type: "proposalStored", uuid, status: "duplicate" };
		const since = new Date(Date.now() - 24 * 3600 * 1000).toISOString();
		const today = this.sql
			.exec<{ n: number }>(
				"SELECT COUNT(*) AS n FROM proposals WHERE church_id = ? AND device = ? AND received_at > ?",
				a.church, a.device, since,
			)
			.one().n;
		// Refused for now, sent again later: the laptop keeps it queued.
		if (today >= PROPOSALS_PER_DAY) return { type: "proposalStored", uuid, status: "later", reason: "too many today" };

		// A change to the same song that nobody was told about yet replaces the
		// earlier one, so Florian gets one message for one fix.
		const t = now();
		this.sql.exec(
			`UPDATE proposals SET state = 'replaced', notified = 1, updated_at = ?
			 WHERE church_id = ? AND device = ? AND song_id = ? AND state = 'new' AND notified = 0`,
			t, a.church, a.device, p.song_id,
		);
		this.sql.exec(
			`INSERT INTO proposals (church_id, uuid, device, kind, song_id, title, payload, received_at, updated_at)
			 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			a.church, p.uuid, a.device, p.kind, p.song_id, String(p.proposed.title ?? ""), JSON.stringify(p), t, t,
		);
		await this.scheduleNotify(NOTIFY_DELAY_MS);
		return { type: "proposalStored", uuid, status: "stored" };
	}

	private async scheduleNotify(ms: number) {
		const at = await this.ctx.storage.getAlarm();
		const want = Date.now() + ms;
		if (at === null || at > want) await this.ctx.storage.setAlarm(want);
	}

	private telegramReady(): boolean {
		return !!this.env.TELEGRAM_BOT_TOKEN && !!this.env.TELEGRAM_CHAT_ID;
	}

	private async telegramCall(method: string, body: object): Promise<Record<string, unknown> | null> {
		try {
			const r = await telegram.fetch(`${telegram.api}/bot${this.env.TELEGRAM_BOT_TOKEN}/${method}`, {
				method: "POST",
				headers: { "Content-Type": "application/json" },
				body: JSON.stringify(body),
			});
			const out = (await r.json()) as Record<string, unknown>;
			// Never log the address: it holds the token.
			if (!out.ok) console.error(`Telegram ${method} failed`, out.description);
			return out.ok ? out : null;
		} catch (e) {
			console.error(`Telegram ${method} failed`, String(e));
			return null;
		}
	}

	// Tell Florian about proposals nobody has told him about. A failed send is
	// tried again by a later alarm, at most ten times.
	async alarm(): Promise<void> {
		if (!this.telegramReady()) return;
		const rows = this.sql
			.exec<{ church_id: string; uuid: string; device: string; song_id: number; payload: string; received_at: string }>(
				`SELECT church_id, uuid, device, song_id, payload, received_at FROM proposals
				 WHERE notified = 0 AND notify_tries < 10 ORDER BY received_at LIMIT 20`,
			)
			.toArray();
		let failed = false;
		for (const r of rows) {
			const p = JSON.parse(r.payload) as Proposal;
			const again =
				this.sql
					.exec(
						`SELECT 1 FROM proposals WHERE church_id = ? AND device = ? AND song_id = ? AND uuid <> ?
						 AND state <> 'replaced' AND received_at <= ?`,
						r.church_id, r.device, r.song_id, r.uuid, r.received_at,
					)
					.toArray().length > 0;
			const text = describeProposal(p, r.device, again);
			const sent = await this.telegramCall("sendMessage", {
				chat_id: this.env.TELEGRAM_CHAT_ID,
				text,
				reply_markup: keyboard(r.uuid, 0),
			});
			const id = (sent?.result as { message_id?: number } | undefined)?.message_id ?? null;
			if (sent)
				this.sql.exec(
					"UPDATE proposals SET notified = 1, tg_message_id = ?, tg_text = ? WHERE church_id = ? AND uuid = ?",
					id, text, r.church_id, r.uuid,
				);
			else {
				failed = true;
				this.sql.exec("UPDATE proposals SET notify_tries = notify_tries + 1 WHERE church_id = ? AND uuid = ?", r.church_id, r.uuid);
			}
		}
		const more = this.sql.exec("SELECT 1 FROM proposals WHERE notified = 0 AND notify_tries < 10 LIMIT 1").toArray().length > 0;
		if (more) await this.ctx.storage.setAlarm(Date.now() + (failed ? 5 * 60_000 : 1_000));
	}

	// Undo or Keep, pressed under a message in Florian's chat only.
	async telegramUpdate(church: string, update: Record<string, unknown>) {
		const q = update.callback_query as
			| { id?: string; data?: string; message?: { chat?: { id?: number | string }; message_id?: number } }
			| undefined;
		if (!q || typeof q.data !== "string") return;
		const chat = String(q.message?.chat?.id ?? "");
		if (!this.env.TELEGRAM_CHAT_ID || chat !== String(this.env.TELEGRAM_CHAT_ID)) return;
		const m = /^(undo|keep):(.+)$/.exec(q.data);
		if (!m || !UUID.test(m[2])) return;
		const r = await this.setProposal(church, m[2], null, m[1] === "undo", null);
		await this.telegramCall("answerCallbackQuery", {
			callback_query_id: q.id,
			text: "error" in r ? "That change is no longer here." : m[1] === "undo" ? "Undone" : "Kept",
		});
	}

	// Florian's tools and the Telegram buttons: a new state, the undo flag, a note.
	async setProposal(church: string, uuid: string, state: string | null, undo: boolean | null, note: string | null) {
		const row = this.proposalRow(church, uuid);
		if (!row) return { error: "no such proposal" };
		this.sql.exec(
			`UPDATE proposals SET state = COALESCE(?, state), undo = COALESCE(?, undo), note = COALESCE(?, note), updated_at = ?
			 WHERE church_id = ? AND uuid = ?`,
			state, undo === null ? null : undo ? 1 : 0, note, now(), church, uuid,
		);
		const after = this.proposalRow(church, uuid)!;
		if (after.undo !== row.undo && row.tg_message_id && this.telegramReady())
			await this.telegramCall("editMessageText", {
				chat_id: this.env.TELEGRAM_CHAT_ID,
				message_id: row.tg_message_id,
				text: (row.tg_text ?? "") + stateLine(after.undo),
				reply_markup: keyboard(uuid, after.undo),
			});
		for (const ws of this.ctx.getWebSockets(after.device)) this.sendStates(ws, { church, device: after.device });
		return this.publicRow(after);
	}

	private proposalRow(church: string, uuid: string) {
		return this.sql
			.exec<{
				uuid: string; device: string; kind: string; song_id: number; title: string; payload: string;
				received_at: string; updated_at: string; state: string; undo: number; note: string | null;
				notified: number; laptop_state: string | null; tg_message_id: number | null; tg_text: string | null;
			}>(
				`SELECT uuid, device, kind, song_id, title, payload, received_at, updated_at, state, undo, note, notified,
				        laptop_state, tg_message_id, tg_text
				 FROM proposals WHERE church_id = ? AND uuid = ?`,
				church, uuid,
			)
			.toArray()[0];
	}

	private publicRow(r: NonNullable<ReturnType<ChurchPush["proposalRow"]>>) {
		const { payload, tg_message_id, tg_text, ...rest } = r;
		return { ...rest, undo: !!r.undo, notified: !!r.notified, proposal: JSON.parse(payload) };
	}

	async listProposals(church: string, state: string | null) {
		const uuids = this.sql
			.exec<{ uuid: string }>(
				state
					? "SELECT uuid FROM proposals WHERE church_id = ? AND state = ? ORDER BY received_at"
					: "SELECT uuid FROM proposals WHERE church_id = ? ORDER BY received_at",
				...(state ? [church, state] : [church]),
			)
			.toArray();
		return uuids.map((u) => this.publicRow(this.proposalRow(church, u.uuid)!));
	}

	// Tell a laptop what became of its proposals, when that changed since it last heard.
	private sendStates(ws: WebSocket, a: Attachment) {
		const rows = this.sql
			.exec<{ uuid: string; song_id: number; state: string; undo: number; laptop_state: string | null }>(
				"SELECT uuid, song_id, state, undo, laptop_state FROM proposals WHERE church_id = ? AND device = ? ORDER BY received_at",
				a.church, a.device,
			)
			.toArray();
		for (const r of rows) {
			const s = laptopState(r.state, r.undo);
			if (s === null || s === (r.laptop_state ?? "pending")) continue;
			try {
				ws.send(JSON.stringify({ type: "proposalState", uuid: r.uuid, song_id: r.song_id, state: s }));
			} catch {
				// it hears with its next hello
			}
		}
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
