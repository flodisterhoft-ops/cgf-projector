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

// ---------------------------------------------------------------------------
// App releases: a signed list of every file of one app version (its folder,
// app-<version>), which laptops download by content hash.

export const KEEP_APP_RELEASES = 3; // their files are kept; older files are deleted
export const FILE_CHUNK = 1024 * 1024;
export const SHA256 = /^[0-9a-f]{64}$/;
const APP_FOLDER = /^app-[A-Za-z0-9._-]{1,40}$/;
const MAX_FILES = 5000;

export interface AppReleaseRow {
	seq: number;
	folder: string;
	envelope: string;
	shas: string[];
}

function safePath(p: unknown): p is string {
	return (
		typeof p === "string" && p.length > 0 && p.length <= 260 && !p.startsWith("/") && !p.includes("\\") &&
		!p.includes(":") && !p.split("/").some((s) => s === "" || s === "." || s === "..")
	);
}

export function parseAppEnvelope(e: unknown, church: string): AppReleaseRow | string {
	if (!e || typeof e !== "object") return "the release is not an object";
	const { type, payload, sig, key } = e as Record<string, unknown>;
	if (type !== "app" || typeof payload !== "string" || typeof sig !== "string" || typeof key !== "string")
		return "an app release needs type, payload, sig and key";
	let p: Record<string, unknown>;
	try {
		p = JSON.parse(payload);
	} catch {
		return "the payload is not JSON";
	}
	if (p.kind !== "cgf-app" || p.v !== 1) return "unknown app release format";
	if (p.church !== church) return "the release belongs to another church";
	if (!isInt(p.seq) || p.seq < 1) return "bad seq";
	if (typeof p.folder !== "string" || !APP_FOLDER.test(p.folder)) return "bad folder name";
	if (!Array.isArray(p.files) || p.files.length === 0 || p.files.length > MAX_FILES) return "bad file list";
	const shas = new Set<string>();
	const paths = new Set<string>();
	for (const f of p.files as Record<string, unknown>[]) {
		if (!f || !safePath(f.path) || typeof f.sha256 !== "string" || !SHA256.test(f.sha256) || !isInt(f.bytes) || f.bytes < 0)
			return "bad file entry";
		const lower = f.path.toLowerCase();
		if (paths.has(lower)) return `${f.path} is listed twice`;
		paths.add(lower);
		shas.add(f.sha256);
	}
	if (p.launcher !== undefined) {
		const l = p.launcher as Record<string, unknown>;
		if (!l || typeof l.sha256 !== "string" || !SHA256.test(l.sha256) || !isInt(l.bytes)) return "bad launcher entry";
		shas.add(l.sha256);
	}
	return { seq: p.seq, folder: p.folder, envelope: JSON.stringify({ type: "app", payload, sig, key }), shas: [...shas] };
}

// Files travel compressed as Qt's qCompress writes them: the original size
// (4 bytes, big-endian) and a zlib stream. Returns the original bytes.
export async function inflateQt(stored: ArrayBuffer): Promise<Uint8Array | null> {
	if (stored.byteLength < 4) return null;
	const view = new DataView(stored);
	const expected = view.getUint32(0);
	try {
		const stream = new Blob([stored.slice(4)]).stream().pipeThrough(new DecompressionStream("deflate"));
		const out = new Uint8Array(await new Response(stream).arrayBuffer());
		return out.byteLength === expected ? out : null;
	} catch {
		return null;
	}
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
