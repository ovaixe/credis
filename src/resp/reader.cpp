#include "resp/reader.h"

#include <format>

#include "util/strings.h"

namespace credis {
namespace {

RespReader::Result incomplete() { return {RespReader::Status::Incomplete, {}}; }
RespReader::Result ok() { return {RespReader::Status::Ok, {}}; }
RespReader::Result protocol_error(std::string message) {
  return {RespReader::Status::ProtocolError, std::move(message)};
}

}  // namespace

void RespReader::reset() {
  argv_.clear();
  multibulk_remaining_ = 0;
  bulk_len_ = -1;
}

RespReader::Result RespReader::parse(Buffer& in) {
  // Mid-command: keep filling the multibulk we already started.
  if (multibulk_remaining_ > 0) return parse_multibulk(in);

  if (in.empty()) return incomplete();
  if (in.peek()[0] == '*') return parse_multibulk(in);
  return parse_inline(in);
}

RespReader::Result RespReader::parse_inline(Buffer& in) {
  const size_t newline = in.find_lf();
  if (newline == Buffer::kNpos) {
    if (in.readable_bytes() > kMaxInlineLen) {
      return protocol_error("ERR Protocol error: too big inline request");
    }
    return incomplete();
  }

  // Accept both "\n" and "\r\n" terminators.
  size_t line_len = newline;
  if (line_len > 0 && in.peek()[line_len - 1] == '\r') --line_len;

  const std::string_view line(in.peek(), line_len);
  argv_.clear();
  if (!split_args(line, &argv_)) {
    // Consume the bad line so the error reply is not followed by a re-parse.
    in.consume(newline + 1);
    return protocol_error("ERR Protocol error: unbalanced quotes in request");
  }
  in.consume(newline + 1);
  return ok();
}

RespReader::Result RespReader::parse_multibulk(Buffer& in) {
  // Step 1: the "*<count>\r\n" header, unless we are resuming mid-command.
  if (multibulk_remaining_ == 0) {
    const size_t crlf = in.find_crlf();
    if (crlf == Buffer::kNpos) {
      if (in.readable_bytes() > kMaxInlineLen) {
        return protocol_error("ERR Protocol error: too big mbulk count string");
      }
      return incomplete();
    }

    int64_t count = 0;
    const std::string_view digits(in.peek() + 1, crlf - 1);
    if (!string2ll(digits, &count) || count > kMaxMultibulkLen) {
      return protocol_error("ERR Protocol error: invalid multibulk length");
    }
    in.consume(crlf + 2);

    if (count <= 0) {
      // "*0\r\n" and "*-1\r\n" are well-formed but carry no command.
      argv_.clear();
      return ok();
    }

    argv_.clear();
    argv_.reserve(static_cast<size_t>(count));
    multibulk_remaining_ = count;
    bulk_len_ = -1;
  }

  // Step 2: one "$<len>\r\n<payload>\r\n" per remaining argument.
  while (multibulk_remaining_ > 0) {
    if (bulk_len_ < 0) {
      const size_t crlf = in.find_crlf();
      if (crlf == Buffer::kNpos) {
        if (in.readable_bytes() > kMaxInlineLen) {
          return protocol_error("ERR Protocol error: too big bulk count string");
        }
        return incomplete();
      }
      if (in.peek()[0] != '$') {
        return protocol_error(
            std::format("ERR Protocol error: expected '$', got '{}'", in.peek()[0]));
      }

      int64_t len = 0;
      const std::string_view digits(in.peek() + 1, crlf - 1);
      if (!string2ll(digits, &len) || len < 0 || len > kMaxBulkLen) {
        return protocol_error("ERR Protocol error: invalid bulk length");
      }
      in.consume(crlf + 2);
      bulk_len_ = len;
    }

    // Payload plus its trailing CRLF must have arrived in full.
    const size_t needed = static_cast<size_t>(bulk_len_) + 2;
    if (in.readable_bytes() < needed) return incomplete();

    argv_.emplace_back(in.peek(), static_cast<size_t>(bulk_len_));
    in.consume(needed);
    bulk_len_ = -1;
    --multibulk_remaining_;
  }

  return ok();
}

}  // namespace credis
