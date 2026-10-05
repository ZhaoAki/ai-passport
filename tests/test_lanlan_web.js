const fs=require('fs'),vm=require('vm'),assert=require('assert');
class El {
 constructor(){this.value='';this.textContent='';this.children=[];this.hidden=false;this.checked=false;}
 get firstChild(){return this.children[0]||null;}
 appendChild(e){this.children.push(e);if(e.selected||(!this.value && e.value))this.value=e.value;return e;}
 removeChild(e){this.children.splice(this.children.indexOf(e),1);if(!this.children.length)this.value='';}
 setAttribute(){} focus(){} reset(){} addEventListener(){}
}
const nodes={};const requests=[];
const ctx={console,Date,Math,Uint8Array,isFinite,setTimeout:()=>1,clearTimeout:()=>{},document:{readyState:'loading',addEventListener(){},createElement:()=>new El(),getElementById:()=>new El()},window:{crypto:{randomUUID:()=> 'probe-request-id'},location:{hash:''},scrollTo(){}}};
ctx.fetch=(path,options)=>{requests.push(JSON.parse(options.body));return Promise.reject(Error('lost response'));};
vm.createContext(ctx);
let source=fs.readFileSync('web/lanlan/app.js','utf8').replace(/\}\(\)\);\s*$/,'window.probe={state,nodes,fillRecordForm,collectRecordBody,handleRecordSubmit,toUtcIso,datetimeLocalValue};}());');
vm.runInContext(source,ctx);const p=ctx.window.probe;
for(const n of ['recordTitle','recordCategory','recordSubitem','recordCustomName','recordTime','recordEstimated','recordPerformer','recordAmount','recordUnit','recordDuration','recordNote','recordFeedback','recordConflict','recordRetry','recordSubmit','recordSubitemField','recordCustomNameField','amountField','durationField','netState','netText','networkState','networkText'])p.nodes[n]=new El();
p.state.user={id:'hehe'};p.state.members=[{id:'hehe',display_name:'赫赫'},{id:'yangyang',display_name:'羊羊'}];
assert.equal(p.toUtcIso(p.datetimeLocalValue(new Date('2026-10-01T01:00:00Z'))), '2026-10-01T01:00:00Z');
assert.equal(p.toUtcIso('2026-02-30T09:00'), null);
const base={category:'meal',occurred_at:'2026-10-01T01:00:00Z',performed_by:'hehe',version:1};
p.fillRecordForm({...base,id:'known',amount_value:100,amount_unit:'g'});
p.fillRecordForm({...base,id:'unknown',amount_value:null,amount_unit:null});
assert.equal(p.nodes.recordAmount.value, '');
assert.equal(p.collectRecordBody(false).amount_value, undefined);
(async()=>{
 p.state.editing=null;p.state.formId='same-key';p.nodes.recordAmount.value='';p.nodes.recordNote.value='first body';
 p.handleRecordSubmit({preventDefault(){}});await new Promise(r=>setImmediate(r));
 p.nodes.recordNote.value='corrected body';p.handleRecordSubmit({preventDefault(){}});await new Promise(r=>setImmediate(r));
 assert.equal(requests.length,2);
 assert.equal(requests[0].client_request_id,requests[1].client_request_id);
 const saved={...base,id:'persisted',note:'first body'};
 ctx.fetch=()=>Promise.resolve({status:409,ok:false,text:()=>Promise.resolve(JSON.stringify({
   error:{code:'idempotency_conflict'},record:saved}))});
 p.handleRecordSubmit({preventDefault(){}});await new Promise(r=>setImmediate(r));
 assert.equal(p.nodes.recordNote.value,'corrected body');
 assert.equal(p.state.editing.id,'persisted');
 assert.equal(p.collectRecordBody(false).expected_version,1);
 assert.equal(p.nodes.recordSubmit.textContent,'保存修改');
 console.log('Web regression tests: PASS ('+process.env.TZ+')');
})().catch(e=>{console.error(e);process.exitCode=1;});
