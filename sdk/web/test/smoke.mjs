import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { load } from '../js/kws.mjs';
const simd=process.argv.includes('--simd');
const kws=await load({simd});
const bytes=await readFile(new URL('../../../resources/models/en-1/model.pwake',import.meta.url));
const keywords=JSON.parse(await readFile(new URL('../../../resources/keywords/en.json',import.meta.url),'utf8')).keywords.slice(0,3);
assert.equal(kws.frontendId,'3f047efa');
const model=kws.loadModel(bytes);
assert.equal(model.nClasses,40);
const pcm=Int16Array.from({length:16031},(_,i)=>(i*7919%65536)-32768);
const frontend=kws.frontend();
const mel=frontend.push(pcm.slice(0,8192));
assert.equal(createHash('sha256').update(new Uint8Array(mel.buffer,mel.byteOffset,mel.byteLength)).digest('hex'),'26027391afd651bf2b575b0a2887a7afaf6311e1bb817e49fecead507ab55cf0');
frontend.close();
const options={keywords,outputs:{posteriors:true,scores:true,events:true}};
const whole=kws.session(model,options);
const expected=whole.pushPcm(pcm);
const finish=whole.finish();
const stream=kws.session(model,options);
const chunks=[stream.pushPcm(pcm.slice(0,113)),stream.pushPcm(pcm.slice(113,5555)),stream.pushPcm(pcm.slice(5555))];
assert.deepEqual(stream.finish(),finish);
for(const name of ['posteriors','scores','fires']) {
  assert.deepEqual(Array.from(expected[name]),chunks.flatMap(x=>Array.from(x[name])));
}
whole.close(); stream.close(); model.close();
console.log(`model and streaming parity passed (${simd?'SIMD':'scalar'})`);
