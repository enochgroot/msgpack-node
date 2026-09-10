'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('events');
const net = require('net');
const msgpack = require('../lib/msgpack');
const stub = require('./fixtures/stub');

function roundTrip(value) {
  const packed = msgpack.pack(value);
  assert.ok(Buffer.isBuffer(packed));
  return msgpack.unpack(packed);
}

describe('msgpack pack/unpack', () => {
  it('round-trips primitive and composite values', () => {
    const o = [
      'string',
      true,
      false,
      null,
      0,
      1,
      -1,
      1.1,
      -1.1,
      2,
      20,
      200,
      2000,
      20000,
      200000,
      2000000,
      20000000,
      200000000,
      2000000000,
      -2,
      -20,
      -200,
      -2000,
      -20000,
      -200000,
      -2000000,
      -20000000,
      -200000000,
      -2000000000,
      { foo: 'bar', baz: 'quux' },
      [1, 2, 3, 4],
      Buffer.from([0, 1, 2, 3, 4, 5, 6, 7]),
    ];
    const got = roundTrip(o);
    assert.deepEqual(got, o);
    assert.ok(Buffer.isBuffer(got[got.length - 1]));
  });

  it('unpacks a 7-bit integer', () => {
    assert.equal(msgpack.unpack(Buffer.from([0x05])), 5);
  });

  it('returns null for a truncated object', () => {
    const packed = msgpack.pack({ a: 'abc', b: 1, c: [1, 2, 3] });
    const truncated = packed.subarray(0, packed.length - 1);
    assert.equal(msgpack.unpack(truncated), null);
  });

  it('uses toJSON when packing', () => {
    const obj = { a: 1 };
    obj.toJSON = () => ({ b: 2 });
    assert.deepEqual(msgpack.unpack(msgpack.pack(obj)), { b: 2 });
  });

  it('throws on circular object and array', () => {
    const o = {};
    o.a = o;
    assert.throws(() => msgpack.pack(o), /circular/);

    const a = [];
    a.push(a);
    assert.throws(() => msgpack.pack(a), /circular/);
  });
});

describe('msgpack.Stream', () => {
  it('sends a packed message through write', () => {
    const s = new EventEmitter();
    s.writable = true;
    s.write = stub();
    const ms = new msgpack.Stream(s);
    ms.send('hello');
    assert.equal(s.write.called, true);
    assert.equal(s.write.args.length, 1);
    assert.deepEqual(msgpack.unpack(s.write.args[0]), 'hello');
  });

  it('passes extra send arguments to write', () => {
    const s = new EventEmitter();
    s.writable = true;
    s.write = stub();
    const ms = new msgpack.Stream(s);
    ms.send('hello', 1, 2, 3);
    assert.equal(s.write.called, true);
    assert.equal(s.write.args.length, 4);
    assert.deepEqual(msgpack.unpack(s.write.args[0]), 'hello');
    assert.deepEqual(Array.prototype.slice.call(s.write.args, 1), [1, 2, 3]);
  });

  it('emits msg for a complete packet', () => {
    const s = new EventEmitter();
    const ms = new msgpack.Stream(s);
    ms.addListener('msg', stub());
    s.emit('data', msgpack.pack('hello'));
    assert.equal(ms.listeners('msg')[0].called, true);
    assert.equal(ms.listeners('msg')[0].args.length, 1);
    assert.equal(ms.listeners('msg')[0].args[0], 'hello');
  });

  it('parses two messages split across data events', () => {
    const s = new EventEmitter();
    const ms = new msgpack.Stream(s);
    ms.addListener('msg', stub());
    const packed = msgpack.pack('hello');
    s.emit('data', packed.subarray(0, packed.length - 1));
    assert.equal(ms.listeners('msg')[0].called, false);
    s.emit('data', packed.subarray(packed.length - 1));
    assert.equal(ms.listeners('msg')[0].called, true);
    assert.equal(ms.listeners('msg')[0].args.length, 1);
    assert.equal(ms.listeners('msg')[0].args[0], 'hello');
  });

  it('round-trips over a TCP socket', (t, done) => {
    const server = net.createServer((c) => {
      c.write(msgpack.pack('hello '));
      setTimeout(() => {
        c.end(msgpack.pack('world'));
      }, 50);
    });

    server.listen(0, '127.0.0.1', () => {
      const addr = server.address();
      const client = net.createConnection(addr.port, addr.address);
      const msgs = [];
      client.on('connect', () => {
        const ms = new msgpack.Stream(client);
        ms.addListener('msg', (m) => {
          msgs.push(m);
          if (msgs.length === 2) {
            assert.deepEqual(msgs, ['hello ', 'world']);
            server.close();
            done();
          }
        });
      });
    });
  });
});
