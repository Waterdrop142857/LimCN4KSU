// Vendoring helper: downloads pinned upstream sources through the GitHub
// contents API into third_party/. Read-only network use; writes only inside
// this project folder. Run: node tools/fetch-vendor.mjs tools/vendor-manifest.json
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const ROOT = resolve(import.meta.dirname, "..");
const UA = { "User-Agent": "bybus-vendor-fetch" };

function contentsUrl(repo, path, ref) {
  const suffix = ref ? `?ref=${encodeURIComponent(ref)}` : "";
  return `https://api.github.com/repos/${repo}/contents/${path}${suffix}`;
}

async function api(url) {
  const response = await fetch(url, { headers: UA });
  if (!response.ok) throw new Error(`${url} -> HTTP ${response.status}`);
  return response.json();
}

async function fetchFile(repo, path, ref) {
  const row = await api(contentsUrl(repo, path, ref));
  if (row.encoding !== "base64") throw new Error(`${path}: unexpected encoding ${row.encoding}`);
  return Buffer.from(row.content, "base64");
}

const manifestPath = process.argv[2];
if (!manifestPath) throw new Error("usage: node tools/fetch-vendor.mjs <manifest.json>");
const manifest = JSON.parse(await readFile(resolve(ROOT, manifestPath), "utf8"));
const target = resolve(ROOT, manifest.target);
const ref = manifest.ref || "";
await mkdir(target, { recursive: true });

const written = [];
if (manifest.mode === "single-dir") {
  const dirUrl = contentsUrl(manifest.repo, manifest.dir, ref);
  const rows = await api(dirUrl);
  const byName = new Map(rows.map((row) => [row.name, row]));
  const missing = manifest.files.filter((name) => !byName.has(name));
  if (missing.length) throw new Error(`missing upstream files: ${missing.join(", ")}`);
  for (const name of manifest.files) {
    const bytes = await fetchFile(manifest.repo, `${manifest.dir}/${name}`, ref);
    await writeFile(resolve(target, name), bytes);
    written.push(`${name}\t${byName.get(name).sha}\t${bytes.length}`);
  }
} else {
  for (const entry of manifest.files) {
    const bytes = await fetchFile(manifest.repo, entry, ref);
    const relative = entry.split("/").slice(1).join("/");
    await mkdir(dirname(resolve(target, relative)), { recursive: true });
    await writeFile(resolve(target, relative), bytes);
    written.push(`${entry}\t${bytes.length}`);
  }
}
await writeFile(
  resolve(target, "VENDOR-RECORD.txt"),
  `source: github.com/${manifest.repo}${ref ? "@" + ref : " (default branch)"}\nfetched: ${new Date().toISOString()}\n` +
    written.join("\n") + "\n",
);
console.log(`${written.length} files -> ${target}`);
