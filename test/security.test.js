'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const msgpack = require('../lib/msgpack');

function assertThrowsUnpack(buf, re) {
  assert.throws(() => msgpack.unpack(buf), re);
}

describe('unpack DoS limits', () => {
  it('rejects array32 header dd ff 00 00 00 without allocating', { timeout: 5000 }, () => {
    const buf = Buffer.from([0xdd, 0xff, 0x00, 0x00, 0x00]);
    assertThrowsUnpack(buf, /limit exceeded/);
  });

  it('rejects map32 header df ff 00 00 00', { timeout: 5000 }, () => {
    const buf = Buffer.from([0xdf, 0xff, 0x00, 0x00, 0x00]);
    assertThrowsUnpack(buf, /limit exceeded/);
  });

  it('rejects str32 claiming 32MiB+1 bytes', { timeout: 5000 }, () => {
    const buf = Buffer.from([0xdb, 0x02, 0x00, 0x00, 0x01]);
    assertThrowsUnpack(buf, /limit exceeded/);
  });

  it('rejects bin32 claiming 32MiB+1 bytes', { timeout: 5000 }, () => {
    const buf = Buffer.from([0xc6, 0x02, 0x00, 0x00, 0x01]);
    assertThrowsUnpack(buf, /limit exceeded/);
  });

  it('returns null for a truncated but modest array16', () => {
    /* array16 of 3 elements, no payload — incomplete, not a bomb. */
    const buf = Buffer.from([0xdc, 0x00, 0x03]);
    assert.equal(msgpack.unpack(buf), null);
  });

  it('round-trips a legitimately large array', () => {
    const n = 10000;
    const a = new Array(n);
    for (let i = 0; i < n; i++) a[i] = i;
    assert.deepEqual(msgpack.unpack(msgpack.pack(a)), a);
  });
});

describe('pack throw paths do not leak (nodejs/node#25686)', () => {
  it('survives many pack failures without crashing', () => {
    for (let i = 0; i < 20000; i++) {
      assert.throws(() => msgpack.pack([function () {}]));
      const o = {};
      o.self = o;
      assert.throws(() => msgpack.pack(o));
    }
    /* If the sbuffer leaked on throw, 20k failures of even small objects
     * would grow RSS dramatically. A follow-up pack still works. */
    assert.equal(msgpack.unpack(msgpack.pack('ok')), 'ok');
  });
});
