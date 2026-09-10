// Wrap a nicer JavaScript API around the MessagePack native bindings.

'use strict';

const buffer = require('buffer');
const events = require('events');
const util = require('util');

const mpBindings = require(__dirname + '/../build/Release/msgpackBinding');

const bpack = mpBindings.pack;
const rawUnpack = mpBindings.unpack;

function pack() {
  const args = new Array(arguments.length);
  for (let i = 0; i < arguments.length; i++) {
    const that = arguments[i];
    if (that && typeof that === 'object' && typeof that.toJSON === 'function') {
      args[i] = that.toJSON();
    } else {
      args[i] = that;
    }
  }
  return bpack.apply(null, args);
}

function unpack(buf) {
  const result = rawUnpack(buf);
  unpack.bytes_remaining = mpBindings.bytesRemaining();
  return result;
}

unpack.bytes_remaining = 0;

function Stream(s) {
  const self = this;
  events.EventEmitter.call(self);
  self.buf = null;

  self.send = function (m) {
    const args = [pack(m)];
    for (let i = 1; i < arguments.length; i++) {
      args.push(arguments[i]);
    }
    return s.write.apply(s, args);
  };

  s.addListener('data', function (d) {
    if (self.buf) {
      const b = buffer.Buffer.allocUnsafe(self.buf.length + d.length);
      self.buf.copy(b, 0, 0, self.buf.length);
      d.copy(b, self.buf.length, 0, d.length);
      self.buf = b;
    } else {
      self.buf = d;
    }

    while (self.buf && self.buf.length > 0) {
      let msg;
      try {
        msg = unpack(self.buf);
      } catch (err) {
        /* A malformed or oversized packet is unrecoverable for this stream:
         * drop the buffer rather than re-parsing the same bomb forever. */
        self.buf = null;
        self.emit('error', err);
        return;
      }
      /* null means "incomplete" only when nothing was consumed; a decoded
       * nil (0xc0) is a real message and must be emitted. */
      if (msg === null && unpack.bytes_remaining === self.buf.length) {
        break;
      }
      self.emit('msg', msg);
      if (unpack.bytes_remaining > 0) {
        self.buf = self.buf.slice(self.buf.length - unpack.bytes_remaining);
      } else {
        self.buf = null;
      }
    }
  });
}

util.inherits(Stream, events.EventEmitter);

exports.pack = pack;
exports.unpack = unpack;
exports.Stream = Stream;
