// Runs against installed npm packages, never imports Python or checkout code.
import assert from 'node:assert/strict';
import {Wake,WakeError} from '@phonlet-ai/wake-node';
import {english} from '@phonlet-ai/wake-model-en';
import {createRequire} from 'node:module';
import {readFile} from 'node:fs/promises';
const require=createRequire(import.meta.url);
assert.equal(require('@phonlet-ai/wake-node').Wake,Wake);
const file=JSON.parse(await readFile(english.keywords,'utf8'));
const model=new Uint8Array(await readFile(english.model));
const pick=(...names)=>structuredClone(file.keywords.filter(k=>names.includes(k.name)));

// Lifecycle, per-phrase sensitivity and private copies.
const chosen=pick('hey phonlet','assistant');chosen[0].sensitivity=0;
const wake=await Wake.create({model:english.model,keywords:chosen,sensitivity:.7});
chosen[0].sensitivity=1;
assert.deepEqual(wake.keywords.map(k=>k.sensitivity),[0,.7]);
assert.deepEqual(wake.process(new Int16Array()),[]);
assert.deepEqual(wake.process(new Int16Array(113)),[]);
assert.deepEqual(wake.process(new Int16Array(15918)),[]);
assert.deepEqual(wake.finish(),{frames:31,discardedSamples:159});
assert.equal(wake.finish(),wake.finish());
assert.throws(()=>wake.process(new Int16Array()),{code:'INVALID_STATE'});
wake.close();wake.close();assert.throws(()=>wake.finish(),{code:'INVALID_STATE'});

// A path, the parsed file and the bare phrase list detect identically.
const pcm=Int16Array.from({length:64000},(_,i)=>((i*7919)%65536)-32768);
const forms=[english.keywords,new URL(`file://${english.keywords}`),file,file.keywords];
const sessions=await Promise.all(forms.map(keywords=>Wake.create({model,keywords,sensitivity:1})));
assert.equal(new Set(sessions.map(s=>JSON.stringify(s.keywords))).size,1);
assert.equal(new Set(sessions.map(s=>s.info.keywordsSha256)).size,1);
for(let i=0;i<pcm.length;i+=1000){const r=sessions.map(s=>s.process(pcm.slice(i,i+1000)));for(const x of r)assert.deepEqual(x,r[0]);}
assert.equal(sessions[0].info.modelSha256,file.model.sha256);
sessions.forEach(s=>s.close());

// Validation and model binding.
const [phrase]=pick('assistant');
for(const value of [null,true,'0.5',NaN,Infinity,-.1,1.1])await assert.rejects(Wake.create({model,keywords:[{...phrase,sensitivity:value}]}),WakeError);
const {threshold_strict:_,...noStrict}=phrase;
for(const bad of [{...phrase,threshold:-1},{...phrase,threshold_loose:NaN},{...phrase,pronunciations:[[0]]},noStrict])await assert.rejects(Wake.create({model,keywords:[bad]}),WakeError);
for(const keywords of [[],{keywords:[]},{...file,unknown:true},[phrase,phrase],'missing.json'])await assert.rejects(Wake.create({model,keywords}),WakeError);
await assert.rejects(Wake.create({keywords:[phrase]}),WakeError);
await assert.rejects(Wake.create({model,keywords:[phrase],unknown:true}),WakeError);
const other=model.slice();other[0]^=1;
await assert.rejects(Wake.create({model:other,keywords:file}),{code:'INCOMPATIBLE_ARTIFACT'});
console.log(JSON.stringify({node:process.version,native:'passed',esm:true,cjs:true,forms:'passed',modelSha256:file.model.sha256}));

const fixture=JSON.parse(await readFile(new URL('./fixtures/conformance.json',import.meta.url),'utf8'));
const stream=await Wake.create({model,keywords:fixture.keywords}),found=[];
for(let i=0;i<fixture.samples;i+=997)found.push(...stream.process(new Int16Array(Math.min(997,fixture.samples-i))));
assert.deepEqual(found.map(e=>e.frameIndex),fixture.expected_frames);
assert(found.every(e=>e.keywordIndex===1&&e.sensitivity===1&&e.sampleIndex===(e.frameIndex+1)*512));
assert.deepEqual(stream.finish(),{frames:70,discardedSamples:17});stream.close();
console.log(JSON.stringify({positiveConformance:found}));
