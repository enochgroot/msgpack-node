`node-msgpack` is an addon for [Node.js](https://nodejs.org) that serializes
and de-serializes JavaScript values with [MessagePack](https://msgpack.org).
Packed output is a `Buffer` and is typically much smaller than JSON.

This tree is a security-focused 2.0 fork (`enochgroot/msgpack-node`).
It requires **Node.js 18+**, vendors **msgpack-c c-7.0.2**, and rejects
oversized unpack headers instead of allocating them. See `SECURITY.md`.

### Usage

```javascript
const assert = require('assert');
const msgpack = require('msgpack');

const o = { a: 1, b: 2, c: [1, 2, 3] };
const b = msgpack.pack(o);
const oo = msgpack.unpack(b);

assert.deepEqual(oo, o);
```

`pack()` accepts any JSON-like value plus Node `Buffer`s and `Date`s.
`unpack()` consumes a `Buffer` and returns a JavaScript value, or `null` if
the buffer is a truncated (incomplete) MessagePack object. Oversized
array/map/string bombs throw.

A streaming helper wraps a readable socket and emits `msg`, plus `error` when
a packet cannot be unpacked (the offending buffer is dropped):

```javascript
const msgpack = require('msgpack');
const ms = new msgpack.Stream(socket);
ms.on('msg', (m) => {
  console.log('received', m);
});
ms.on('error', (e) => {
  console.error('bad packet', e.message);
});
ms.send({ hello: 'world' });
```

### Type mapping (2.0)

Packing:

* `undefined` / `null` → nil
* `boolean` → bool
* finite integers → uint/int
* other numbers → float64
* `string` → str (UTF-8)
* `Date` → str (ISO 8601, `toISOString()`), at any nesting level
* `Buffer` → bin
* `Array` → array
* objects with a `toJSON()` method → whatever `toJSON()` returns, at any
  nesting level
* other objects → map of every own enumerable key; numeric keys are packed as
  integer keys, not dropped
* functions, circular refs, and nesting deeper than 512 throw

Unpacking:

* nil → `null`
* bool / int / float → JS boolean / number
* str → `string`
* bin → `Buffer`
* array / map → Array / Object
* ext → throws

`unpack.bytes_remaining` is the number of unused trailing bytes after the last
successful (or attempted) unpack. Stream uses that to splice leftover data.

### Limits

* array/map length ≤ 1,000,000
* str/bin/ext length ≤ 32 MiB
* nesting depth ≤ 512 on both pack and unpack

The payload `dd ff 00 00 00` throws `msgpack unpack limit exceeded`. Packing a
value nested deeper than 512 throws `Cowardly refusing to pack object nested
more than 512 levels deep` instead of overflowing the C stack.

### Building, installation, testing

```
npm install
npm test
```

Needs a C/C++ toolchain and Python (node-gyp). GitHub Actions runs Node 18/20/22
on Ubuntu and macOS.

### Command line

`bin/json2msgpack` and `bin/msgpack2json` convert JSON ↔ MessagePack on
stdin/stdout:

```
echo '{"hello":"world"}' | bin/json2msgpack | bin/msgpack2json
```

### License

BSD-3-Clause for this addon. Vendored msgpack-c is Boost Software License 1.0
(`deps/msgpack/LICENSE`).
