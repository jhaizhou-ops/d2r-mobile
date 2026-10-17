import assert from 'node:assert/strict';
import { commitVerifiedImport } from '../entry/src/main/ets/data/ImportCommit.ts';
function fixture(entries, failMove) {
  const files = new Map(Object.entries(entries));
  let moves = 0;
  return { files, async exists(path) { return files.has(path); }, async move(from, to) {
    if (++moves === failMove) { throw new Error('injected rename failure'); }
    assert.ok(files.has(from)); assert.ok(!files.has(to), 'do not overwrite another file');
    files.set(to, files.get(from)); files.delete(from);
  } };
}
let io = fixture({staged:'verified'});
await commitVerifiedImport(io,'staged','target','backup',false);
assert.deepEqual([...io.files], [['target','verified']]);
io=fixture({staged:'verified',target:'old'});
await assert.rejects(commitVerifiedImport(io,'staged','target','backup',false));
assert.equal(io.files.get('target'),'old'); assert.equal(io.files.get('staged'),'verified');
await commitVerifiedImport(io,'staged','target','backup',true);
assert.equal(io.files.get('target'),'verified'); assert.equal(io.files.get('backup'),'old');
io=fixture({staged:'verified',target:'old'},2);
await assert.rejects(commitVerifiedImport(io,'staged','target','backup',true),/injected/);
assert.equal(io.files.get('target'),'old'); assert.equal(io.files.get('staged'),'verified');
io=fixture({staged:'verified',target:'old'},1);
await assert.rejects(commitVerifiedImport(io,'staged','target','backup',true));
assert.equal(io.files.get('target'),'old'); assert.equal(io.files.get('staged'),'verified');
io=fixture({staged:'verified',target:'old',backup:'prior'});
await assert.rejects(commitVerifiedImport(io,'staged','target','backup',true));
assert.equal(io.files.get('backup'),'prior'); assert.equal(io.files.get('target'),'old');
// Process ended after backup rename: startup sees missing target and validated incoming.
io=fixture({staged:'verified',backup:'old'});
await commitVerifiedImport(io,'staged','target','unused',false);
assert.equal(io.files.get('target'),'verified'); assert.equal(io.files.get('backup'),'old');
console.log('PASS: first import, explicit repair, backup retention, rename failures/rollback, backup collision, recoverable verified incoming');
