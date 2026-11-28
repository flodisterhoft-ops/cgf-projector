import { env } from "cloudflare:workers";
import { runDurableObjectAlarm } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import worker, { type PushEnv } from "../src/index";
import { describe as describeProposal, lineDiff, PROPOSALS_PER_DAY, telegram } from "../src/proposals";

const E = env as unknown as PushEnv;
const PUBLISHER = { Authorization: "Bearer test-publish-token", "Content-Type": "application/json" };
const HOOK = { "X-Telegram-Bot-Api-Secret-Token": "test-hook-secret", "Content-Type": "application/json" };

function call(path: string, init?: RequestInit): Promise<Response> {
	return worker.fetch(new Request("https://push.test" + path, init), E);
}

async function addDevice(church: string, name: string): Promise<string> {
	const r = await call(`/v1/${church}/devices`, { method: "POST", headers: PUBLISHER, body: JSON.stringify({ name }) });
	return ((await r.json()) as { token: string }).token;
}

async function connect(church: string, token: string) {
	const r = await call(`/v1/${church}/ws`, { headers: { Upgrade: "websocket", Authorization: `Bearer ${token}` } });
	expect(r.status).toBe(101);
	const ws = r.webSocket!;
	ws.accept();
	const inbox: any[] = [];
	const waiting: (() => void)[] = [];
	ws.addEventListener("message", (e) => {
		inbox.push(JSON.parse(e.data as string));
		waiting.splice(0).forEach((w) => w());
	});
	return {
		inbox,
		async next(n = 1) {
			while (inbox.length < n) await new Promise<void>((res) => waiting.push(res));
			return inbox.splice(0, n);
		},
		send: (m: unknown) => ws.send(JSON.stringify(m)),
	};
}

const pause = (ms = 50) => new Promise((r) => setTimeout(r, ms));

let n = 0;
function uuid(): string {
	n++;
	return `00000000-0000-4000-8000-${String(n).padStart(12, "0")}`;
}

function edit(song_id = 7, extra: object = {}) {
	return {
		type: "proposal",
		uuid: uuid(),
		kind: "edit",
		song_id,
		base: { title: "Amazing Grace", songbook: "Hymns", number: 12, song_text: "Verse 1\nAmazing grace how sweet the sond\nThat saved a wretch" },
		proposed: { title: "Amazing Grace", songbook: "Hymns", number: 12, song_text: "Verse 1\nAmazing grace how sweet the sound\nThat saved a wretch\nlike me" },
		created_at: "2026-11-28T10:00:00Z",
		user: "Operator",
		library_version: 4,
		...extra,
	};
}

// A fake Telegram that records every call.
let calls: { method: string; body: any }[] = [];
let telegramUp = true;
const realFetch = telegram.fetch;
beforeEach(() => {
	calls = [];
	telegramUp = true;
	telegram.fetch = async (url: string, init: RequestInit) => {
		const method = url.split("/").pop()!;
		expect(url).toContain("/bottest-bot-token/");
		if (!telegramUp) return new Response(JSON.stringify({ ok: false, description: "down" }));
		calls.push({ method, body: JSON.parse(String(init.body)) });
		return new Response(JSON.stringify({ ok: true, result: { message_id: 100 + calls.length } }));
	};
});
afterEach(() => {
	telegram.fetch = realFetch;
});

async function list(church: string, state?: string) {
	const r = await call(`/v1/${church}/proposals${state ? `?state=${state}` : ""}`, { headers: PUBLISHER });
	expect(r.status).toBe(200);
	return ((await r.json()) as { proposals: any[] }).proposals;
}

function setProposal(church: string, id: string, body: object) {
	return call(`/v1/${church}/proposals/${id}`, { method: "POST", headers: PUBLISHER, body: JSON.stringify(body) });
}

function press(church: string, data: string, chat = 42) {
	return call(`/v1/${church}/telegram`, {
		method: "POST",
		headers: HOOK,
		body: JSON.stringify({ update_id: 1, callback_query: { id: "cb1", data, message: { chat: { id: chat }, message_id: 101 } } }),
	});
}

describe("proposals from laptops", () => {
	it("stores a proposal once and answers the laptop", async () => {
		const c = "prop-store";
		const laptop = await connect(c, await addDevice(c, "Church laptop"));
		const p = edit();
		laptop.send(p);
		expect(await laptop.next()).toMatchObject([{ type: "proposalStored", uuid: p.uuid, status: "stored" }]);
		laptop.send(p);
		expect(await laptop.next()).toMatchObject([{ status: "duplicate" }]);
		const rows = await list(c);
		expect(rows).toHaveLength(1);
		expect(rows[0]).toMatchObject({ uuid: p.uuid, device: "Church laptop", kind: "edit", song_id: 7, state: "new", undo: false });
		expect(rows[0].proposal.proposed.song_text).toContain("sound");
	});

	it("refuses malformed proposals so the laptop stops sending them", async () => {
		const c = "prop-bad";
		const laptop = await connect(c, await addDevice(c, "Booth"));
		const bad = [
			edit(7, { uuid: "not-a-uuid" }),
			edit(600001), // an edit of a laptop's own song
			{ ...edit(), kind: "new", song_id: 7, base: null },
			edit(7, { base: null }),
			edit(7, { proposed: { song_text: "x" } }),
			edit(7, { proposed: { title: "T", song_text: "x".repeat(70000) } }),
		];
		for (const b of bad) laptop.send(b);
		const answers = await laptop.next(bad.length);
		expect(answers.map((a) => a.status)).toEqual(bad.map(() => "rejected"));
		expect(await list(c)).toHaveLength(0);
	});

	it("takes at most a day's worth from one laptop and asks for the rest later", async () => {
		const c = "prop-limit";
		const laptop = await connect(c, await addDevice(c, "Booth"));
		for (let i = 0; i < PROPOSALS_PER_DAY; i++) laptop.send(edit(i + 1));
		const ok = await laptop.next(PROPOSALS_PER_DAY);
		expect(ok.every((a) => a.status === "stored")).toBe(true);
		laptop.send(edit(999));
		expect(await laptop.next()).toMatchObject([{ status: "later" }]);
	});

	it("needs the publisher token to read or change proposals", async () => {
		expect((await call("/v1/prop-auth/proposals")).status).toBe(401);
		const id = uuid();
		expect((await call(`/v1/prop-auth/proposals/${id}`, { method: "POST", body: "{}" })).status).toBe(401);
		expect((await setProposal("prop-auth", id, { state: "applied" })).status).toBe(404);
		expect((await setProposal("prop-auth", id, { state: "eaten" })).status).toBe(400);
	});
});

describe("Telegram", () => {
	it("tells Florian once, with an Undo button, after a short wait", async () => {
		const c = "tg-notify";
		const laptop = await connect(c, await addDevice(c, "Church laptop"));
		const first = edit();
		const second = edit(); // saved again before the message went out
		laptop.send(first);
		laptop.send(second);
		await laptop.next(2);
		expect(calls).toHaveLength(0);
		expect(await runDurableObjectAlarm(E.CHURCH.getByName(c))).toBe(true);
		expect(calls).toHaveLength(1);
		const msg = calls[0];
		expect(msg.method).toBe("sendMessage");
		expect(msg.body.chat_id).toBe("42");
		expect(msg.body.text).toContain('Church laptop (Operator): changed "Amazing Grace" (Hymns 12)');
		expect(msg.body.text).toContain("1 line changed, 1 line added");
		expect(msg.body.reply_markup.inline_keyboard[0][0]).toEqual({ text: "Undo", callback_data: `undo:${second.uuid}` });
		const rows = await list(c);
		expect(rows.map((r) => r.state)).toEqual(["replaced", "new"]);
		expect(rows[1].notified).toBe(true);
	});

	it("tries again later when Telegram is down", async () => {
		const c = "tg-retry";
		const laptop = await connect(c, await addDevice(c, "Booth"));
		laptop.send(edit());
		await laptop.next();
		telegramUp = false;
		await runDurableObjectAlarm(E.CHURCH.getByName(c));
		expect((await list(c))[0].notified).toBe(false);
		telegramUp = true;
		expect(await runDurableObjectAlarm(E.CHURCH.getByName(c))).toBe(true);
		expect(calls.map((x) => x.method)).toEqual(["sendMessage"]);
		expect((await list(c))[0].notified).toBe(true);
	});

	it("Undo and Keep flip the flag, the button and what the laptop is told", async () => {
		const c = "tg-undo";
		const laptop = await connect(c, await addDevice(c, "Church laptop"));
		laptop.send({ type: "hello", version: 4 });
		await laptop.next(); // welcome
		const p = edit();
		laptop.send(p);
		await laptop.next();
		await runDurableObjectAlarm(E.CHURCH.getByName(c));

		expect((await press(c, `undo:${p.uuid}`)).status).toBe(200);
		expect((await list(c))[0].undo).toBe(true);
		const edited = calls.find((x) => x.method === "editMessageText")!;
		expect(edited.body.message_id).toBe(101);
		expect(edited.body.text).toContain("Undone");
		expect(edited.body.reply_markup.inline_keyboard[0][0].callback_data).toBe(`keep:${p.uuid}`);
		expect(calls.find((x) => x.method === "answerCallbackQuery")!.body.text).toBe("Undone");
		expect(await laptop.next()).toMatchObject([{ type: "proposalState", uuid: p.uuid, song_id: 7, state: "undone" }]);
		laptop.send({ type: "proposalStateAck", uuid: p.uuid, state: "undone" });
		await pause();

		await press(c, `keep:${p.uuid}`);
		expect((await list(c))[0].undo).toBe(false);
		expect(await laptop.next()).toMatchObject([{ type: "proposalState", state: "pending" }]);
	});

	it("ignores buttons from other chats and calls without the webhook secret", async () => {
		const c = "tg-guard";
		const laptop = await connect(c, await addDevice(c, "Booth"));
		const p = edit();
		laptop.send(p);
		await laptop.next();
		await press(c, `undo:${p.uuid}`, 999);
		expect((await list(c))[0].undo).toBe(false);
		const r = await call(`/v1/${c}/telegram`, {
			method: "POST",
			headers: { "X-Telegram-Bot-Api-Secret-Token": "guess" },
			body: JSON.stringify({ callback_query: { data: `undo:${p.uuid}`, message: { chat: { id: 42 } } } }),
		});
		expect(r.status).toBe(401);
		expect((await list(c))[0].undo).toBe(false);
	});
});

describe("Florian's tools and the laptop", () => {
	it("an applied proposal is reported to the laptop at its next hello, once", async () => {
		const c = "prop-applied";
		const token = await addDevice(c, "Church laptop");
		let laptop = await connect(c, token);
		const p = edit();
		laptop.send(p);
		await laptop.next();

		const r = await setProposal(c, p.uuid, { state: "received" });
		expect(r.status).toBe(200);
		expect(laptop.inbox).toHaveLength(0); // received changes nothing on the laptop
		await setProposal(c, p.uuid, { state: "applied", note: "master song 123" });
		expect(await laptop.next()).toMatchObject([{ type: "proposalState", state: "applied" }]);

		laptop = await connect(c, token);
		laptop.send({ type: "hello", version: 4 });
		const again = await laptop.next(2);
		expect(again.map((m) => m.type)).toEqual(["welcome", "proposalState"]);
		laptop.send({ type: "proposalStateAck", uuid: p.uuid, state: "applied" });
		await pause();
		laptop.send({ type: "hello", version: 4 });
		await laptop.next();
		await pause();
		expect(laptop.inbox).toHaveLength(0);
		expect((await list(c, "applied"))[0]).toMatchObject({ note: "master song 123", laptop_state: "applied" });
	});

	it("a companion without the app open is not told (it could not act on it)", async () => {
		const c = "prop-closed";
		const laptop = await connect(c, await addDevice(c, "Booth"));
		const p = edit();
		laptop.send(p);
		await laptop.next();
		await setProposal(c, p.uuid, { undo: true });
		await laptop.next(); // live, to the open connection
		laptop.send({ type: "hello" }); // the app closed: no version
		await laptop.next();
		await pause();
		expect(laptop.inbox).toHaveLength(0);
	});
});

describe("the message text", () => {
	it("counts changed, added and removed lines", () => {
		expect(lineDiff("a\nb\nc", "a\nB\nc\nd")).toMatchObject({ changed: 1, added: 1, removed: 0, first: ["b", "B"] });
		expect(lineDiff("a\nb\n\nc", "a\nc")).toMatchObject({ changed: 0, added: 0, removed: 1 });
		expect(lineDiff("a", "a")).toMatchObject({ changed: 0, added: 0, removed: 0, first: null });
	});

	it("describes a new song and a retitle", () => {
		const fresh = describeProposal(
			{ uuid: uuid(), kind: "new", song_id: 500003, base: null, proposed: { title: "Wie groß bist du", songbook: "Lieder", song_text: "a\nb" }, created_at: "", user: "", library_version: 4, replaces: null },
			"Church laptop",
			false,
		);
		expect(fresh).toBe('Church laptop: new song "Wie groß bist du" (Lieder), 2 lines.');
		const retitle = describeProposal(
			{ uuid: uuid(), kind: "edit", song_id: 9, base: { title: "Old", song_text: "x" }, proposed: { title: "New", song_text: "x" }, created_at: "", user: "", library_version: 4, replaces: null },
			"Booth",
			true,
		);
		expect(retitle).toBe('Booth: changed "New" again: title was "Old".');
	});
});
