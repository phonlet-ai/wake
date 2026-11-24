import sdk=require('@phonlet-ai/wake-node');
async function run(model:string,keywords:string){const wake=await sdk.Wake.create({model,keywords});wake.process(new Int16Array(512));wake.close();}
export {run};
