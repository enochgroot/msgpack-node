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

`pack()` accepts any JSON-like value plus Node `Buffer`s. `unpack()` consumes
a `Buffer` and returns a JavaScript value, or `null` if the buffer is a
truncated (incomplete) MessagePack object. Oversized array/map/string bombs
throw.

A streaming helper wraps a readable socket and emits `msg`:

```javascript
const msgpack = require('msgpack');
const ms = new msgpack.Stream(socket);
ms.on('msg', (m) => {
  console.log('received', m);
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
* `Buffer` → bin
* `Array` → array
* other objects → map (own enumerable string keys)
* functions and circular refs throw

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
* nesting depth ≤ 512

The payload `dd ff 00 00 00` throws `msgpack unpack limit exceeded`.

### Building, installation, testing

```
npm install
npm test
```

Needs a C/C++ toolchain and Python (node-gyp). GitHub Actions runs Node 18/20/22
on Ubuntu and macOS.

### Command line

`bin/json2msgpack` and `bin/msgpack2json` convert JSON ↔ MessagePack on stdin/stdout.

### License

BSD-3-Clause for this addon. Vendored msgpack-c is Boost Software License 1.0
(`deps/msgpack/LICENSE`).
