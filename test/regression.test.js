'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const msgpack = require('../lib/msgpack');

describe('regressions', () => {
  it('unpacks a 7-bit integer from a larger buffer (bytes_remaining)', () => {
    const buf = Buffer.from([0x05, 0x00]);
    assert.equal(msgpack.unpack(buf), 5);
    assert.equal(msgpack.unpack.bytes_remaining, 1);
  });

  it('rejects Date without toJSON as a packable object (no crash)', () => {
    /* lib/msgpack.js invokes toJSON for Date before native pack. */
    const packed = msgpack.pack(new Date('2000-06-13T00:00:00.000Z'));
    assert.equal(typeof msgpack.unpack(packed), 'string');
  });
});
