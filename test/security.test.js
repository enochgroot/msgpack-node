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

function nestArrays(n) {
  let a = 1;
  for (let i = 0; i < n; i++) a = [a];
  return a;
}

describe('pack recursion depth', () => {
  it('throws instead of crashing on 8000 nested arrays', () => {
    /* Before the cap this recursed 8000 frames deep in C++ and SIGSEGVed. */
    assert.throws(() => msgpack.pack(nestArrays(8000)), /Cowardly refusing/);
    /* Process is still alive and usable. */
    assert.equal(msgpack.unpack(msgpack.pack('ok')), 'ok');
  });

  it('throws instead of crashing on 8000 nested objects', () => {
    let o = 1;
    for (let i = 0; i < 8000; i++) o = { o };
    assert.throws(() => msgpack.pack(o), /Cowardly refusing/);
    assert.equal(msgpack.unpack(msgpack.pack('ok')), 'ok');
  });

  it('still round-trips allowed nesting depths', () => {
    for (const n of [1, 32, 100]) {
      assert.deepEqual(msgpack.unpack(msgpack.pack(nestArrays(n))), nestArrays(n));
    }
  });
});

describe('unpack depth vs the C embed stack', () => {
  it('round-trips 32 levels of nesting', () => {
    /* msgpack-c defaults MSGPACK_EMBED_STACK_SIZE to 32, which made this
     * fail with a generic parse error; the vendored build now uses 512. */
    const a = nestArrays(32);
    assert.deepEqual(msgpack.unpack(msgpack.pack(a)), a);
  });

  it('round-trips the deepest packable nesting', () => {
    /* Pack allows a value at depth 512, so the deepest array holding a
     * scalar is 511 levels; unpack accepts 511 containers as well. */
    const a = nestArrays(511);
    assert.deepEqual(msgpack.unpack(msgpack.pack(a)), a);
    assert.throws(() => msgpack.pack(nestArrays(512)), /Cowardly refusing/);
  });

  it('rejects an over-deep buffer with the limit error', () => {
    /* Hand-built: N fixarray-of-1 headers around a single fixint. 511
     * containers is the working maximum, 512 is over the limit. */
    const deep = (n) => Buffer.concat([Buffer.alloc(n, 0x91), Buffer.from([0x01])]);
    assert.notEqual(msgpack.unpack(deep(511)), null);
    assert.throws(() => msgpack.unpack(deep(512)), /limit exceeded/);
    assert.throws(() => msgpack.unpack(deep(1000)), /limit exceeded/);
  });
});
