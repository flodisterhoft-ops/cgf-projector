// Proposals: songs an operator added or changed on a church laptop, on their
// way to the master library (docs/proposals-up.md). The server stores them as
// laptop input, tells Florian on Telegram and keeps his Undo choice; the
// importer on his PC applies them to the master. Nothing here ever reaches
// another laptop.

export const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
export const MAX_PROPOSAL_CHARS = 64 * 1024;
export const PROPOSALS_PER_DAY = 100;
const LOCAL_BAND = 500000;

// The song fields a proposal carries. Fonts, colours and backgrounds stay on
// the laptop.
export const TEXT_FIELDS = ["title", "songbook_id", "songbook", "number", "song_text", "category", "tune", "words", "music", "notes"] as const;
type Fields = Partial<Record<(typeof TEXT_FIELDS)[number], string | number | null>>;

export interface Proposal {
	uuid: string;
	kind: "edit" | "new";
	song_id: number;
	base: Fields | null;
	proposed: Fields;
	created_at: string;
	user: string;
	library_version: number | null;
}

// Server states (set by Florian's tools): new → received → applied, or undone.
// The undo flag (the Telegram button, or `pull_proposals.py undo`) says what
// Florian wants; the importer makes the master match it.
export type State = "new" | "received" | "applied" | "undone" | "replaced";
export const STATES: State[] = ["new", "received", "applied", "undone", "replaced"];

// What the laptop is told: undone (put the library's text back), applied (the
// master has it; the next release of the song ends the laptop's own copy), or
// pending (keep the operator's text).
export type LaptopState = "pending" | "applied" | "undone";
export function laptopState(state: string, undo: number): LaptopState | null {
	if (state === "replaced") return null;
	if (undo) return "undone";
	return state === "applied" ? "applied" : "pending";
}

function fields(v: unknown): Fields | null {
	if (!v || typeof v !== "object" || Array.isArray(v)) return null;
	const out: Fields = {};
	for (const k of TEXT_FIELDS) {
		const x = (v as Record<string, unknown>)[k];
		if (x === undefined || x === null) continue;
		if (typeof x === "string" || (typeof x === "number" && Number.isFinite(x))) out[k] = x;
		else return null;
	}
	return out;
}

// A proposal as a laptop sends it, checked; or why it is refused.
export function parseProposal(m: Record<string, unknown>): Proposal | string {
	if (JSON.stringify(m).length > MAX_PROPOSAL_CHARS) return "too large";
	if (typeof m.uuid !== "string" || !UUID.test(m.uuid)) return "bad uuid";
	if (m.kind !== "edit" && m.kind !== "new") return "bad kind";
	const id = m.song_id;
	if (typeof id !== "number" || !Number.isInteger(id) || id < 1) return "bad song_id";
	if (m.kind === "edit" && id > LOCAL_BAND) return "an edit names a library song";
	if (m.kind === "new" && id <= LOCAL_BAND) return "a new song has a laptop id";
	const proposed = fields(m.proposed);
	if (!proposed || typeof proposed.title !== "string" || !proposed.title.trim()) return "no title";
	if (typeof proposed.song_text !== "string") return "no song text";
	const base = m.kind === "edit" ? fields(m.base) : null;
	if (m.kind === "edit" && !base) return "an edit needs its base";
	const user = typeof m.user === "string" ? m.user.slice(0, 60) : "";
	const created = typeof m.created_at === "string" ? m.created_at.slice(0, 40) : "";
	const lv = typeof m.library_version === "number" && Number.isInteger(m.library_version) ? m.library_version : null;
	return { uuid: m.uuid, kind: m.kind, song_id: id, base, proposed, created_at: created, user, library_version: lv };
}

function lines(text: unknown): string[] {
	return String(text ?? "")
		.replace(/\r\n?/g, "\n")
		.split("\n")
		.map((l) => l.trim())
		.filter((l) => l.length > 0);
}

// Lines added, removed and changed between two texts, by a longest common
// subsequence of their non-empty lines; a removed line followed by an added
// one counts as changed. Also the first changed pair, for the message.
export function lineDiff(a: unknown, b: unknown) {
	const x = lines(a);
	const y = lines(b);
	const n = x.length;
	const m = y.length;
	// Songs are short (a few hundred lines at most), so the table is small.
	const t: number[][] = Array.from({ length: n + 1 }, () => new Array<number>(m + 1).fill(0));
	for (let i = n - 1; i >= 0; i--)
		for (let j = m - 1; j >= 0; j--) t[i][j] = x[i] === y[j] ? t[i + 1][j + 1] + 1 : Math.max(t[i + 1][j], t[i][j + 1]);
	const ops: ["-" | "+", string][] = [];
	let i = 0;
	let j = 0;
	while (i < n || j < m) {
		if (i < n && j < m && x[i] === y[j]) {
			i++;
			j++;
		} else if (i < n && (j === m || t[i + 1][j] >= t[i][j + 1])) ops.push(["-", x[i++]]);
		else ops.push(["+", y[j++]]);
	}
	let added = 0;
	let removed = 0;
	let changed = 0;
	let first: [string, string] | null = null;
	for (let k = 0; k < ops.length; k++) {
		// Pair a run of removals with the additions right after it.
		if (ops[k][0] === "-") {
			let r = k;
			while (r < ops.length && ops[r][0] === "-") r++;
			let s = r;
			while (s < ops.length && ops[s][0] === "+") s++;
			const pairs = Math.min(r - k, s - r);
			if (pairs && !first) first = [ops[k][1], ops[r][1]];
			changed += pairs;
			removed += r - k - pairs;
			added += s - r - pairs;
			k = s - 1;
		} else added++;
	}
	return { added, removed, changed, first };
}

function clip(s: string, n = 80): string {
	const t = s.replace(/\s+/g, " ").trim();
	return t.length > n ? t.slice(0, n - 1) + "…" : t;
}

function plural(n: number, one: string, many: string): string {
	return `${n} ${n === 1 ? one : many}`;
}

function where(f: Fields): string {
	const book = typeof f.songbook === "string" && f.songbook ? f.songbook : "";
	const num = f.number !== undefined && f.number !== null && String(f.number) !== "0" ? ` ${f.number}` : "";
	return book ? ` (${book}${num})` : "";
}

// The Telegram text for one proposal.
export function describe(p: Proposal, device: string, again: boolean): string {
	const who = p.user ? `${device} (${p.user})` : device;
	const title = String(p.proposed.title ?? "");
	if (p.kind === "new") {
		const n = lines(p.proposed.song_text).length;
		return `${who}: new song "${clip(title)}"${where(p.proposed)}, ${plural(n, "line", "lines")}.`;
	}
	const base = p.base ?? {};
	const what: string[] = [];
	const oldTitle = String(base.title ?? "");
	if (oldTitle !== title) what.push(`title was "${clip(oldTitle, 60)}"`);
	const d = lineDiff(base.song_text, p.proposed.song_text);
	if (d.changed) what.push(plural(d.changed, "line changed", "lines changed"));
	if (d.added) what.push(plural(d.added, "line added", "lines added"));
	if (d.removed) what.push(plural(d.removed, "line removed", "lines removed"));
	for (const k of ["songbook_id", "number", "tune", "words", "music", "category", "notes"] as const)
		if (String(base[k] ?? "") !== String(p.proposed[k] ?? "")) what.push(`${k === "songbook_id" ? "songbook" : k} changed`);
	if (!what.length) what.push("no text change");
	let text = `${who}: changed "${clip(title)}"${where(p.proposed)}${again ? " again" : ""}: ${what.join(", ")}.`;
	if (d.first) text += `\n"${clip(d.first[0])}"\n→ "${clip(d.first[1])}"`;
	return text;
}

// The button under a message: Undo, or Keep after an undo.
export function keyboard(uuid: string, undo: number) {
	return {
		inline_keyboard: [[undo ? { text: "Keep the change", callback_data: `keep:${uuid}` } : { text: "Undo", callback_data: `undo:${uuid}` }]],
	};
}

export function stateLine(undo: number): string {
	return undo ? "\n\nUndone: the laptop shows the library's text again." : "";
}

// How long the server waits before telling Florian, so a song saved twice
// in a row makes one message.
export const NOTIFY_DELAY_MS = 30_000;

// Telegram's address and the fetch used to reach it; the tests replace both.
export const telegram = {
	api: "https://api.telegram.org",
	fetch: (url: string, init: RequestInit): Promise<Response> => fetch(url, init),
};
