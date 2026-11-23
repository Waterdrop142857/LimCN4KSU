// Builds the KernelSU module ZIP with correct Unix permissions.
//
// Compress-Archive / .NET ZipArchive do not store the executable bit, which
// would leave module scripts and binaries non-executable after installation, so
// this script writes the archive itself. Everything stays inside the project:
//   build/out/*        linked binaries (from build.ps1)
//   module/*           module tree
//   build/bybus-*.zip  final archive
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import {
  crc32 as nodeCrc32,
  deflateRawSync,
  inflateRawSync,
  constants as zlibConstants,
} from "node:zlib";
import { mkdir, readFile, stat, writeFile, copyFile } from "node:fs/promises";
import { basename, join, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("..", import.meta.url));
const MODULE = join(ROOT, "module");
const OUT = join(ROOT, "build", "out");
const BUILD = join(ROOT, "build");
const WEBUI = join(ROOT, "webui");

const EXECUTABLE = new Set([
  "service.sh",
  "post-fs-data.sh",
  "uninstall.sh",
  "action.sh",
]);
const EXECUTABLE_DIRS = new Set(["bin"]);
const REQUIRED = [
  "module.prop",
  "NOTICE",
  "customize.sh",
  "service.sh",
  "post-fs-data.sh",
  "uninstall.sh",
  "action.sh",
  "webroot/index.html",
  "webroot/app.js",
  "webroot/app.css",
  "webroot/NOTICE.txt",
  "bin/bybusd",
  "bin/bybusctl",
  "zygisk/arm64-v8a.so",
  "assets/ChineseFont.ttf",
  "assets/font.sha256",
  "assets/Sarasa-Gothic-OFL-1.1.txt",
];

// Unsigned shifts are mandatory here: `>>` sign-extends a negative intermediate
// and produces a table that is self-consistent but not real CRC-32.
const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i += 1) {
    let value = i;
    for (let bit = 0; bit < 8; bit += 1) value = (value >>> 1) ^ (0xedb88320 & (0 - (value & 1)));
    table[i] = value >>> 0;
  }
  return table;
})();

function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

// Re-reads the produced archive with an independent CRC implementation and
// inflates every entry. A writer bug must fail the build, not the device
// installer (KernelSU reports "Invalid checksum" from its Rust zip reader).
function verifyArchive(buffer) {
  const independent = typeof nodeCrc32 === "function" ? nodeCrc32 : crc32;
  let eocd = -1;
  for (let i = buffer.length - 22; i >= 0; i -= 1) {
    if (buffer.readUInt32LE(i) === 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error("zip verify: end of central directory missing");
  const count = buffer.readUInt16LE(eocd + 10);
  let offset = buffer.readUInt32LE(eocd + 16);
  for (let i = 0; i < count; i += 1) {
    const method = buffer.readUInt16LE(offset + 10);
    const storedCrc = buffer.readUInt32LE(offset + 16);
    const compressed = buffer.readUInt32LE(offset + 20);
    const uncompressed = buffer.readUInt32LE(offset + 24);
    const nameLength = buffer.readUInt16LE(offset + 28);
    const extraLength = buffer.readUInt16LE(offset + 30);
    const commentLength = buffer.readUInt16LE(offset + 32);
    const localOffset = buffer.readUInt32LE(offset + 42);
    const name = buffer.toString("utf8", offset + 46, offset + 46 + nameLength);
    const localNameLength = buffer.readUInt16LE(localOffset + 26);
    const localExtraLength = buffer.readUInt16LE(localOffset + 28);
    const dataStart = localOffset + 30 + localNameLength + localExtraLength;
    const payload = buffer.subarray(dataStart, dataStart + compressed);
    const data = method === 8 ? inflateRawSync(payload) : payload;
    if (data.length !== uncompressed) {
      throw new Error(`zip verify: ${name} size ${data.length} != ${uncompressed}`);
    }
    if (independent(data) >>> 0 !== storedCrc) {
      throw new Error(`zip verify: ${name} crc mismatch`);
    }
    offset += 46 + nameLength + extraLength + commentLength;
  }
  return count;
}

async function stageBinaries() {
  await mkdir(join(MODULE, "bin"), { recursive: true });
  await mkdir(join(MODULE, "zygisk"), { recursive: true });
  const copies = [
    ["bybusd", join(MODULE, "bin", "bybusd")],
    ["bybusctl", join(MODULE, "bin", "bybusctl")],
    ["bybus-zygisk.so", join(MODULE, "zygisk", "arm64-v8a.so")],
  ];
  for (const [source, target] of copies) {
    await copyFile(join(OUT, source), target);
    // Guard against packaging a stale binary: the staged copy must equal the
    // freshly linked artefact byte for byte.
    const [staged, built] = await Promise.all([readFile(target), readFile(join(OUT, source))]);
    if (!staged.equals(built)) throw new Error(`staged copy differs from build output: ${source}`);
  }
}

async function checkElf(name) {
  const data = await readFile(join(MODULE, name));
  if (data.length < 64 || data.subarray(0, 4).toString("hex") !== "7f454c46" ||
      data[4] !== 2 || data[5] !== 1 || data.readUInt16LE(18) !== 183) {
    throw new Error(`expected ELF64 arm64 binary: ${name}`);
  }
}

function zipEntry(name, data, mode) {
  const nameBytes = Buffer.from(name, "utf8");
  const deflated = deflateRawSync(data, { level: zlibConstants.Z_BEST_COMPRESSION });
  const useDeflate = deflated.length < data.length;
  const payload = useDeflate ? deflated : data;
  const local = Buffer.alloc(30);
  local.writeUInt32LE(0x04034b50, 0);
  local.writeUInt16LE(20, 4);               // version needed
  local.writeUInt16LE(0x0800, 6);           // UTF-8 names
  local.writeUInt16LE(useDeflate ? 8 : 0, 8);
  local.writeUInt16LE(0, 10);               // time
  local.writeUInt16LE(0x21, 12);            // date (1996-01-01)
  local.writeUInt32LE(crc32(data), 14);
  local.writeUInt32LE(payload.length, 18);
  local.writeUInt32LE(data.length, 22);
  local.writeUInt16LE(nameBytes.length, 26);
  local.writeUInt16LE(0, 28);
  return { nameBytes, payload, local, compressed: payload.length, uncompressed: data.length, crc: crc32(data), method: useDeflate ? 8 : 0, mode };
}

async function packageModule() {
  const viteCli = join(WEBUI, "node_modules", "vite", "bin", "vite.js");
  const vite = await stat(viteCli).catch(() => null);
  if (!vite?.isFile()) {
    throw new Error("Vite dependencies missing; run pnpm install --frozen-lockfile in webui");
  }
  execFileSync(process.execPath, [viteCli, "build"], { cwd: WEBUI, stdio: "inherit" });
  await stageBinaries();
  for (const required of REQUIRED) {
    const info = await stat(join(MODULE, required)).catch(() => null);
    if (!info || !info.isFile()) throw new Error(`missing module file: ${required}`);
  }

  for (const binary of ["bin/bybusd", "bin/bybusctl", "zygisk/arm64-v8a.so"]) {
    await checkElf(binary);
  }
  const font = await readFile(join(MODULE, "assets/ChineseFont.ttf"));
  const fontDigest = (await readFile(join(MODULE, "assets/font.sha256"), "utf8")).trim().split(/\s+/)[0];
  if (createHash("sha256").update(font).digest("hex") !== fontDigest) {
    throw new Error("font digest mismatch");
  }

  const files = REQUIRED.slice().sort().map((name) => join(MODULE, name));
  const entries = [];
  const sums = [];
  for (const file of files) {
    const name = file.slice(MODULE.length + 1).split(sep).join("/");
    const data = await readFile(file);
    const top = name.split("/")[0];
    const executable = EXECUTABLE.has(name) || EXECUTABLE_DIRS.has(top);
    const mode = executable ? 0o100755 : 0o100644;
    entries.push(zipEntry(name, data, mode));
    sums.push(`${createHash("sha256").update(data).digest("hex")}  ${name}`);
  }

  const localParts = [];
  let offset = 0;
  for (const entry of entries) {
    entry.offset = offset;
    localParts.push(entry.local, entry.nameBytes, entry.payload);
    offset += entry.local.length + entry.nameBytes.length + entry.payload.length;
  }
  const central = [];
  for (const entry of entries) {
    const header = Buffer.alloc(46);
    header.writeUInt32LE(0x02014b50, 0);
    header.writeUInt16LE(0x031e, 4);        // made by: UNIX, v3.0
    header.writeUInt16LE(20, 6);
    header.writeUInt16LE(0x0800, 8);
    header.writeUInt16LE(entry.method, 10);
    header.writeUInt16LE(0, 12);
    header.writeUInt16LE(0x21, 14);
    header.writeUInt32LE(entry.crc, 16);
    header.writeUInt32LE(entry.compressed, 20);
    header.writeUInt32LE(entry.uncompressed, 24);
    header.writeUInt16LE(entry.nameBytes.length, 28);
    header.writeUInt16LE(0, 30);
    header.writeUInt16LE(0, 32);
    header.writeUInt16LE(0, 34);
    header.writeUInt16LE(0, 36);
    header.writeUInt32LE((entry.mode << 16) >>> 0, 38);
    header.writeUInt32LE(entry.offset, 42);
    central.push(header, entry.nameBytes);
  }
  const centralBuffer = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(0, 4);
  end.writeUInt16LE(0, 6);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralBuffer.length, 12);
  end.writeUInt32LE(offset, 16);
  end.writeUInt16LE(0, 20);

  const archive = Buffer.concat([...localParts, centralBuffer, end]);
  // Fail the build here rather than shipping a package that any real zip reader
  // (KernelSU's ksud included) rejects as "Invalid checksum".
  const verified = verifyArchive(archive);
  if (verified !== entries.length) {
    throw new Error(`zip verify: ${verified} entries readable, ${entries.length} written`);
  }
  const props = await readFile(join(MODULE, "module.prop"), "utf8");
  const version = /^version=(.+)$/m.exec(props)?.[1]?.trim() ?? "0.0.0";
  const target = join(BUILD, `bybus-${version}.zip`);
  await writeFile(target, archive);
  await writeFile(join(BUILD, "SHA256SUMS"), `${sums.join("\n")}\n`);
  const archiveDigest = createHash("sha256").update(archive).digest("hex");
  await writeFile(`${target}.sha256`, `${archiveDigest}  ${basename(target)}\n`);

  const total = entries.reduce((sum, entry) => sum + entry.uncompressed, 0);
  console.log(`zlib version: ${zlibConstants.Z_BEST_COMPRESSION !== undefined ? "ok" : "n/a"}`);
  console.log(`entries: ${entries.length}  uncompressed: ${(total / 1048576).toFixed(1)} MiB  zip: ${(archive.length / 1048576).toFixed(1)} MiB`);
  console.log(`archive: ${target}`);
  console.log(`sha256:  ${archiveDigest}`);
  return target;
}

await packageModule();


