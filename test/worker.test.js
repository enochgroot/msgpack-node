'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const path = require('path');
const { Worker } = require('worker_threads');

/* Load the addon on the main thread first: the regression only shows up on
 * the *second* load of msgpackBinding.node, i.e. inside the worker. */
const msgpack = require('../');
const binding = require('../build/Release/msgpackBinding');

const WORKER = path.join(__dirname, 'fixtures', 'msgpack-worker.js');

function runWorker(op) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(WORKER, { workerData: { op: op } });
    let message;
    let settled = false;
    worker.once('message', (m) => { message = m; });
    worker.once('error', (err) => {
      settled = true;
      reject(err);
    });
    worker.once('exit', (code) => {
      if (settled) return;
      if (code !== 0) return reject(new Error('worker exited with code ' + code));
      resolve(message);
    });
  });
}

describe('worker_threads', () => {
  it('loads the addon inside a worker and round-trips values', async () => {
    const result = await runWorker('roundtrip');
    assert.deepEqual(result.keys, ['a', 'b']);
    assert.equal(result.a, 1);
    assert.equal(result.bIsBuffer, true, 'bin stays a Buffer in the worker');
    assert.equal(result.b, 'hi');
  });

  it('still detects cycles inside a worker', async () => {
    const result = await runWorker('cycle');
    assert.equal(result.threw, true);
    assert.match(result.message, /circular/);
  });

  it('leaves the main thread working after a worker has loaded the addon', async () => {
    await runWorker('roundtrip');

    const value = { a: 1, b: Buffer.from('hi'), c: [1, 'two', null] };
    const unpacked = msgpack.unpack(msgpack.pack(value));
    assert.equal(unpacked.a, 1);
    assert.ok(Buffer.isBuffer(unpacked.b));
    assert.equal(unpacked.b.toString('latin1'), 'hi');
    assert.deepEqual(unpacked.c, [1, 'two', null]);

    /* Cycle detection uses a process-wide private key; check it survived the
     * worker's module init. */
    const cyclic = {};
    cyclic.self = cyclic;
    assert.throws(() => msgpack.pack(cyclic), /circular/);
  });

  it('does not let a worker unpack clobber the main thread bytes_remaining', async () => {
    const buf = Buffer.concat([msgpack.pack(1), Buffer.alloc(3)]);
    assert.equal(msgpack.unpack(buf), 1);
    assert.equal(msgpack.unpack.bytes_remaining, 3);

    const result = await runWorker('unpack-remaining');
    assert.equal(result.value, 'worker');
    assert.equal(result.bytesRemaining, 7);

    /* The JS-level snapshot is per-thread by construction; the native counter
     * behind it is a single global unless it is thread_local. */
    assert.equal(msgpack.unpack.bytes_remaining, 3);
    assert.equal(binding.bytesRemaining(), 3);
  });
});
