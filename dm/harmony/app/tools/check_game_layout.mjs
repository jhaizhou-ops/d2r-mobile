import assert from 'node:assert/strict';
import {fitGameViewport, controlScale, ImmersiveState, restoreWindowProperties} from '../entry/src/main/ets/lifecycle/GameLayout.ts';
for (const [width,height] of [[312,640],[800,240],[160,90],[1,1]]) {
  const v=fitGameViewport(width,height);
  assert.ok(v.width<=width && v.height<=height);
  assert.ok(Math.abs(v.width/v.height-16/9)<1e-10);
}
for (const x of [0,-1,NaN,Infinity]) assert.deepEqual(fitGameViewport(x,20),{width:0,height:0});
for (const x of [0,-1,NaN,Infinity]) assert.equal(controlScale(x,270),0);
for (const width of [140,220,270,328,700]) {
  for (const base of [270,328]) {
    const scale=controlScale(width,base);
    assert.ok(base*scale<=width);
    assert.equal(36*scale/scale,36,'scaled stick coordinates must map back to its center');
  }
}
const changes=[],errors=[],calls=[];
let resolveEnter;
const target={async apply(enabled) {calls.push(enabled);if(enabled) await new Promise(r=>resolveEnter=r);}};
const state=new ImmersiveState(v=>changes.push(v),e=>errors.push(e));
state.bind(target); state.request(true);
await new Promise(r=>setImmediate(r));
state.request(false); resolveEnter(); await state.whenIdle();
assert.equal(calls.at(-1),false); assert.equal(changes.at(-1),false);
assert.ok(!changes.includes(true),'stale enter completion cannot put UI back into fullscreen');
state.request(true); await new Promise(r=>setImmediate(r)); state.unbind(); resolveEnter();
await state.whenIdle(); assert.equal(calls.at(-1),false);
const failureCalls=[];
state.bind({async apply(v){failureCalls.push(v);if(v)throw Error('layout failure');}});
state.request(true);await state.whenIdle();
assert.deepEqual(failureCalls,[true,false]); assert.equal(errors.length,1);
state.bind({async apply(v){calls.push(v);}});state.request(true);await state.whenIdle();
assert.equal(changes.at(-1),true);state.request(false);await state.whenIdle();assert.equal(changes.at(-1),false);
const restored=[];
await assert.rejects(restoreWindowProperties(async()=>{restored.push('layout');throw Error('destroying');},
  async()=>{restored.push('orientation');}));
assert.deepEqual(restored,['layout','orientation']);
console.log('PASS production layout: aspect/letterbox, tiny windows, coordinate scaling, late enter/exit, unbind, partial failure restore, queue recovery');
