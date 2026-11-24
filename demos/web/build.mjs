import {cp, mkdir, readFile, readdir, rm, writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const sdk = process.argv[2] ?? path.dirname(fileURLToPath(import.meta.resolve('@phonlet-ai/wake-web')));
const assets = process.argv[3] ?? path.dirname(fileURLToPath(import.meta.resolve('@phonlet-ai/wake-model-en')));
const brand = process.argv[4] ?? path.resolve(here, '../../assets/brand');
const out = path.join(here, 'dist');

// Only this generated directory is replaced. Removed source files cannot linger in a release.
await rm(out, {recursive: true, force: true});
for (const dir of ['vendor/wake', 'assets', 'brand']) await mkdir(path.join(out, dir), {recursive: true});
for (const name of ['index.html', 'app.mjs', 'styles.css']) await cp(path.join(here, name), path.join(out, name));
for (const name of await readdir(sdk)) {
  if (name.endsWith('.mjs') || ['js', 'dist'].includes(name)) {
    await cp(path.join(sdk, name), path.join(out, 'vendor/wake', name), {recursive: true});
  }
}
for (const name of ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md']) await cp(path.join(sdk, name), path.join(out, 'vendor/wake', name));
for (const name of ['model.pwake', 'keywords.json', 'LICENSE', 'THIRD_PARTY_NOTICES.md']) await cp(path.join(assets, name), path.join(out, 'assets', name));
const brandFiles = ['phonlet-ai-horizontal-dark.svg', 'phonlet-ai-favicon-gradient.svg', 'phonlet-ai-wave-background.svg', 'brand-tokens.css'];
for (const name of brandFiles) {
  await cp(path.join(brand, name), path.join(out, 'brand', name));
}
const brandManifest = JSON.parse(await readFile(path.join(brand, 'manifest.json'), 'utf8'));
brandManifest.files = Object.fromEntries(brandFiles.map(name => [name, brandManifest.files[name]]));
await writeFile(path.join(out, 'brand/manifest.json'), JSON.stringify(brandManifest, null, 2) + '\n');
await cp(path.join(sdk, 'LICENSE'), path.join(out, 'LICENSE'));
await cp(path.join(sdk, 'THIRD_PARTY_NOTICES.md'), path.join(out, 'THIRD_PARTY_NOTICES.md'));
await writeFile(path.join(out, 'README.txt'), `Phonlet Wake — private browser demo\n\nServe this directory:\n  python -m http.server 8080 --bind 127.0.0.1\nOpen http://localhost:8080 in your browser.\n\nAll model, runtime and brand assets are included locally. No CDN is used.\nMicrophone: localhost/HTTPS, 16 kHz AudioContext.\nFiles: mono 16 kHz signed PCM16 WAV.\n\nChoose phrases and adjust each sensitivity before starting.\nStop releases resources owned by the demo. Start creates a fresh session.\nSee LICENSE for distribution terms and THIRD_PARTY_NOTICES.md for third-party components.\n`);
console.log(`Standalone demo: ${out}`);
