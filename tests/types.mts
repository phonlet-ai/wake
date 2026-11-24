import {Wake,WakeError,type KeywordFile,type Phrase} from '@phonlet-ai/wake-node';
import {english} from '@phonlet-ai/wake-model-en';
import {Wake as WebWake} from '@phonlet-ai/wake-web';
import {WakeManager} from '@phonlet-ai/wake-web/capture';
const wake=await Wake.create({model:english.model,keywords:english.keywords,sensitivity:.7});
const when:number=wake.process(new Int16Array(512))[0]?.timeSeconds;
wake.finish();wake.close();new WakeError('INVALID_STATE','closed');
const phrase:Phrase={name:'hey kettle',pronunciations:[[16,13,20,11,31,3,21]],threshold_loose:-1.21875,threshold_strict:-0.8740234375,endpoint:false,sensitivity:.3};
const file:KeywordFile={model:{id:'en-1',sha256:'0'.repeat(64)},keywords:[phrase]};
await Wake.create({model:english.model,keywords:[phrase]});await Wake.create({model:english.model,keywords:file});
const web=await WebWake.create({model:'model.pwake',keywords:'keywords.json'});
await web.process(new Int16Array(512));await web.close();
const manager=new WakeManager({wake:{model:'model.pwake',keywords:[phrase]},onDetection:event=>console.log(event.keyword),onError:console.error});
await manager.start();await manager.stop();await manager.close();
// @ts-expect-error sensitivity is numeric
Wake.create({model:english.model,keywords:[{...phrase,sensitivity:'high'}]});
// @ts-expect-error a phrase needs its thresholds
Wake.create({model:english.model,keywords:[{name:'hey kettle',pronunciations:[[16]],endpoint:false}]});
