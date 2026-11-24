import {readFile} from 'node:fs/promises';
import {Wake,readWav} from '@phonlet-ai/wake-node';
import {english} from '@phonlet-ai/wake-model-en';
import {args} from './options.mjs';
const input=args(), pcm=readWav(await readFile(input.path));
const wake=await Wake.create({assets:english,...input.wake});
try{for(let i=0;i<pcm.length;i+=4096)for(const event of wake.process(pcm.subarray(i,i+4096)))console.log(JSON.stringify(event));console.log(JSON.stringify(wake.finish()));}
finally{wake.close();}
