(async()=>{
 const assert=(ok,message)=>{if(!ok)throw Error(message);};
 const {Wake}=await import(location.origin+'/vendor/wake/index.mjs');
 const {WakeManager}=await import(location.origin+'/vendor/wake/capture.mjs');
 const fixture=await (await fetch('./test-fixture.json')).json(),report=[];
 const model=new Uint8Array(await (await fetch('./assets/model.pwake')).arrayBuffer());
 const synthetic={model,keywords:fixture.keywords};
 for(const simd of [false,true]){
  const wake=await Wake.create({...synthetic,simd}),found=[];
  for(let i=0;i<fixture.samples;i+=997)found.push(...await wake.process(new Int16Array(Math.min(997,fixture.samples-i))));
  assert(JSON.stringify(found.map(e=>e.frameIndex))===JSON.stringify(fixture.expected_frames),'wrong positive event frames');
  assert(found.every(e=>e.keywordIndex===1&&e.sensitivity===1&&e.sampleIndex===(e.frameIndex+1)*512),'wrong event metadata');
  const final=await wake.finish();assert(final.frames===70&&final.discardedSamples===17,'wrong EOF');
  await wake.close();report.push({backend:wake.info.backend,events:found,final});
 }
 // Borrowed capture resources survive stop/restart/close; detections stop before stop resolves.
 const context=new AudioContext({sampleRate:16000}),destination=context.createMediaStreamDestination();
 const oscillator=context.createOscillator();oscillator.frequency.value=220;oscillator.connect(destination);oscillator.start();
 const found=[],errors=[];
 const manager=new WakeManager({wake:synthetic,audioContext:context,stream:destination.stream,onDetection:e=>found.push(e),onError:e=>errors.push(e.message)});
 await manager.start();await manager.start();await new Promise(r=>setTimeout(r,2000));await manager.stop();
 const count=found.length;await new Promise(r=>setTimeout(r,100));assert(found.length===count,'event after stop');
 assert(context.state!=='closed'&&destination.stream.getTracks().every(t=>t.readyState==='live'),'borrowed resource closed');
 await manager.start();await new Promise(r=>setTimeout(r,1200));await manager.close();await manager.close();
 assert(!errors.length,'capture errors: '+errors.join(';'));assert(found.length>0,'capture yielded no synthetic events; context time='+context.currentTime);
 assert(manager.state==='closed','manager not closed');
 oscillator.stop();destination.stream.getTracks().forEach(t=>t.stop());await context.close();
 // Own a fake microphone/context and prove they are closed on normal shutdown.
 const originalContext=window.AudioContext,originalCapture=navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
 const contexts=[],streams=[];
 window.AudioContext=class extends originalContext{constructor(...args){super(...args);contexts.push(this);}};
 navigator.mediaDevices.getUserMedia=async constraints=>{const stream=await originalCapture(constraints);streams.push(stream);return stream;};
 try{
  const own=new WakeManager({wake:{model:'./assets/model.pwake',keywords:'./assets/keywords.json'},onDetection:()=>{},onError:e=>errors.push(e.message)});
  await own.start();await own.stop();await own.close();
  assert(contexts.length===1&&contexts[0].state==='closed','owned context leaked');
  assert(streams.length===1&&streams[0].getTracks().every(t=>t.readyState==='ended'),'owned tracks leaked');
 }finally{window.AudioContext=originalContext;navigator.mediaDevices.getUserMedia=originalCapture;}
 assert(!errors.length,'owned capture errors');
 window.offlineWake=await Wake.create({model:'./assets/model.pwake',keywords:'./assets/keywords.json'});
 return {browser:navigator.userAgent,report,capture:{borrowed:'passed',owned:'passed',restart:'passed',syntheticEvents:found.length},readyForOffline:true};
})()
