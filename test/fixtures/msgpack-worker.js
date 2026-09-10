'use strict';

/* Loaded inside a worker_threads Worker. Requiring the addon here is the
 * actual failure in msgpack-node#60: a non-context-aware NODE_MODULE throws
 * "Module did not self-register" the second time the .node file is loaded. */
const { parentPort, workerData } = require('worker_threads');
const msgpack = require('../../');

function run(op) {
  switch (op) {
    case 'roundtrip': {
      const value = { a: 1, b: Buffer.from('hi') };
      const unpacked = msgpack.unpack(msgpack.pack(value));
      return {
        a: unpacked.a,
        bIsBuffer: Buffer.isBuffer(unpacked.b),
        b: unpacked.b.toString('latin1'),
        keys: Object.keys(unpacked)
      };
    }
    case 'cycle': {
      const o = { name: 'loop' };
      o.self = o;
      try {
        msgpack.pack(o);
        return { threw: false, message: null };
      } catch (err) {
        return { threw: true, message: err.message };
      }
    }
    case 'unpack-remaining': {
      /* A different buffer than the main thread used, leaving a different
       * number of trailing bytes behind. */
      const buf = Buffer.concat([msgpack.pack('worker'), Buffer.alloc(7)]);
      const value = msgpack.unpack(buf);
      return { value: value, bytesRemaining: msgpack.unpack.bytes_remaining };
    }
    default:
      throw new Error('unknown op: ' + op);
  }
}

parentPort.postMessage(run(workerData.op));
