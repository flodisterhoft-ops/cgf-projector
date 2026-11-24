import { env } from "cloudflare:workers";
import { evictDurableObject } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import worker, { type PushEnv } from "../src/index";
import { KEEP_RELEASES } from "../src/release";

const E = env as unknown as PushEnv;
const PUBLISHER = { Authorization: "Bearer test-publish-token", "Content-Type": "application/json" };

function call(path: string, init?: RequestInit): Promise<Response> {
	return worker.fetch(new Request("https://push.test" + path, init), E);
}

// The server reads only the header fields; signatures are the app's business.
function part(church: string, from: number, to: number, n = 1, of = 1, extra: object = {}) {
	return {
		type: "release",
		payload: JSON.stringify({ kind: "cgf-release", v: 1, church, from, to, part: n, parts: of, ...extra }),
		sig: "c2lnbmF0dXJl",
		key: "test",
	};
}

function publish(church: string, parts: unknown[]): Promise<Response> {
	return call(`/v1/${church}/releases`, { method: "POST", headers: PUBLISHER, body: JSON.stringify({ parts }) });
}

async function addDevice(church: string, name: string): Promise<string> {
	const r = await call(`/v1/${church}/devices`, { method: "POST", headers: PUBLISHER, body: JSON.stringify({ name }) });
	expect(r.status).toBe(200);
	return ((await r.json()) as { token: string }).token;
}

async function status(church: string) {
	const r = await call(`/v1/${church}/status`, { headers: PUBLISHER });
	expect(r.status).toBe(200);
	return (await r.json()) as {
		latest: number;
		versions: number;
		oldest_from: number | null;
		devices: { name: string; connected: boolean; last_version: number | null }[];
	};
}

function header(m: { payload: string }) {
	const p = JSON.parse(m.payload);
	return `${p.from}>${p.to}:${p.part}/${p.parts}`;
}

const pause = (ms = 50) => new Promise((r) => setTimeout(r, ms));

// A connected laptop that collects every message it receives.
async function connect(church: string, token: string) {
	const r = await call(`/v1/${church}/ws`, { headers: { Upgrade: "websocket", Authorization: `Bearer ${token}` } });
	expect(r.status).toBe(101);
	const ws = r.webSocket!;
	ws.accept();
	const inbox: any[] = [];
	const waiting: (() => void)[] = [];
	let closed = false;
	ws.addEventListener("message", (e) => {
		inbox.push(e.data === "pong" ? "pong" : JSON.parse(e.data as string));
		waiting.splice(0).forEach((w) => w());
	});
	ws.addEventListener("close", () => {
		closed = true;
		waiting.splice(0).forEach((w) => w());
	});
	return {
		ws,
		inbox,
		isClosed: () => closed,
		async next(n = 1) {
			while (inbox.length < n && !closed) await new Promise<void>((res) => waiting.push(res));
			return inbox.splice(0, n);
		},
		async closedSoon() {
			while (!closed) await new Promise<void>((res) => waiting.push(res));
		},
		send: (m: unknown) => ws.send(typeof m === "string" ? m : JSON.stringify(m)),
	};
}

describe("routes and access", () => {
	it("exports only the Worker and its Durable Object (the runtime rejects anything else)", async () => {
		const entry = await import("../src/index");
		expect(Object.keys(entry).sort()).toEqual(["ChurchPush", "default"]);
	});

	it("answers at the root and refuses unknown paths", async () => {
		expect((await call("/")).status).toBe(200);
		expect((await call("/v1/Bad%20Church/status", { headers: PUBLISHER })).status).toBe(404);
		expect((await call("/v1/routes/nothing", { headers: PUBLISHER })).status).toBe(404);
	});

	it("refuses publisher calls without the right token", async () => {
		const body = JSON.stringify({ parts: [part("access", 0, 1)] });
		expect((await call("/v1/access/releases", { method: "POST", body })).status).toBe(401);
		const wrong = { Authorization: "Bearer not-the-token" };
		expect((await call("/v1/access/releases", { method: "POST", headers: wrong, body })).status).toBe(401);
		expect((await call("/v1/access/status", { headers: wrong })).status).toBe(401);
	});

	it("refuses a laptop with a missing or unknown token", async () => {
		await addDevice("access", "Booth");
		expect((await call("/v1/access/ws", { headers: { Upgrade: "websocket" } })).status).toBe(401);
		const r = await call("/v1/access/ws", { headers: { Upgrade: "websocket", Authorization: "Bearer guess" } });
		expect(r.status).toBe(401);
		expect((await call("/v1/access/ws")).status).toBe(426);
	});

	it("refuses incomplete, foreign and malformed releases", async () => {
		expect((await publish("checks", [part("checks", 0, 1, 1, 2)])).status).toBe(400);
		expect((await publish("checks", [part("other", 0, 1)])).status).toBe(400);
		expect((await publish("checks", [part("checks", 2, 2)])).status).toBe(400);
		expect((await publish("checks", [{ type: "release", payload: "{", sig: "x", key: "k" }])).status).toBe(400);
		expect((await publish("checks", [])).status).toBe(400);
		expect((await status("checks")).latest).toBe(0);
	});
});

describe("releases reach laptops", () => {
	it("replays releases in order from the laptop's version", async () => {
		const c = "replay";
		const laptop = await connect(c, await addDevice(c, "Booth"));
		await publish(c, [part(c, 1, 2, 2, 2), part(c, 1, 2, 1, 2)]);
		await publish(c, [part(c, 2, 3)]);
		laptop.inbox.length = 0; // the live broadcasts; replays are what is tested here

		laptop.send({ type: "hello", version: 1 });
		const all = await laptop.next(4);
		expect(all[0]).toMatchObject({ type: "welcome", device: "Booth", latest: 3 });
		expect(all.slice(1).map(header)).toEqual(["1>2:1/2", "1>2:2/2", "2>3:1/1"]);

		laptop.send({ type: "hello", version: 2 });
		expect((await laptop.next(2)).map((m) => m.type ?? "")).toEqual(["welcome", "release"]);

		laptop.send({ type: "hello", version: 3 });
		expect(await laptop.next()).toMatchObject([{ type: "welcome", latest: 3 }]);
		await pause();
		expect(laptop.inbox).toEqual([]);
	});

	it("tells a laptop to take the snapshot when the chain is broken", async () => {
		const c = "gaps";
		const laptop = await connect(c, await addDevice(c, "Booth"));
		await publish(c, [part(c, 1, 2)]);
		await publish(c, [part(c, 3, 4)]); // 2>3 was never pushed
		laptop.inbox.length = 0;

		laptop.send({ type: "hello", version: 0 });
		expect(await laptop.next(2)).toMatchObject([{ type: "welcome" }, { type: "resync", have: 0, latest: 4 }]);

		laptop.send({ type: "hello", version: 1 });
		const m = await laptop.next(3);
		expect(m[1].type).toBe("release");
		expect(m[2]).toMatchObject({ type: "resync", have: 2, latest: 4 });
	});

	it("sends a new release to connected laptops at once and records their acks", async () => {
		const c = "live";
		const booth = await connect(c, await addDevice(c, "Booth"));
		const second = await connect(c, await addDevice(c, "Back room"));
		const r = await publish(c, [part(c, 0, 1, 1, 1, { songs: [{ id: 7, title: "Test" }] })]);
		expect(await r.json()).toMatchObject({ stored: 1, latest: 1, connected: 2 });
		for (const laptop of [booth, second]) {
			const [m] = await laptop.next();
			expect(m.type).toBe("release");
			expect(JSON.parse(m.payload).songs[0].title).toBe("Test");
		}
		booth.send({ type: "ack", version: 1 });
		await pause();
		const s = await status(c);
		expect(s.devices.find((d) => d.name === "Booth")).toMatchObject({ connected: true, last_version: 1 });
		expect(s.devices.find((d) => d.name === "Back room")?.last_version).toBeNull();
	});

	it("answers keep-alives", async () => {
		const laptop = await connect("pings", await addDevice("pings", "Booth"));
		laptop.send("ping");
		expect(await laptop.next()).toEqual(["pong"]);
	});

	it("keeps only the newest releases", async () => {
		const c = "retention";
		const parts = Array.from({ length: KEEP_RELEASES + 5 }, (_, i) => part(c, i, i + 1));
		expect((await publish(c, parts)).status).toBe(200);
		const s = await status(c);
		expect(s).toMatchObject({ latest: KEEP_RELEASES + 5, versions: KEEP_RELEASES, oldest_from: 5 });
	});

	it("keeps laptops connected while the object sleeps", async () => {
		const c = "sleep";
		const laptop = await connect(c, await addDevice(c, "Booth"));
		await publish(c, [part(c, 0, 1)]);
		laptop.inbox.length = 0;
		await evictDurableObject(E.CHURCH.getByName(c));
		laptop.send({ type: "hello", version: 0 });
		expect((await laptop.next(2)).map((m) => m.type)).toEqual(["welcome", "release"]);
	});
});

describe("devices", () => {
	it("removing a device closes its connection and locks it out", async () => {
		const c = "remove";
		const token = await addDevice(c, "Booth");
		const laptop = await connect(c, token);
		const r = await call(`/v1/${c}/devices/Booth`, { method: "DELETE", headers: PUBLISHER });
		expect(await r.json()).toEqual({ deleted: 1 });
		await laptop.closedSoon();
		const again = await call(`/v1/${c}/ws`, { headers: { Upgrade: "websocket", Authorization: `Bearer ${token}` } });
		expect(again.status).toBe(401);
	});

	it("a new token for the same device replaces the old one", async () => {
		const c = "rotate";
		const old = await addDevice(c, "Booth");
		const laptop = await connect(c, old);
		const fresh = await addDevice(c, "Booth");
		expect(fresh).not.toBe(old);
		await laptop.closedSoon();
		const r = await call(`/v1/${c}/ws`, { headers: { Upgrade: "websocket", Authorization: `Bearer ${old}` } });
		expect(r.status).toBe(401);
		await connect(c, fresh);
		expect((await status(c)).devices).toHaveLength(1);
	});

	it("refuses device names it cannot show", async () => {
		const r = await call("/v1/names/devices", { method: "POST", headers: PUBLISHER, body: JSON.stringify({ name: "<b>" }) });
		expect(r.status).toBe(400);
	});
});

// ---------------------------------------------------------------------------
// App releases

async function qCompress(data: Uint8Array): Promise<Uint8Array> {
	const z = new Uint8Array(await new Response(new Blob([data]).stream().pipeThrough(new CompressionStream("deflate"))).arrayBuffer());
	const out = new Uint8Array(4 + z.byteLength);
	new DataView(out.buffer).setUint32(0, data.byteLength);
	out.set(z, 4);
	return out;
}

async function hex(data: Uint8Array): Promise<string> {
	const d = await crypto.subtle.digest("SHA-256", data);
	return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function upload(church: string, text: string) {
	const data = new TextEncoder().encode(text);
	const sha = await hex(data);
	const r = await call(`/v1/${church}/app/files/${sha}`, { method: "PUT", headers: PUBLISHER, body: await qCompress(data) });
	return { sha, status: r.status, bytes: data.byteLength };
}

function appRelease(church: string, seq: number, folder: string, files: { path: string; sha256: string; bytes: number }[]) {
	return {
		type: "app",
		payload: JSON.stringify({ kind: "cgf-app", v: 1, church, seq, folder, version: folder.slice(4), files }),
		sig: "c2ln",
		key: "test",
	};
}

function publishApp(church: string, release: unknown) {
	return call(`/v1/${church}/app/releases`, { method: "POST", headers: PUBLISHER, body: JSON.stringify({ release }) });
}

describe("app releases", () => {
	it("checks an upload against its hash and reports which files are missing", async () => {
		const c = "app-upload";
		const good = await upload(c, "program file one");
		expect(good.status).toBe(200);
		const data = await qCompress(new TextEncoder().encode("something else"));
		const wrong = await call(`/v1/${c}/app/files/${"0".repeat(64)}`, { method: "PUT", headers: PUBLISHER, body: data });
		expect(wrong.status).toBe(400);
		const junk = await call(`/v1/${c}/app/files/${good.sha}`, { method: "PUT", headers: PUBLISHER, body: "not compressed" });
		expect(junk.status).toBe(400);
		const r = await call(`/v1/${c}/app/files/missing`, {
			method: "POST", headers: PUBLISHER, body: JSON.stringify({ sha256: [good.sha, "1".repeat(64)] }),
		});
		expect(await r.json()).toEqual({ missing: ["1".repeat(64)] });
	});

	it("refuses a release whose files are not all uploaded, and seqs that do not increase", async () => {
		const c = "app-refuse";
		const f = await upload(c, "exe bytes");
		const missing = await publishApp(c, appRelease(c, 1, "app-1.0.0", [
			{ path: "CGFProjector.exe", sha256: f.sha, bytes: f.bytes },
			{ path: "Qt6Core.dll", sha256: "2".repeat(64), bytes: 5 },
		]));
		expect(missing.status).toBe(400);
		expect(((await missing.json()) as { missing: string[] }).missing).toEqual(["2".repeat(64)]);
		expect((await publishApp(c, appRelease(c, 1, "app-1.0.0", [{ path: "CGFProjector.exe", sha256: f.sha, bytes: f.bytes }]))).status).toBe(200);
		expect((await publishApp(c, appRelease(c, 1, "app-1.0.1", [{ path: "CGFProjector.exe", sha256: f.sha, bytes: f.bytes }]))).status).toBe(400);
		const bad = [{ path: "../evil.exe", sha256: f.sha, bytes: 1 }];
		expect((await publishApp(c, appRelease(c, 2, "app-1.0.2", bad))).status).toBe(400);
		expect((await publishApp(c, appRelease(c, 2, "../app", [{ path: "a", sha256: f.sha, bytes: 1 }]))).status).toBe(400);
	});

	it("sends a new release to laptops, again after hello, and serves its files to them only", async () => {
		const c = "app-deliver";
		const token = await addDevice(c, "Booth");
		const laptop = await connect(c, token);
		const f = await upload(c, "the new program");
		const r = await publishApp(c, appRelease(c, 1, "app-2.0.0", [{ path: "CGFProjector.exe", sha256: f.sha, bytes: f.bytes }]));
		expect(await r.json()).toMatchObject({ seq: 1, folder: "app-2.0.0", connected: 1 });
		const [m] = await laptop.next();
		expect(m.type).toBe("app");
		expect(JSON.parse(m.payload).folder).toBe("app-2.0.0");

		// Without the projector app open the laptop sends no version: no song replay, just the app.
		laptop.send({ type: "hello", app: "app-1.0.0" });
		expect((await laptop.next(2)).map((x) => x.type)).toEqual(["welcome", "app"]);
		await pause();
		expect(laptop.inbox).toEqual([]);

		const file = await call(`/v1/${c}/app/files/${f.sha}`, { headers: { Authorization: `Bearer ${token}` } });
		expect(file.status).toBe(200);
		expect(file.headers.get("X-CGF-Bytes")).toBe(String(f.bytes));
		const body = new Uint8Array(await file.arrayBuffer());
		expect(body.byteLength).toBeGreaterThan(4);
		expect((await call(`/v1/${c}/app/files/${f.sha}`, { headers: { Authorization: "Bearer guess" } })).status).toBe(401);
		expect((await call(`/v1/${c}/app/files/${f.sha}`)).status).toBe(404);

		const s = await status(c);
		expect(s.devices[0]).toMatchObject({ name: "Booth" });
		expect((s.devices[0] as unknown as { app_folder: string }).app_folder).toBe("app-1.0.0");
	});

	it("keeps the files of the last releases and deletes older ones", async () => {
		const c = "app-cleanup";
		const files = [];
		for (let i = 1; i <= 4; i++) {
			const f = await upload(c, `build ${i}`);
			files.push(f);
			const r = await publishApp(c, appRelease(c, i, `app-0.${i}`, [{ path: "CGFProjector.exe", sha256: f.sha, bytes: f.bytes }]));
			expect(r.status).toBe(200);
		}
		const r = await call(`/v1/${c}/app/files/missing`, {
			method: "POST", headers: PUBLISHER, body: JSON.stringify({ sha256: files.map((f) => f.sha) }),
		});
		expect(await r.json()).toEqual({ missing: [files[0].sha] });
		const s = (await (await call(`/v1/${c}/status`, { headers: PUBLISHER })).json()) as { app: { seq: number } };
		expect(s.app.seq).toBe(4);
	});
});
