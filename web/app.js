// The patcher in the browser: the game's binary and Info.plist are patched by the same
// core as the desktop patcher (web/patcher.cpp, compiled with Emscripten); every other
// file of the IPA is copied into the new zip as it is, still compressed. Nothing is
// decompressed but the two files being patched, and nothing leaves the browser.
import createFtlCore from './ftl-core.js';

const DYLIB_PATH = 'Frameworks/ftlios.dylib';
const HYPERSPACE_PATH = 'Frameworks/Hyperspace.1.6.21.arm64.dylib';
const DEFAULT_TEMPLATE = new URL('files/prepatch.ftlt', import.meta.url);
const DEFAULT_HYPERSPACE_TEMPLATE = new URL('files/prepatch-hyperspace.ftlt', import.meta.url);
const DEFAULT_DYLIB = new URL('files/ftlios.dylib', import.meta.url);

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder();

let corePromise = null;
function core() {
  if (!corePromise) corePromise = createFtlCore();
  return corePromise;
}

// ---- zip reading (central directory, Zip64 where needed) ----

const EOCD = 0x06054b50;
const EOCD64 = 0x06064b50;
const LOCATOR64 = 0x07064b50;
const CENTRAL = 0x02014b50;
const LOCAL = 0x04034b50;

const u64 = (view, at) => Number(view.getBigUint64(at, true));

export async function readZipDirectory(file) {
  const tailLen = Math.min(file.size, 0xffff + 22);
  const tailStart = file.size - tailLen;
  const tail = new DataView(await file.slice(tailStart).arrayBuffer());
  let eocd = -1;
  for (let i = tail.byteLength - 22; i >= 0; i--) {
    if (tail.getUint32(i, true) !== EOCD) continue;
    if (i + 22 + tail.getUint16(i + 20, true) === tail.byteLength) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error('This file is not an IPA (no zip end record found).');

  let count = tail.getUint16(eocd + 10, true);
  let cdSize = tail.getUint32(eocd + 12, true);
  let cdOffset = tail.getUint32(eocd + 16, true);
  if (count === 0xffff || cdSize === 0xffffffff || cdOffset === 0xffffffff) {
    // Zip64: the real values are in the Zip64 end record, found by the locator before it
    const locator = Math.min(eocd, 20);
    if (tail.getUint32(eocd - locator, true) !== LOCATOR64)
      throw new Error('This IPA is too large or too broken to read.');
    const at = u64(tail, eocd - locator + 8) - tailStart;
    const rec = new DataView(await file.slice(at, at + 56).arrayBuffer());
    if (rec.getUint32(0, true) !== EOCD64) throw new Error('This IPA is too large or too broken to read.');
    count = u64(rec, 32);
    cdSize = u64(rec, 40);
    cdOffset = u64(rec, 48);
  }

  const cd = new DataView(await file.slice(cdOffset, cdOffset + cdSize).arrayBuffer());
  const entries = [];
  let at = 0;
  for (let i = 0; i < count; i++) {
    if (cd.getUint32(at, true) !== CENTRAL) throw new Error('This IPA has a damaged file list.');
    const nameLen = cd.getUint16(at + 28, true);
    const extraLen = cd.getUint16(at + 30, true);
    const commentLen = cd.getUint16(at + 32, true);
    const entry = {
      name: DECODER.decode(new Uint8Array(cd.buffer, cd.byteOffset + at + 46, nameLen)),
      method: cd.getUint16(at + 10, true),
      time: cd.getUint16(at + 12, true),
      date: cd.getUint16(at + 14, true),
      crc: cd.getUint32(at + 16, true),
      csize: cd.getUint32(at + 20, true),
      usize: cd.getUint32(at + 24, true),
      local: cd.getUint32(at + 42, true),
    };
    if (entry.usize === 0xffffffff || entry.csize === 0xffffffff || entry.local === 0xffffffff) {
      for (let x = at + 46 + nameLen, end = x + extraLen; x + 4 <= end; ) {
        const id = cd.getUint16(x, true);
        const size = cd.getUint16(x + 2, true);
        let p = x + 4;
        if (id === 1) {
          // The Zip64 extra field: only the values that don't fit, in this order
          if (entry.usize === 0xffffffff) {
            entry.usize = u64(cd, p);
            p += 8;
          }
          if (entry.csize === 0xffffffff) {
            entry.csize = u64(cd, p);
            p += 8;
          }
          if (entry.local === 0xffffffff) {
            entry.local = u64(cd, p);
            p += 8;
          }
        }
        x += 4 + size;
      }
    }
    entries.push(entry);
    at += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

// One entry's contents, decompressed
export async function readEntry(file, entry) {
  const header = new DataView(await file.slice(entry.local, entry.local + 30).arrayBuffer());
  if (header.getUint32(0, true) !== LOCAL) throw new Error(`This IPA is damaged (${entry.name}).`);
  const start = entry.local + 30 + header.getUint16(26, true) + header.getUint16(28, true);
  const raw = file.slice(start, start + entry.csize);
  if (entry.method === 0) return new Uint8Array(await raw.arrayBuffer());
  if (entry.method !== 8) throw new Error(`This IPA uses an unsupported compression (${entry.method}).`);
  const stream = raw.stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// ---- zip writing ----

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function dosNow() {
  const d = new Date();
  return {
    time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
    date: ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
  };
}

class ZipWriter {
  constructor() {
    this.parts = [];
    this.offset = 0;
    this.entries = [];
  }

  // A file: `data` (Uint8Array, stored) or `source` (a Blob of already compressed data)
  add(name, { data = null, source = null, method = 0, crc = 0, csize = 0, usize = 0, mode = 0o100644, time, date }) {
    const nameBytes = ENCODER.encode(name);
    if (data) {
      method = 0;
      crc = crc32(data);
      csize = usize = data.length;
    }
    const offset = this.offset;
    const zip64 = usize >= 0xffffffff || csize >= 0xffffffff || offset >= 0xffffffff;
    const extraLen = zip64 ? 20 : 0;
    const header = new Uint8Array(30 + nameBytes.length + extraLen);
    const v = new DataView(header.buffer);
    v.setUint32(0, LOCAL, true);
    v.setUint16(4, zip64 ? 45 : 20, true);
    v.setUint16(6, 0x0800, true); // names are UTF-8
    v.setUint16(8, method, true);
    v.setUint16(10, time, true);
    v.setUint16(12, date, true);
    v.setUint32(14, crc, true);
    v.setUint32(18, zip64 ? 0xffffffff : csize, true);
    v.setUint32(22, zip64 ? 0xffffffff : usize, true);
    v.setUint16(26, nameBytes.length, true);
    v.setUint16(28, extraLen, true);
    header.set(nameBytes, 30);
    if (zip64) {
      const p = 30 + nameBytes.length;
      v.setUint16(p, 1, true);
      v.setUint16(p + 2, 16, true);
      v.setBigUint64(p + 4, BigInt(usize), true);
      v.setBigUint64(p + 12, BigInt(csize), true);
    }
    this.parts.push(header, data ?? source);
    this.offset += header.length + csize;
    this.entries.push({ nameBytes, method, crc, csize, usize, offset, mode, zip64, time, date });
  }

  finish() {
    const cdOffset = this.offset;
    const central = [];
    let cdSize = 0;
    for (const e of this.entries) {
      const extraLen = e.zip64 ? 28 : 0;
      const rec = new Uint8Array(46 + e.nameBytes.length + extraLen);
      const v = new DataView(rec.buffer);
      v.setUint32(0, CENTRAL, true);
      v.setUint16(4, (3 << 8) | 45, true); // made on Unix: the mode below is used on extraction
      v.setUint16(6, e.zip64 ? 45 : 20, true);
      v.setUint16(8, 0x0800, true);
      v.setUint16(10, e.method, true);
      v.setUint16(12, e.time, true);
      v.setUint16(14, e.date, true);
      v.setUint32(16, e.crc, true);
      v.setUint32(20, e.zip64 ? 0xffffffff : e.csize, true);
      v.setUint32(24, e.zip64 ? 0xffffffff : e.usize, true);
      v.setUint16(28, e.nameBytes.length, true);
      v.setUint16(30, extraLen, true);
      v.setUint32(38, (e.mode << 16) >>> 0, true);
      v.setUint32(42, e.zip64 ? 0xffffffff : e.offset, true);
      rec.set(e.nameBytes, 46);
      if (e.zip64) {
        const p = 46 + e.nameBytes.length;
        v.setUint16(p, 1, true);
        v.setUint16(p + 2, 24, true);
        v.setBigUint64(p + 4, BigInt(e.usize), true);
        v.setBigUint64(p + 12, BigInt(e.csize), true);
        v.setBigUint64(p + 20, BigInt(e.offset), true);
      }
      central.push(rec);
      cdSize += rec.length;
    }
    this.parts.push(...central);

    const zip64 = this.entries.length >= 0xffff || cdOffset >= 0xffffffff || cdSize >= 0xffffffff;
    const end = new Uint8Array(zip64 ? 56 + 20 + 22 : 22);
    const v = new DataView(end.buffer);
    let at = 0;
    if (zip64) {
      v.setUint32(at, EOCD64, true);
      v.setBigUint64(at + 4, 44n, true);
      v.setUint16(at + 12, (3 << 8) | 45, true);
      v.setUint16(at + 14, 45, true);
      v.setBigUint64(at + 24, BigInt(this.entries.length), true);
      v.setBigUint64(at + 32, BigInt(this.entries.length), true);
      v.setBigUint64(at + 40, BigInt(cdSize), true);
      v.setBigUint64(at + 48, BigInt(cdOffset), true);
      at += 56;
      v.setUint32(at, LOCATOR64, true);
      v.setBigUint64(at + 8, BigInt(cdOffset + cdSize), true);
      v.setUint32(at + 16, 1, true);
      at += 20;
    }
    v.setUint32(at, EOCD, true);
    v.setUint16(at + 8, Math.min(this.entries.length, 0xffff), true);
    v.setUint16(at + 10, Math.min(this.entries.length, 0xffff), true);
    v.setUint32(at + 12, zip64 ? 0xffffffff : cdSize, true);
    v.setUint32(at + 16, zip64 ? 0xffffffff : cdOffset, true);
    this.parts.push(end);
    return new Blob(this.parts, { type: 'application/octet-stream' });
  }
}

// ---- the patcher core (WASM) ----

function cError(mod) {
  const message = mod.UTF8ToString(mod._ftl_error());
  return new Error(message || 'The patcher failed.');
}

function withBytes(mod, buffers, fn) {
  const pointers = buffers.map((bytes) => {
    const p = mod._malloc(bytes.length || 1);
    if (bytes.length) mod.HEAPU8.set(bytes, p);
    return p;
  });
  try {
    return fn(...pointers);
  } finally {
    for (const p of pointers) mod._free(p);
  }
}

function outBytes(mod, pointer, lenPointer) {
  if (!pointer) throw cError(mod);
  const len = mod.HEAPU32[lenPointer >> 2];
  const bytes = mod.HEAPU8.slice(pointer, pointer + len);
  mod._ftl_free(pointer);
  return bytes;
}

function newString(mod, text) {
  const bytes = ENCODER.encode(text);
  const p = mod._malloc(bytes.length + 1);
  mod.HEAPU8.set(bytes, p);
  mod.HEAPU8[p + bytes.length] = 0;
  return p;
}

function plistString(mod, plist, key) {
  const lenPointer = mod._malloc(4);
  const keyPointer = newString(mod, key);
  try {
    const out = withBytes(mod, [plist], (p) => mod._ftl_plist_string(p, plist.length, keyPointer, lenPointer));
    if (!out) {
      // No message: the key is not there (or not a string)
      const message = mod.UTF8ToString(mod._ftl_error());
      if (message) throw new Error(message);
      return null;
    }
    return outBytes(mod, out, lenPointer);
  } finally {
    mod._free(keyPointer);
    mod._free(lenPointer);
  }
}

async function loadAsset(url) {
  if (url instanceof Uint8Array) return url;
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Could not load ${url.pathname} (${response.status}).`);
  return new Uint8Array(await response.arrayBuffer());
}

// A Hyperspace build for iOS is a 64-bit arm64 dylib (the desktop patcher's check)
function checkDylib(bytes, name) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  // mach_header_64: magic, cputype (arm64), cpusubtype, filetype (MH_DYLIB = 6)
  if (
    bytes.length < 32 ||
    view.getUint32(0, true) !== 0xfeedfacf ||
    view.getUint32(4, true) !== 0x0100000c ||
    view.getUint32(12, true) !== 6
  )
    throw new Error(`${name} isn’t a Hyperspace build for iOS (an arm64 .dylib).`);
  // The platform it's built for (LC_BUILD_VERSION), so its macOS build is caught too
  const commands = view.getUint32(16, true);
  for (let at = 32, i = 0; i < commands && at + 16 <= bytes.length; i++) {
    const cmd = view.getUint32(at, true);
    const size = view.getUint32(at + 4, true);
    if (cmd === 0x32) {
      const platform = view.getUint32(at + 8, true);
      if (platform === 1 || platform === 6)
        throw new Error(`${name} is Hyperspace’s macOS build; choose its iOS build (buildscripts/ios/build.sh device).`);
      break;
    }
    if (size < 8) break;
    at += size;
  }
}

// The icon files as the core packed them: count, then each name and its PNG
function unpackIcons(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const icons = new Map();
  let at = 0;
  const count = view.getUint32(at, true);
  at += 4;
  for (let i = 0; i < count; i++) {
    const nameLen = view.getUint32(at, true);
    at += 4;
    const name = DECODER.decode(bytes.subarray(at, at + nameLen));
    at += nameLen;
    const pngLen = view.getUint32(at, true);
    at += 4;
    icons.set(name, bytes.slice(at, at + pngLen));
    at += pngLen;
  }
  return icons;
}

// ---- patching an IPA ----

export async function patchIpa(
  file,
  {
    bundleId = '',
    name = '',
    icon = null, // { name, bytes }, a PNG or .icns
    prepatch = null, // { name, bytes }, another .ftlt template
    hyperspace = null, // { name, bytes }
    simulator = false,
    assets = null,
    onProgress = () => {},
  } = {},
) {
  const templatePromise =
    prepatch?.bytes ?? assets?.template ?? loadAsset(hyperspace ? DEFAULT_HYPERSPACE_TEMPLATE : DEFAULT_TEMPLATE);
  const dylibPromise = assets?.dylib ?? loadAsset(DEFAULT_DYLIB);
  const assetsPromise = Promise.all([templatePromise, dylibPromise]).then(([template, dylib]) => ({
    template,
    dylib,
  }));
  // Read below, once the IPA is checked; don't warn if we give up before then
  assetsPromise.catch(() => {});
  if (prepatch) {
    if (!prepatch.bytes?.length) throw new Error('Choose a prepatch template (.ftlt).');
    if (prepatch.bytes.length < 8 || DECODER.decode(prepatch.bytes.subarray(0, 8)) !== 'FTLTMPL\0')
      throw new Error(`${prepatch.name || 'This file'} isn’t a prepatch template (.ftlt).`);
  }
  if (hyperspace) {
    if (!hyperspace.bytes?.length) throw new Error('Choose Hyperspace’s iOS build (.dylib).');
    checkDylib(hyperspace.bytes, hyperspace.name || 'This dylib');
  }

  onProgress('Reading the IPA…');
  const entries = await readZipDirectory(file);
  const apps = new Set();
  for (const e of entries) {
    const m = /^Payload\/([^/]+\.app)\//.exec(e.name);
    if (m) apps.add(m[1]);
  }
  if (apps.size !== 1) throw new Error('This IPA should have exactly one app (Payload/FTL.app).');
  const app = [...apps][0];
  const prefix = `Payload/${app}/`;
  const byName = new Map(entries.map((e) => [e.name, e]));

  if (bundleId && !/^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$/.test(bundleId))
    throw new Error('The bundle ID can only have letters, digits, - and dots, like com.example.ftl.');
  const has = (name) => byName.has(prefix + name);
  if (has(DYLIB_PATH))
    throw new Error('This IPA is already patched (it has ftlios.dylib in it). Choose the game’s original IPA.');
  const infoEntry = byName.get(prefix + 'Info.plist');
  if (!infoEntry) throw new Error('This IPA has no Info.plist: it is not an app.');

  onProgress('Reading the app’s Info.plist…');
  const info = await readEntry(file, infoEntry);
  const mod = await core();
  const exeNameBytes = plistString(mod, info, 'CFBundleExecutable');
  if (!exeNameBytes) throw new Error('This IPA’s Info.plist has no CFBundleExecutable.');
  const exeName = DECODER.decode(exeNameBytes);
  const exeEntry = byName.get(prefix + exeName);
  if (!exeEntry) throw new Error(`This IPA has no ${exeName}: it is not a complete app.`);
  const originalIdBytes = plistString(mod, info, 'CFBundleIdentifier');
  const id = bundleId || (originalIdBytes ? DECODER.decode(originalIdBytes) : '') || 'com.ftlgame.FTL';

  let icons = null;
  if (icon) {
    onProgress('Making the icon…');
    const iconNames = entries
      .filter((e) => e.name.startsWith(prefix) && !e.name.slice(prefix.length).includes('/'))
      .map((e) => e.name.slice(prefix.length))
      .filter((n) => /^AppIcon.*\.png$/.test(n));
    const lenPointer = mod._malloc(4);
    const namesPointer = newString(mod, iconNames.join('\n'));
    const imageNamePointer = newString(mod, icon.name);
    try {
      const out = withBytes(mod, [icon.bytes], (p) =>
        mod._ftl_icons(namesPointer, imageNamePointer, p, icon.bytes.length, lenPointer),
      );
      icons = unpackIcons(outBytes(mod, out, lenPointer));
    } finally {
      mod._free(namesPointer);
      mod._free(imageNamePointer);
      mod._free(lenPointer);
    }
    onProgress(`  icon: ${icons.size} sizes from ${icon.name}`);
  }

  onProgress('Reading the game…');
  const exe = await readEntry(file, exeEntry);
  const { template, dylib } = await assetsPromise;

  onProgress('Patching the game (this takes a moment)…');
  const exeLen = mod._malloc(4);
  const infoLen = mod._malloc(4);
  const idPointer = newString(mod, id);
  const namePointer = newString(mod, name);
  let patchedExe, patchedInfo;
  try {
    const outExe = withBytes(mod, [exe, template], (ep, tp) =>
      mod._ftl_patch_exe(ep, exe.length, tp, template.length, idPointer, simulator ? 1 : 0, exeLen),
    );
    patchedExe = outBytes(mod, outExe, exeLen);
    const outInfo = withBytes(mod, [info], (p) =>
      mod._ftl_patch_info(p, info.length, idPointer, namePointer, icons ? 1 : 0, simulator ? 1 : 0, infoLen),
    );
    patchedInfo = outBytes(mod, outInfo, infoLen);
  } finally {
    mod._free(idPointer);
    mod._free(namePointer);
    mod._free(exeLen);
    mod._free(infoLen);
  }

  onProgress('Writing the patched IPA…');
  const zip = new ZipWriter();
  const now = dosNow();
  const dropped = (name) => {
    const rel = name.slice(prefix.length);
    return (
      rel === 'ftl.dat.vanilla' ||
      rel === 'embedded.mobileprovision' ||
      rel === '_CodeSignature' ||
      rel.startsWith('_CodeSignature/') ||
      rel === 'SC_Info' ||
      rel.startsWith('SC_Info/')
    );
  };
  let wroteExe = false;
  for (const e of entries) {
    if (e.name.endsWith('/') || !e.name.startsWith(prefix) || dropped(e.name)) continue;
    const rel = e.name.slice(prefix.length);
    // Replaced below when Hyperspace is on; left out like the desktop patcher when it is off
    if (rel === HYPERSPACE_PATH) continue;
    // Executables keep their mode; the rest follow what the desktop patcher writes
    const executable = e.name === prefix + exeName || /\.dylib$/.test(e.name);
    const mode = executable ? 0o100755 : 0o100644;
    if (e.name === prefix + exeName) {
      zip.add(e.name, { data: patchedExe, mode, ...now });
      wroteExe = true;
    } else if (e.name === prefix + 'Info.plist') {
      zip.add(e.name, { data: patchedInfo, mode, ...now });
    } else if (icons && icons.has(rel)) {
      zip.add(e.name, { data: icons.get(rel), mode, ...now });
    } else {
      // Copied as it is: the compressed bytes go into the new zip, still compressed
      const header = new DataView(await file.slice(e.local, e.local + 30).arrayBuffer());
      const start = e.local + 30 + header.getUint16(26, true) + header.getUint16(28, true);
      zip.add(e.name, {
        source: file.slice(start, start + e.csize),
        method: e.method,
        crc: e.crc,
        csize: e.csize,
        usize: e.usize,
        mode,
        time: e.time,
        date: e.date,
      });
    }
  }
  if (!wroteExe) throw new Error(`This IPA has no ${exeName}.`);
  zip.add(prefix + DYLIB_PATH, { data: dylib, mode: 0o100755, ...now });
  if (hyperspace) zip.add(prefix + HYPERSPACE_PATH, { data: hyperspace.bytes, mode: 0o100755, ...now });

  const patched = zip.finish();
  onProgress('Done.');
  return {
    blob: patched,
    name: file.name.replace(/\.ipa$/i, '') + '-patched.ipa',
    bundleId: id,
    size: patched.size,
  };
}
