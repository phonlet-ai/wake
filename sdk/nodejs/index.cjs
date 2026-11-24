'use strict';
const fs = require('node:fs/promises');
const { WakeError, options, selectPhrases, keywordsSha256, events, pcm, freeze, SDK_VERSION, INVENTORY, FRONTEND, readWav, digest } = require('./shared.cjs');
// A keyword file from a path or URL; a parsed file or a list of phrases passes through.
async function keywordFile(value) {
  if (typeof value !== 'string' && !(value instanceof URL)) return value;
  let text;
  try { text = await fs.readFile(value, 'utf8'); }
  catch (error) { throw new WakeError('CORRUPT_ARTIFACT', error.message); }
  try { return JSON.parse(text); }
  catch (error) { throw new WakeError('CORRUPT_ARTIFACT', `keyword file is not valid JSON: ${error.message}`); }
}
// The model container from a path, URL or bytes (copied, so the caller keeps its buffer).
async function modelBytes(value) {
  if (value instanceof Uint8Array) return new Uint8Array(value);
  if (typeof value !== 'string' && !(value instanceof URL)) throw new WakeError('INVALID_ARGUMENT','model must be a path, URL or Uint8Array');
  try { return new Uint8Array(await fs.readFile(value)); }
  catch (error) { throw new WakeError('CORRUPT_ARTIFACT',error.message); }
}
let loaded, engineSha256;
function native() {
  if (loaded) return loaded;
  const target = `${process.platform}-${process.arch}-${process.report.getReport().header.glibcVersionRuntime ? 'gnu' : 'musl'}`;
  if (target !== 'linux-x64-gnu') throw new WakeError('UNSUPPORTED_TARGET',`No qualified native package for ${target}; build from source explicitly.`);
  try {
    const file=require.resolve(`@phonlet-ai/wake-node-${target}`);
    engineSha256=require('node:crypto').createHash('sha256').update(require('node:fs').readFileSync(file)).digest('hex');
    loaded=require(file);
  }
  catch (error) { throw new WakeError('UNSUPPORTED_TARGET',`Install @phonlet-ai/wake-node-${target}@${SDK_VERSION}. ${error.message}`); }
  if (loaded.abi !== 1 || loaded.frontendId.toString(16) !== FRONTEND || JSON.parse(loaded.language).inventory_id !== INVENTORY) throw new WakeError('INCOMPATIBLE_ARTIFACT','native engine identity mismatch');
  return loaded;
}
class Wake {
  #handle; #native; #selected; #state = 'ready'; #final; #info;
  constructor(token,addon,handle,selected,info) {
    if (token !== Wake) throw new WakeError('INVALID_ARGUMENT','use Wake.create');
    this.#native=addon;this.#handle=handle;this.#selected=selected;this.#info=freeze(info);
  }
  // A model (path, URL or bytes) and its phrases: a keyword file path or URL, the parsed keyword
  // file, or a list of phrases written in code. A keyword file that names its model must name this one.
  static async create(input) {
    const {mode,defaultValue}=options(input);
    const model=await modelBytes(input.model), modelSha256=await digest(model);
    const selected=selectPhrases(await keywordFile(input.keywords),modelSha256,defaultValue);
    const addon=native();
    const handle=addon.create(model,JSON.stringify(selected),Float64Array.from(selected,k=>k.sensitivity),mode==='independent'?0:1);
    return new Wake(Wake,addon,handle,selected,{sdkVersion:SDK_VERSION,engineAbi:1,backend:'node-api',engineSha256,keywordsSha256:await keywordsSha256(selected),keywordMode:mode,scorer:'production',scorerOptions:'',modelSha256,inventoryId:INVENTORY,frontendId:FRONTEND});
  }
  get sampleRate(){return 16000;} get frameLength(){return 512;} get state(){return this.#state;}
  get keywords(){return this.#selected;} get info(){return this.#info;}
  process(input) {
    if(this.#state!=='ready') throw new WakeError('INVALID_STATE',`Wake is ${this.#state}`);
    pcm(input);let result;
    try{result=this.#native.process(this.#handle,input);}
    catch(error){this.close();throw error;}
    return events(result.fires,result.frameOffset,this.#selected);
  }
  finish(){
    if(this.#state==='closed') throw new WakeError('INVALID_STATE','Wake is closed');
    if(!this.#final){this.#final=Object.freeze(this.#native.finish(this.#handle));this.#state='finished';}
    return this.#final;
  }
  close(){if(this.#state!=='closed'){this.#native.close(this.#handle);this.#state='closed';}}
}
module.exports={Wake,WakeError,readWav};
