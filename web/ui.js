// The page: choose an IPA, patch it, save the result.
import { patchIpa } from './app.js';

const drop = document.getElementById('drop');
const input = document.getElementById('ipa');
const dropTitle = document.getElementById('drop-title');
const dropHint = drop.querySelector('.drop-hint');
const nameInput = document.getElementById('name');
const iconInput = document.getElementById('icon');
const bundleId = document.getElementById('bundle-id');
const prepatchInput = document.getElementById('prepatch');
const hyperspace = document.getElementById('hyperspace');
const hyperspaceFiles = document.getElementById('hyperspace-files');
const hyperspaceDylib = document.getElementById('hyperspace-dylib');
const patch = document.getElementById('patch');
const status = document.getElementById('status');
const log = document.getElementById('log');
const result = document.getElementById('result');
const download = document.getElementById('download');

let file = null;
let patchedUrl = null;

function showStatus(text, isError = false) {
  status.textContent = text;
  status.classList.toggle('error', isError);
}

function logLine(text) {
  const line = document.createElement('li');
  line.textContent = text;
  log.append(line);
}

function choose(selected) {
  file = selected;
  if (!file) return;
  drop.classList.add('has-file');
  dropTitle.textContent = file.name;
  dropHint.textContent = `${(file.size / 1e6).toFixed(0)} MB`;
  patch.disabled = false;
  showStatus('');
  log.replaceChildren();
  result.hidden = true;
}

drop.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' || event.key === ' ') {
    event.preventDefault();
    input.click();
  }
});
input.addEventListener('change', () => choose(input.files[0] ?? null));

for (const type of ['dragenter', 'dragover']) {
  drop.addEventListener(type, (event) => {
    event.preventDefault();
    drop.classList.add('over');
  });
}
for (const type of ['dragleave', 'drop']) {
  drop.addEventListener(type, (event) => {
    event.preventDefault();
    drop.classList.remove('over');
  });
}
drop.addEventListener('drop', (event) => {
  const dropped = [...(event.dataTransfer?.files ?? [])].find((f) => /\.ipa$/i.test(f.name));
  if (!dropped) return showStatus('Drop an .ipa file.', true);
  choose(dropped);
});

hyperspace.addEventListener('change', () => {
  hyperspaceFiles.hidden = !hyperspace.checked;
});

patch.addEventListener('click', async () => {
  if (!file) return;
  patch.disabled = true;
  result.hidden = true;
  log.replaceChildren();
  if (patchedUrl) {
    URL.revokeObjectURL(patchedUrl);
    patchedUrl = null;
  }
  showStatus('Working…');
  try {
    const options = {
      bundleId: bundleId.value.trim(),
      name: nameInput.value.trim(),
      onProgress: logLine,
    };
    const iconFile = iconInput.files[0];
    if (iconFile) options.icon = { name: iconFile.name, bytes: new Uint8Array(await iconFile.arrayBuffer()) };
    const prepatchFile = prepatchInput.files[0];
    if (prepatchFile)
      options.prepatch = { name: prepatchFile.name, bytes: new Uint8Array(await prepatchFile.arrayBuffer()) };
    if (hyperspace.checked) {
      const dylibFile = hyperspaceDylib.files[0];
      if (!dylibFile) throw new Error('Choose Hyperspace’s iOS build (.dylib), or turn Hyperspace off.');
      options.hyperspace = { name: dylibFile.name, bytes: new Uint8Array(await dylibFile.arrayBuffer()) };
    }
    const out = await patchIpa(file, options);
    patchedUrl = URL.createObjectURL(out.blob);
    download.href = patchedUrl;
    download.download = out.name;
    download.textContent = `Save ${out.name} (${(out.size / 1e6).toFixed(0)} MB)`;
    result.hidden = false;
    showStatus('The patched IPA is ready.');
  } catch (error) {
    console.error(error);
    showStatus(error.message || 'The patch failed.', true);
  } finally {
    patch.disabled = false;
  }
});
