# Security notes (node-msgpack 2.0.0)

This fork vendors [msgpack-c](https://github.com/msgpack/msgpack-c) **c-7.0.2**
(`e17beb371b59459a13b48e166a11e123bda5bf93`), the C library.

## Unpack limits

`msgpack.unpack` is fail-closed. Before the C unpacker runs, a format walker
rejects:

- array/map counts above 1,000,000
- str/bin/ext lengths above 32 MiB
- nesting deeper than 512
- container counts that cannot fit in the remaining buffer when the declared
  size is also past those caps

The known bomb `dd ff 00 00 00` (array32 with 0xFF000000 elements) throws
`msgpack unpack limit exceeded` and does not allocate.

Vendored `src/unpack.c` also caps `template_callback_array` /
`template_callback_map` at `MSGPACK_NODE_MAX_CONTAINER` (1e6) so a missed
walker case still cannot ask the zone allocator for gigabytes of
`msgpack_object` slots.

## unpacker_expand_buffer / integer overflow

msgpack-c 6.1.0+ added overflow checks in `msgpack_unpacker_expand_buffer`
(size vs `SIZE_MAX - used`, doubling that saturates). c-7.0.2 includes those
checks. This binding uses `msgpack_unpack_next` (non-streaming) for
`unpack()`, so the expander is not on the default path; it is still present
in the vendored sources.

CVE-2026-72854 (msgpack-c unpacker buffer expansion) is addressed by staying
on c-7.0.2 rather than the historical 0.5.x/1.x C snapshot this addon used
to ship.

## nodejs/node#25686 (sbuffer leak on pack throw)

`pack()` used to allocate a `msgpack_sbuffer` and return via `Nan::ThrowError`
on circular refs / unencodable values without freeing it. The sbuffer is now
owned by an RAII guard that returns pooled buffers or `msgpack_sbuffer_free`s
on every exit path, including C++ exceptions.
