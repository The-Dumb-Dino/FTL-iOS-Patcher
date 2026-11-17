// Runs the web patcher on an IPA from the command line (needs Node 20+):
//   node test.mjs [IPA] [OUT.ipa]
// NAME, ICON (a PNG or .icns), HYPERSPACE (its dylib) and PREPATCH (a .ftlt) test the
// Advanced options.
import { readFile, writeFile } from 'node:fs/promises';
import { patchIpa } from './app.js';

const ipa = process.argv[2] ?? `${process.env.HOME}/Downloads/FTL_1.6.21_ios_8.0.ipa`;
const out = process.argv[3] ?? '/tmp/web-patched.ipa';
const iconPath = process.env.ICON;
const hyperspacePath = process.env.HYPERSPACE;
const prepatchPath = process.env.PREPATCH;

const file = new File([await readFile(ipa)], ipa.split('/').pop());
const assets = {
  template: new Uint8Array(
    await readFile(new URL(hyperspacePath ? 'files/prepatch-hyperspace.ftlt' : 'files/prepatch.ftlt', import.meta.url)),
  ),
  dylib: new Uint8Array(await readFile(new URL('files/ftlios.dylib', import.meta.url))),
};
const options = { assets, name: process.env.NAME ?? '', onProgress: (line) => console.log(line) };
if (iconPath)
  options.icon = { name: iconPath.split('/').pop(), bytes: new Uint8Array(await readFile(iconPath)) };
if (prepatchPath)
  options.prepatch = { name: prepatchPath.split('/').pop(), bytes: new Uint8Array(await readFile(prepatchPath)) };
if (hyperspacePath)
  options.hyperspace = {
    name: hyperspacePath.split('/').pop(),
    bytes: new Uint8Array(await readFile(hyperspacePath)),
  };

const started = performance.now();
const result = await patchIpa(file, options);
await writeFile(out, new Uint8Array(await result.blob.arrayBuffer()));
console.log(`${out}: ${(result.size / 1e6).toFixed(1)} MB, ${((performance.now() - started) / 1000).toFixed(1)} s`);
