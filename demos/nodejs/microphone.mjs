// Linux example: ALSA capture is optional; the core SDK has no capture dependency.
import {spawn} from 'node:child_process';
import {Wake} from '@phonlet-ai/wake-node';
import {english} from '@phonlet-ai/wake-model-en';
import {args} from './options.mjs';
const input=args(true), wake=await Wake.create({assets:english,...input.wake});
const recorder=spawn('arecord',['-q','-D',input.device,'-t','raw','-f','S16_LE','-r','16000','-c','1'],{stdio:['ignore','pipe','pipe']});
let stopping=false, diagnostic='', remainder=Buffer.alloc(0);
const stop=()=>{stopping=true;recorder.kill('SIGTERM');};
process.once('SIGINT',stop);process.once('SIGTERM',stop);
const done=new Promise((resolve,reject)=>{recorder.once('error',reject);recorder.once('close',(code,signal)=>stopping||code===0?resolve():reject(new Error(`arecord failed (${code??signal}): ${diagnostic}`)));});
done.catch(()=>{});
recorder.stderr.on('data',data=>{diagnostic=(diagnostic+data.toString()).slice(-4096);});
try{
 console.error('Listening locally; Ctrl+C stops.');
 for await(const data of recorder.stdout){
  const bytes=Buffer.concat([remainder,data]);
  if(bytes.length>8192)throw new Error('Capture backlog exceeded 4096 samples; stopping without dropping audio.');
  const pcm=new Int16Array(Math.floor(bytes.length/2));for(let i=0;i<pcm.length;i++)pcm[i]=bytes.readInt16LE(i*2);
  remainder=Buffer.from(bytes.subarray(pcm.length*2));
  for(const event of wake.process(pcm))console.log(JSON.stringify(event));
 }
 await done;if(remainder.length)throw new Error('Capture ended with a partial PCM16 sample');
 console.error(JSON.stringify(wake.finish()));
}finally{
 stop();await done.catch(()=>{});wake.close();process.removeListener('SIGINT',stop);process.removeListener('SIGTERM',stop);
}
