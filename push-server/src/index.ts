// CGF Projector push server: tells church laptops about a new library release
// the moment it is published. One Durable Object per church holds the laptops'
// WebSocket connections, the recent releases (for laptops that were offline)
// and the device list. Protocol: docs/song-push.md.
//
// The server never sees the master library and never needs the signing key:
// releases arrive signed from the publisher's PC and the projector app checks
// the signature itself, so this server can only relay, never forge.

import { DurableObject } from "cloudflare:workers";
import { isInt, KEEP_RELEASES, parseEnvelope, type ReleaseRow } from "./release";

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

		// Everything else is the publisher's. A missing secret fails closed.
		const token = bearer(request);
		if (!env.PUBLISH_TOKEN || !token || !(await sameSecret(token, env.PUBLISH_TOKEN)))
			return json({ error: "unauthorized" }, 401);

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
			`);
		});
		// Keep-alives are answered without waking the object.
		ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
	}

	async fetch(request: Request): Promise<Response> {
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
		const version = isInt(m.version) && m.version >= 0 ? m.version : 0;
		if (m.type === "hello") {
			this.touch(a.church, a.device, null);
			this.replay(ws, a, version);
		} else if (m.type === "ack") {
			this.touch(a.church, a.device, version);
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
				`INSERT OR REPLACE INTO releases (church_id, to_version, part, parts, from_version, envelope, received_at)
				 VALUES (?, ?, ?, ?, ?, ?, ?)`,
				church, r.to, r.part, r.parts, r.from, r.envelope, received,
			);
		const latest = this.latest(church);
		this.sql.exec("DELETE FROM releases WHERE church_id = ? AND to_version <= ?", church, latest - KEEP_RELEASES);

		// A laptop at another version notices the gap and asks for a replay.
		const sockets = this.ctx.getWebSockets();
		for (const ws of sockets)
			for (const r of sorted)
				try {
					ws.send(r.envelope);
				} catch {
					// a closing connection; it catches up when it reconnects
				}
		return { stored: rows.length, latest, connected: sockets.length };
	}

	async createDevice(church: string, name: string) {
		const bytes = crypto.getRandomValues(new Uint8Array(32));
		const token = btoa(String.fromCharCode(...bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
		this.sql.exec(
			`INSERT INTO devices (church_id, name, token_sha256, created_at) VALUES (?, ?, ?, ?)
			 ON CONFLICT (church_id, name) DO UPDATE SET token_sha256 = excluded.token_sha256,
			   created_at = excluded.created_at, last_seen_at = NULL, last_version = NULL`,
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
			.exec<{ name: string; created_at: string; last_seen_at: string | null; last_version: number | null }>(
				"SELECT name, created_at, last_seen_at, last_version FROM devices WHERE church_id = ? ORDER BY name",
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
		return { church, latest: this.latest(church), ...r, devices };
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

	// Send a laptop every release from its version onward, in order. If the
	// chain is broken (it was offline longer than the releases kept here), it
	// is told to take the full snapshot instead.
	private replay(ws: WebSocket, a: Attachment, version: number) {
		const latest = this.latest(a.church);
		ws.send(JSON.stringify({ type: "welcome", device: a.device, latest }));
		if (version >= latest) return;

		const rows = this.sql
			.exec<{ to_version: number; from_version: number; part: number; parts: number; envelope: string }>(
				`SELECT to_version, from_version, part, parts, envelope FROM releases
				 WHERE church_id = ? AND to_version > ? ORDER BY to_version, part`,
				a.church, version,
			)
			.toArray();
		let current = version;
		let i = 0;
		while (i < rows.length) {
			const to = rows[i].to_version;
			const group = [];
			while (i < rows.length && rows[i].to_version === to) group.push(rows[i++]);
			if (group[0].from_version !== current || group.length !== group[0].parts) break;
			for (const r of group) ws.send(r.envelope);
			current = to;
		}
		if (current < latest) ws.send(JSON.stringify({ type: "resync", have: current, latest }));
	}
}
