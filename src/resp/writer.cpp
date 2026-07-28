#include "resp/writer.h"

#include <charconv>

#include "util/strings.h"

namespace credis {

void RespWriter::append_length(char prefix, int64_t value) {
  char buf[24];
  buf[0] = prefix;
  const auto res = std::to_chars(buf + 1, buf + sizeof(buf) - 2, value);
  *res.ptr = '\r';
  *(res.ptr + 1) = '\n';
  out_->append(buf, static_cast<size_t>(res.ptr - buf) + 2);
}

void RespWriter::simple_string(std::string_view s) {
  out_->append('+');
  out_->append(s);
  out_->append("\r\n");
}

void RespWriter::error(std::string_view message) {
  out_->append('-');
  out_->append(message);
  out_->append("\r\n");
}

void RespWriter::integer(int64_t value) { append_length(':', value); }

void RespWriter::bulk(std::string_view s) {
  append_length('$', static_cast<int64_t>(s.size()));
  out_->append(s);
  out_->append("\r\n");
}

void RespWriter::null_bulk() {
  if (protocol_ == RespProtocol::Resp3) {
    out_->append("_\r\n");
  } else {
    out_->append("$-1\r\n");
  }
}

void RespWriter::null_array() {
  if (protocol_ == RespProtocol::Resp3) {
    out_->append("_\r\n");
  } else {
    out_->append("*-1\r\n");
  }
}

void RespWriter::array(int64_t count) { append_length('*', count); }

void RespWriter::map(int64_t pair_count) {
  if (protocol_ == RespProtocol::Resp3) {
    append_length('%', pair_count);
  } else {
    append_length('*', pair_count * 2);
  }
}

void RespWriter::set(int64_t count) {
  append_length(protocol_ == RespProtocol::Resp3 ? '~' : '*', count);
}

void RespWriter::double_value(double value) {
  if (protocol_ == RespProtocol::Resp3) {
    const std::string text = d2string(value);
    out_->append(',');
    out_->append(text);
    out_->append("\r\n");
  } else {
    bulk(d2string(value));
  }
}

void RespWriter::boolean(bool value) {
  if (protocol_ == RespProtocol::Resp3) {
    out_->append(value ? "#t\r\n" : "#f\r\n");
  } else {
    integer(value ? 1 : 0);
  }
}

void RespWriter::verbatim(std::string_view s, std::string_view format) {
  if (protocol_ == RespProtocol::Resp3) {
    // =<len>\r\n<fmt>:<payload>\r\n — the length covers the "txt:" prefix too.
    append_length('=', static_cast<int64_t>(s.size() + 4));
    out_->append(format.substr(0, 3));
    out_->append(':');
    out_->append(s);
    out_->append("\r\n");
  } else {
    bulk(s);
  }
}

}  // namespace credis
