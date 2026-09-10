/*
 * Node.js MessagePack bindings.
 *
 * Pack/unpack JavaScript values via vendored msgpack-c (C API).
 * Unpack is fail-closed: oversized array/map/string/bin headers are rejected
 * before the C library allocates. Pack errors always release the sbuffer
 * (nodejs/node#25686).
 */

#include <cmath>
#include <cstdint>

#include <nan.h>
#include <msgpack.h>

namespace {

const uint32_t kMaxContainer = 1000000u;
const uint32_t kMaxBytes = 32u * 1024u * 1024u;
const int kMaxDepth = 512;
/* Pack recursion is bounded the same way master bounded it, so deeply nested
 * input throws instead of running the C stack out. */
const int kMaxPackDepth = 512;
/* Largest/smallest doubles that survive a cast to uint64_t/int64_t. */
const double kTwoPow64 = 18446744073709551616.0;
const double kInt64Min = -9223372036854775808.0;
const size_t kSbufferPoolMax = 512;

enum ScanStatus {
  kScanOk = 0,
  kScanContinue,
  kScanLimit,
  kScanParse
};

struct Cursor {
  const unsigned char* p;
  const unsigned char* end;
};

static bool ReadU8(Cursor* c, uint8_t* out) {
  if (c->p >= c->end) return false;
  *out = *c->p++;
  return true;
}

static bool ReadU16(Cursor* c, uint16_t* out) {
  if (c->end - c->p < 2) return false;
  *out = static_cast<uint16_t>((c->p[0] << 8) | c->p[1]);
  c->p += 2;
  return true;
}

static bool ReadU32(Cursor* c, uint32_t* out) {
  if (c->end - c->p < 4) return false;
  *out = (static_cast<uint32_t>(c->p[0]) << 24) |
         (static_cast<uint32_t>(c->p[1]) << 16) |
         (static_cast<uint32_t>(c->p[2]) << 8) |
         static_cast<uint32_t>(c->p[3]);
  c->p += 4;
  return true;
}

static bool Skip(Cursor* c, size_t n) {
  if (static_cast<size_t>(c->end - c->p) < n) return false;
  c->p += n;
  return true;
}

static ScanStatus CheckContainer(uint32_t n, size_t remaining, bool is_map, int sp) {
  if (n > kMaxContainer) return kScanLimit;
  uint64_t items = is_map ? static_cast<uint64_t>(n) * 2u : n;
  if (items > remaining) {
    /* Declared payload cannot exist in this buffer. If n is huge this is
     * a DoS header; if n is modest the message is merely truncated. */
    if (n > kMaxContainer || items > kMaxBytes) return kScanLimit;
    return kScanContinue;
  }
  if (sp >= kMaxDepth) return kScanLimit;
  return kScanOk;
}

static ScanStatus CheckBytes(uint32_t n, size_t remaining) {
  if (n > kMaxBytes) return kScanLimit;
  if (n > remaining) return kScanContinue;
  return kScanOk;
}

/*
 * Walk one MessagePack object. Extra trailing bytes are allowed (streaming).
 * Incomplete headers/payloads return kScanContinue. Absurd sizes return
 * kScanLimit without allocating.
 */
static ScanStatus ScanOne(const char* data, size_t len, size_t* consumed) {
  Cursor c;
  c.p = reinterpret_cast<const unsigned char*>(data);
  c.end = c.p + len;

  struct Frame { uint32_t remaining; };
  Frame stack[kMaxDepth];
  int sp = 0;
  stack[sp++].remaining = 1;

  while (sp > 0) {
    if (stack[sp - 1].remaining == 0) {
      sp--;
      continue;
    }
    stack[sp - 1].remaining--;

    uint8_t b;
    if (!ReadU8(&c, &b)) return kScanContinue;

    if (b <= 0x7f || b >= 0xe0) {
      continue; /* fixint */
    }
    if ((b & 0xf0) == 0x80) { /* fixmap */
      uint32_t n = b & 0x0f;
      ScanStatus st = CheckContainer(n, static_cast<size_t>(c.end - c.p), true, sp);
      if (st != kScanOk) return st;
      stack[sp++].remaining = n * 2u;
      continue;
    }
    if ((b & 0xf0) == 0x90) { /* fixarray */
      uint32_t n = b & 0x0f;
      ScanStatus st = CheckContainer(n, static_cast<size_t>(c.end - c.p), false, sp);
      if (st != kScanOk) return st;
      stack[sp++].remaining = n;
      continue;
    }
    if ((b & 0xe0) == 0xa0) { /* fixstr */
      uint32_t n = b & 0x1f;
      ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
      if (st != kScanOk) return st;
      if (!Skip(&c, n)) return kScanContinue;
      continue;
    }

    switch (b) {
      case 0xc0: /* nil */
      case 0xc2: /* false */
      case 0xc3: /* true */
        break;
      case 0xc1:
        return kScanParse;
      case 0xc4: { /* bin8 */
        uint8_t n;
        if (!ReadU8(&c, &n)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xc5: { /* bin16 */
        uint16_t n;
        if (!ReadU16(&c, &n)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xc6: { /* bin32 */
        uint32_t n;
        if (!ReadU32(&c, &n)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xc7: { /* ext8 */
        uint8_t n;
        if (!ReadU8(&c, &n)) return kScanContinue;
        if (!Skip(&c, 1)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xc8: { /* ext16 */
        uint16_t n;
        if (!ReadU16(&c, &n)) return kScanContinue;
        if (!Skip(&c, 1)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xc9: { /* ext32 */
        uint32_t n;
        if (!ReadU32(&c, &n)) return kScanContinue;
        if (!Skip(&c, 1)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xca: /* float32 */
        if (!Skip(&c, 4)) return kScanContinue;
        break;
      case 0xcb: /* float64 */
        if (!Skip(&c, 8)) return kScanContinue;
        break;
      case 0xcc: /* uint8 */
        if (!Skip(&c, 1)) return kScanContinue;
        break;
      case 0xcd: /* uint16 */
        if (!Skip(&c, 2)) return kScanContinue;
        break;
      case 0xce: /* uint32 */
        if (!Skip(&c, 4)) return kScanContinue;
        break;
      case 0xcf: /* uint64 */
        if (!Skip(&c, 8)) return kScanContinue;
        break;
      case 0xd0: /* int8 */
        if (!Skip(&c, 1)) return kScanContinue;
        break;
      case 0xd1: /* int16 */
        if (!Skip(&c, 2)) return kScanContinue;
        break;
      case 0xd2: /* int32 */
        if (!Skip(&c, 4)) return kScanContinue;
        break;
      case 0xd3: /* int64 */
        if (!Skip(&c, 8)) return kScanContinue;
        break;
      case 0xd4: /* fixext1 */
        if (!Skip(&c, 2)) return kScanContinue;
        break;
      case 0xd5: /* fixext2 */
        if (!Skip(&c, 3)) return kScanContinue;
        break;
      case 0xd6: /* fixext4 */
        if (!Skip(&c, 5)) return kScanContinue;
        break;
      case 0xd7: /* fixext8 */
        if (!Skip(&c, 9)) return kScanContinue;
        break;
      case 0xd8: /* fixext16 */
        if (!Skip(&c, 17)) return kScanContinue;
        break;
      case 0xd9: { /* str8 */
        uint8_t n;
        if (!ReadU8(&c, &n)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xda: { /* str16 */
        uint16_t n;
        if (!ReadU16(&c, &n)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xdb: { /* str32 */
        uint32_t n;
        if (!ReadU32(&c, &n)) return kScanContinue;
        ScanStatus st = CheckBytes(n, static_cast<size_t>(c.end - c.p));
        if (st != kScanOk) return st;
        if (!Skip(&c, n)) return kScanContinue;
        break;
      }
      case 0xdc: { /* array16 */
        uint16_t n;
        if (!ReadU16(&c, &n)) return kScanContinue;
        ScanStatus st = CheckContainer(n, static_cast<size_t>(c.end - c.p), false, sp);
        if (st != kScanOk) return st;
        stack[sp++].remaining = n;
        break;
      }
      case 0xdd: { /* array32 */
        uint32_t n;
        if (!ReadU32(&c, &n)) return kScanContinue;
        ScanStatus st = CheckContainer(n, static_cast<size_t>(c.end - c.p), false, sp);
        if (st != kScanOk) return st;
        stack[sp++].remaining = n;
        break;
      }
      case 0xde: { /* map16 */
        uint16_t n;
        if (!ReadU16(&c, &n)) return kScanContinue;
        ScanStatus st = CheckContainer(n, static_cast<size_t>(c.end - c.p), true, sp);
        if (st != kScanOk) return st;
        stack[sp++].remaining = static_cast<uint32_t>(n) * 2u;
        break;
      }
      case 0xdf: { /* map32 */
        uint32_t n;
        if (!ReadU32(&c, &n)) return kScanContinue;
        ScanStatus st = CheckContainer(n, static_cast<size_t>(c.end - c.p), true, sp);
        if (st != kScanOk) return st;
        stack[sp++].remaining = n * 2u;
        break;
      }
      default:
        return kScanParse;
    }
  }

  *consumed = static_cast<size_t>(c.p - reinterpret_cast<const unsigned char*>(data));
  return kScanOk;
}

class MsgpackException {
 public:
  explicit MsgpackException(v8::Local<v8::Value> err) : err_(err) {}
  v8::Local<v8::Value> value() const { return err_; }

 private:
  v8::Local<v8::Value> err_;
};

static v8::Local<v8::Value> Error(const char* msg) {
  return Nan::Error(msg);
}

/* Persistent identity flag for cycle detection (not enumerable). */
static Nan::Persistent<v8::String> stack_key;

static v8::Local<v8::String> StackKey() {
  return Nan::New(stack_key);
}

static void Mark(v8::Local<v8::Object> obj) {
  Nan::SetPrivate(obj, StackKey(), Nan::True());
}

static void Unmark(v8::Local<v8::Object> obj) {
  Nan::DeletePrivate(obj, StackKey());
}

static bool IsMarked(v8::Local<v8::Object> obj) {
  Nan::MaybeLocal<v8::Value> v = Nan::GetPrivate(obj, StackKey());
  if (v.IsEmpty()) return false;
  return v.ToLocalChecked()->IsTrue();
}

static void JsToMsgpack(msgpack_packer* pk, v8::Local<v8::Value> o, int depth);

/*
 * Call a zero-argument JS method, converting a JS-level throw into a
 * MsgpackException carrying the original error so Pack() can rethrow it.
 */
static v8::Local<v8::Value> CallNoArgs(v8::Local<v8::Object> recv,
                                       v8::Local<v8::Function> fn) {
  Nan::TryCatch try_catch;
  Nan::MaybeLocal<v8::Value> r = Nan::Call(fn, recv, 0, NULL);
  if (r.IsEmpty()) {
    v8::Local<v8::Value> ex = try_catch.Exception();
    if (ex.IsEmpty()) {
      throw MsgpackException(Error("Error serializing object"));
    }
    throw MsgpackException(ex);
  }
  return r.ToLocalChecked();
}

static void PackArray(msgpack_packer* pk, v8::Local<v8::Array> arr, int depth) {
  if (IsMarked(arr)) {
    throw MsgpackException(Error("Cowardly refusing to pack circular reference"));
  }
  Mark(arr);
  uint32_t len = arr->Length();
  if (msgpack_pack_array(pk, len)) {
    Unmark(arr);
    throw MsgpackException(Error("Error serializing object"));
  }
  try {
    for (uint32_t i = 0; i < len; i++) {
      JsToMsgpack(pk, Nan::Get(arr, i).ToLocalChecked(), depth);
    }
  } catch (...) {
    Unmark(arr);
    throw;
  }
  Unmark(arr);
}

static void PackObject(msgpack_packer* pk, v8::Local<v8::Object> obj, int depth) {
  if (IsMarked(obj)) {
    throw MsgpackException(Error("Cowardly refusing to pack circular reference"));
  }
  Mark(obj);

  /* toJSON wins over the map encoding, at every level, matching both
   * JSON.stringify and the top-level wrapper in lib/msgpack.js. */
  v8::Local<v8::Value> to_json =
      Nan::Get(obj, Nan::New("toJSON").ToLocalChecked()).FromMaybe(
          v8::Local<v8::Value>(Nan::Undefined()));
  if (to_json->IsFunction()) {
    try {
      JsToMsgpack(pk, CallNoArgs(obj, to_json.As<v8::Function>()), depth);
    } catch (...) {
      Unmark(obj);
      throw;
    }
    Unmark(obj);
    return;
  }

  /* Every own enumerable key is packed, numeric keys included; V8 hands back
   * index keys as Numbers, which JsToMsgpack packs as integer map keys. */
  v8::Local<v8::Array> names = Nan::GetOwnPropertyNames(obj).ToLocalChecked();
  uint32_t len = names->Length();
  if (msgpack_pack_map(pk, len)) {
    Unmark(obj);
    throw MsgpackException(Error("Error serializing object"));
  }
  try {
    for (uint32_t i = 0; i < len; i++) {
      v8::Local<v8::Value> key = Nan::Get(names, i).ToLocalChecked();
      JsToMsgpack(pk, key, depth);
      JsToMsgpack(pk, Nan::Get(obj, key).ToLocalChecked(), depth);
    }
  } catch (...) {
    Unmark(obj);
    throw;
  }
  Unmark(obj);
}

static void JsToMsgpack(msgpack_packer* pk, v8::Local<v8::Value> o, int depth) {
  int rc = 0;

  if (kMaxPackDepth < ++depth) {
    throw MsgpackException(
        Error("Cowardly refusing to pack object nested more than 512 levels deep"));
  }

  if (o->IsUndefined() || o->IsNull()) {
    rc = msgpack_pack_nil(pk);
  } else if (o->IsBoolean()) {
    rc = o->IsTrue() ? msgpack_pack_true(pk) : msgpack_pack_false(pk);
  } else if (o->IsNumber()) {
    double d = Nan::To<double>(o).FromJust();
    /* Only take an integer path when the value actually fits the integer
     * type; otherwise the cast is undefined behavior (1e30 became 2^64-1). */
    if (std::isfinite(d) && std::trunc(d) == d && d >= 0 && d < kTwoPow64) {
      rc = msgpack_pack_uint64(pk, static_cast<uint64_t>(d));
    } else if (std::isfinite(d) && std::trunc(d) == d && d < 0 && d >= kInt64Min) {
      rc = msgpack_pack_int64(pk, static_cast<int64_t>(d));
    } else {
      rc = msgpack_pack_double(pk, d);
    }
  } else if (o->IsString()) {
    Nan::Utf8String bytes(o);
    rc = msgpack_pack_str(pk, bytes.length());
    if (rc == 0) {
      rc = msgpack_pack_str_body(pk, *bytes, bytes.length());
    }
  } else if (o->IsDate()) {
    /* Dates pack as their ISO-8601 string, as they did before 2.0.0. */
    v8::Local<v8::Object> date = o.As<v8::Object>();
    v8::Local<v8::Value> fn =
        Nan::Get(date, Nan::New("toISOString").ToLocalChecked()).ToLocalChecked();
    if (!fn->IsFunction()) {
      throw MsgpackException(Error("cannot pack Date"));
    }
    v8::Local<v8::Value> iso = CallNoArgs(date, fn.As<v8::Function>());
    Nan::Utf8String bytes(iso);
    rc = msgpack_pack_str(pk, bytes.length());
    if (rc == 0) {
      rc = msgpack_pack_str_body(pk, *bytes, bytes.length());
    }
  } else if (o->IsArray()) {
    PackArray(pk, o.As<v8::Array>(), depth);
    return;
  } else if (node::Buffer::HasInstance(o)) {
    char* data = node::Buffer::Data(o.As<v8::Object>());
    size_t len = node::Buffer::Length(o.As<v8::Object>());
    rc = msgpack_pack_bin(pk, len);
    if (rc == 0) {
      rc = msgpack_pack_bin_body(pk, data, len);
    }
  } else if (o->IsFunction()) {
    throw MsgpackException(Error("cannot pack function"));
  } else if (o->IsObject()) {
    PackObject(pk, o.As<v8::Object>(), depth);
    return;
  } else {
    throw MsgpackException(Error("cannot pack object"));
  }

  if (rc) {
    throw MsgpackException(Error("Error serializing object"));
  }
}

static v8::Local<v8::Value> MsgpackToJs(const msgpack_object* mo);

static v8::Local<v8::Value> MsgpackToJs(const msgpack_object* mo) {
  switch (mo->type) {
    case MSGPACK_OBJECT_NIL:
      return Nan::Null();
    case MSGPACK_OBJECT_BOOLEAN:
      return Nan::New(mo->via.boolean);
    case MSGPACK_OBJECT_POSITIVE_INTEGER:
      /* Values that fit in 2^53-1 stay as Number; larger become the
       * closest Number (legacy behavior). */
      return Nan::New<v8::Number>(static_cast<double>(mo->via.u64));
    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
      return Nan::New<v8::Number>(static_cast<double>(mo->via.i64));
    case MSGPACK_OBJECT_FLOAT32:
    case MSGPACK_OBJECT_FLOAT64:
      return Nan::New<v8::Number>(mo->via.f64);
    case MSGPACK_OBJECT_STR:
      if (mo->via.str.size == 0) {
        return Nan::New<v8::String>("").ToLocalChecked();
      }
      return Nan::New<v8::String>(mo->via.str.ptr, mo->via.str.size).ToLocalChecked();
    case MSGPACK_OBJECT_BIN:
      if (mo->via.bin.size == 0) {
        return Nan::NewBuffer(0).ToLocalChecked();
      }
      return Nan::CopyBuffer(mo->via.bin.ptr, mo->via.bin.size).ToLocalChecked();
    case MSGPACK_OBJECT_EXT:
      /* Fail closed on extension types: callers expecting core JSON-like
       * values should not silently receive opaque ext payloads. */
      throw MsgpackException(Error("cannot unpack ext type"));
    case MSGPACK_OBJECT_ARRAY: {
      v8::Local<v8::Array> arr = Nan::New<v8::Array>(mo->via.array.size);
      for (uint32_t i = 0; i < mo->via.array.size; i++) {
        Nan::Set(arr, i, MsgpackToJs(&mo->via.array.ptr[i]));
      }
      return arr;
    }
    case MSGPACK_OBJECT_MAP: {
      v8::Local<v8::Object> obj = Nan::New<v8::Object>();
      for (uint32_t i = 0; i < mo->via.map.size; i++) {
        const msgpack_object_kv* kv = &mo->via.map.ptr[i];
        v8::Local<v8::Value> key = MsgpackToJs(&kv->key);
        v8::Local<v8::Value> val = MsgpackToJs(&kv->val);
        Nan::Set(obj, key, val);
      }
      return obj;
    }
    default:
      throw MsgpackException(Error("Encountered unknown object type"));
  }
}

struct SbufPool {
  msgpack_sbuffer* list[kSbufferPoolMax];
  size_t length;
};

static SbufPool sbuf_pool = {{0}, 0};

class PackBuffer {
 public:
  PackBuffer() : sb_(NULL), from_pool_(false) {
    if (sbuf_pool.length > 0) {
      sb_ = sbuf_pool.list[--sbuf_pool.length];
      from_pool_ = true;
      msgpack_sbuffer_clear(sb_);
    } else {
      sb_ = msgpack_sbuffer_new();
      from_pool_ = false;
    }
    if (sb_ == NULL) {
      throw MsgpackException(Error("Error initializing packing buffer"));
    }
  }

  ~PackBuffer() {
    if (sb_ == NULL) return;
    if (from_pool_) {
      if (sbuf_pool.length == kSbufferPoolMax) {
        msgpack_sbuffer_free(sb_);
      } else {
        sbuf_pool.list[sbuf_pool.length++] = sb_;
      }
    } else {
      msgpack_sbuffer_free(sb_);
    }
    sb_ = NULL;
  }

  msgpack_sbuffer* get() { return sb_; }
  bool from_pool() const { return from_pool_; }

  char* release_data(size_t* size) {
    *size = sb_->size;
    char* data = msgpack_sbuffer_release(sb_);
    return data;
  }

 private:
  PackBuffer(const PackBuffer&);
  PackBuffer& operator=(const PackBuffer&);
  msgpack_sbuffer* sb_;
  bool from_pool_;
};

static void MsgpackFree(char* data, void* hint) {
  (void)hint;
  free(data);
}

static int remaining_bytes_in_buffer = 0;

NAN_METHOD(BytesRemaining) {
  info.GetReturnValue().Set(Nan::New<v8::Number>(remaining_bytes_in_buffer));
}

NAN_METHOD(Pack) {
  try {
    PackBuffer buf;
    msgpack_packer pk;
    msgpack_packer_init(&pk, buf.get(), msgpack_sbuffer_write);

    if (info.Length() == 1) {
      JsToMsgpack(&pk, info[0], 0);
    } else {
      if (msgpack_pack_array(&pk, info.Length())) {
        throw MsgpackException(Error("Error serializing object"));
      }
      for (int i = 0; i < info.Length(); i++) {
        JsToMsgpack(&pk, info[i], 0);
      }
    }

    if (buf.from_pool()) {
      info.GetReturnValue().Set(
          Nan::CopyBuffer(buf.get()->data, buf.get()->size).ToLocalChecked());
      return;
    }
    size_t size = 0;
    char* data = buf.release_data(&size);
    info.GetReturnValue().Set(
        Nan::NewBuffer(data, size, MsgpackFree, NULL).ToLocalChecked());
  } catch (const MsgpackException& e) {
    Nan::ThrowError(e.value());
  }
}

NAN_METHOD(Unpack) {
  if (info.Length() < 1 || !info[0]->IsObject() || !node::Buffer::HasInstance(info[0])) {
    return Nan::ThrowTypeError("First argument must be a Buffer");
  }

  v8::Local<v8::Object> buf = Nan::To<v8::Object>(info[0]).ToLocalChecked();
  char* data = node::Buffer::Data(buf);
  size_t len = node::Buffer::Length(buf);

  remaining_bytes_in_buffer = static_cast<int>(len);

  size_t consumed = 0;
  ScanStatus scan = ScanOne(data, len, &consumed);
  if (scan == kScanContinue) {
    remaining_bytes_in_buffer = static_cast<int>(len);
    info.GetReturnValue().Set(Nan::Null());
    return;
  }
  if (scan == kScanLimit) {
    return Nan::ThrowError("msgpack unpack limit exceeded");
  }
  if (scan == kScanParse) {
    return Nan::ThrowError("Encountered error unpacking buffer");
  }

  msgpack_unpacked result;
  msgpack_unpacked_init(&result);
  size_t off = 0;
  msgpack_unpack_return ret = msgpack_unpack_next(&result, data, len, &off);
  remaining_bytes_in_buffer = static_cast<int>(len - off);

  if (ret == MSGPACK_UNPACK_SUCCESS || ret == MSGPACK_UNPACK_EXTRA_BYTES) {
    try {
      v8::Local<v8::Value> v = MsgpackToJs(&result.data);
      msgpack_unpacked_destroy(&result);
      info.GetReturnValue().Set(v);
      return;
    } catch (const MsgpackException& e) {
      msgpack_unpacked_destroy(&result);
      return Nan::ThrowError(e.value());
    }
  }

  msgpack_unpacked_destroy(&result);
  if (ret == MSGPACK_UNPACK_CONTINUE) {
    remaining_bytes_in_buffer = static_cast<int>(len);
    info.GetReturnValue().Set(Nan::Null());
    return;
  }
  Nan::ThrowError("Encountered error unpacking buffer");
}

NAN_MODULE_INIT(Init) {
  stack_key.Reset(Nan::New("_msgpack_stack").ToLocalChecked());
  Nan::Set(target, Nan::New("pack").ToLocalChecked(),
           Nan::GetFunction(Nan::New<v8::FunctionTemplate>(Pack)).ToLocalChecked());
  Nan::Set(target, Nan::New("unpack").ToLocalChecked(),
           Nan::GetFunction(Nan::New<v8::FunctionTemplate>(Unpack)).ToLocalChecked());
  Nan::Set(target, Nan::New("bytesRemaining").ToLocalChecked(),
           Nan::GetFunction(Nan::New<v8::FunctionTemplate>(BytesRemaining)).ToLocalChecked());
}

NODE_MODULE(msgpackBinding, Init)

}  // namespace
