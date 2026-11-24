// Release parts as the push server stores them (docs/song-push.md). Kept out of
// index.ts: the Workers runtime treats every export of the entry module as an
// entrypoint, so plain values and helpers must live here.

// Versions kept for laptops that were offline; older ones catch up from the
// full snapshot instead.
export const KEEP_RELEASES = 200;
// Durable Object SQLite rows hold at most 8 MB; the publisher splits releases
// into parts far below this.
const MAX_ENVELOPE_CHARS = 4 * 1024 * 1024;
const MAX_PARTS = 1000;

export interface ReleaseRow {
	to: number;
	from: number;
	part: number;
	parts: number;
	envelope: string;
}

export function isInt(v: unknown): v is number {
	return typeof v === "number" && Number.isSafeInteger(v);
}

// Checks one signed release part and returns what the server stores, or an
// error. The payload stays exactly as signed; only its header fields are read.
export function parseEnvelope(e: unknown, church: string): ReleaseRow | string {
	if (!e || typeof e !== "object") return "a part is not an object";
	const { type, payload, sig, key } = e as Record<string, unknown>;
	if (type !== "release" || typeof payload !== "string" || typeof sig !== "string" || typeof key !== "string")
		return "a part needs type, payload, sig and key";
	let p: Record<string, unknown>;
	try {
		p = JSON.parse(payload);
	} catch {
		return "a payload is not JSON";
	}
	if (p.kind !== "cgf-release" || p.v !== 1) return "unknown release format";
	if (p.church !== church) return "a release belongs to another church";
	const { from, to, part, parts } = p;
	if (!isInt(from) || !isInt(to) || from < 0 || to <= from) return "bad from/to versions";
	if (!isInt(parts) || !isInt(part) || parts < 1 || parts > MAX_PARTS || part < 1 || part > parts)
		return "bad part numbers";
	const envelope = JSON.stringify({ type: "release", payload, sig, key });
	if (envelope.length > MAX_ENVELOPE_CHARS) return "a part is too large";
	return { to, from, part, parts, envelope };
}
