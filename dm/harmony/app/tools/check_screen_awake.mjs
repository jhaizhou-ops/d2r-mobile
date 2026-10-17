import assert from 'node:assert/strict';
import { ScreenAwake } from '../entry/src/main/ets/lifecycle/ScreenAwake.ts';
const calls = [], errors = [];
let resolveFirst, held = false;
const target = { async setWindowKeepScreenOn(value) {
  calls.push(value);
  if (value && held) { await new Promise(resolve => { resolveFirst = resolve; }); }
} };
const awake = new ScreenAwake(message => errors.push(message));
awake.bind(target); awake.setForeground(true); await awake.whenIdle();
assert.ok(calls.every(value => !value), 'idle foreground must allow screen timeout');
awake.setGameActive(true); await awake.whenIdle(); assert.equal(calls.at(-1), true);
awake.setForeground(false); await awake.whenIdle(); assert.equal(calls.at(-1), false);
awake.setForeground(true); await awake.whenIdle(); assert.equal(calls.at(-1), true);
awake.setGameActive(false); await awake.whenIdle(); assert.equal(calls.at(-1), false);
held = true; awake.setGameActive(true);
await new Promise(resolve => setImmediate(resolve));
assert.equal(typeof resolveFirst, 'function');
awake.setForeground(false); awake.unbind(); resolveFirst();
await awake.whenIdle(); assert.equal(calls.at(-1), false, 'exit must follow an in-flight enable');
awake.bind({ async setWindowKeepScreenOn() { throw new Error('destroyed'); } });
await awake.whenIdle(); assert.equal(errors.length, 1);
awake.bind(target); held = false; awake.setForeground(true); awake.setGameActive(false);
await awake.whenIdle(); assert.equal(calls.at(-1), false, 'failed window must not poison later requests');
console.log('PASS: idle, active, background, foreground, exit, in-flight completion ordering, failure recovery');
