#pragma once

#include <cstdint>
#include <string_view>

#include "net/buffer.h"

namespace credis {

enum class RespProtocol : int { Resp2 = 2, Resp3 = 3 };

// Serializes replies straight into a connection's output buffer — there is no
// intermediate reply object, which keeps the hot path allocation-free.
//
// RESP3 only changes a handful of type encodings; everything else is
// byte-identical, so the protocol version is branched on exactly where it
// matters (nulls, maps, sets, doubles, booleans, verbatim strings).
class RespWriter {
 public:
  RespWriter(Buffer& out, RespProtocol protocol) : out_(&out), protocol_(protocol) {}

  RespProtocol protocol() const { return protocol_; }
  void set_protocol(RespProtocol protocol) { protocol_ = protocol; }

  // --- simple scalars ---
  void simple_string(std::string_view s);  // +OK\r\n
  void ok() { simple_string("OK"); }
  void error(std::string_view message);  // -ERR ...\r\n
  void integer(int64_t value);           // :42\r\n
  void bulk(std::string_view s);         // $3\r\nfoo\r\n

  // --- nulls ---
  // RESP2 distinguishes a null bulk ($-1) from a null array (*-1); RESP3 uses a
  // single "_" for both. Callers should pick the one matching the reply shape so
  // RESP2 clients see what real Redis sends.
  void null_bulk();
  void null_array();

  // --- aggregates: emit the header, then exactly `count` elements ---
  void array(int64_t count);  // *n\r\n
  // n field/value *pairs*: RESP2 flattens to an array of 2n items, RESP3 uses %n.
  void map(int64_t pair_count);
  void set(int64_t count);  // RESP2 *n, RESP3 ~n

  // --- RESP3-typed scalars, degrading to RESP2 spellings ---
  void double_value(double value);  // RESP2 bulk string, RESP3 ,3.5\r\n
  void boolean(bool value);         // RESP2 :1/:0, RESP3 #t/#f
  void verbatim(std::string_view s, std::string_view format = "txt");

  // Raw pre-encoded protocol bytes, for replies built elsewhere.
  void raw(std::string_view bytes) { out_->append(bytes); }

  Buffer& buffer() { return *out_; }

 private:
  void append_length(char prefix, int64_t value);

  Buffer* out_;
  RespProtocol protocol_;
};

}  // namespace credis
