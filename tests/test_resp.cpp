#include <random>
#include <string>
#include <vector>

#include "harness.h"
#include "net/buffer.h"
#include "resp/reader.h"
#include "resp/writer.h"
#include "util/strings.h"

using namespace credis;

namespace {

// Parses one command from a complete request.
RespReader::Result parse_all(RespReader& reader, Buffer& buffer) {
  return reader.parse(buffer);
}

std::string encode_command(const std::vector<std::string>& args) {
  std::string out = "*" + ll2string(static_cast<int64_t>(args.size())) + "\r\n";
  for (const std::string& arg : args) {
    out += "$" + ll2string(static_cast<int64_t>(arg.size())) + "\r\n" + arg + "\r\n";
  }
  return out;
}

}  // namespace

TEST(RespReader, ParsesMultibulkCommand) {
  Buffer buffer;
  buffer.append(encode_command({"SET", "key", "value"}));

  RespReader reader;
  const auto result = parse_all(reader, buffer);
  CHECK(result.status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv().size(), 3u);
  CHECK_BYTES(reader.argv()[0], "SET");
  CHECK_BYTES(reader.argv()[1], "key");
  CHECK_BYTES(reader.argv()[2], "value");
  CHECK(buffer.empty());
}

// The parser must survive a command arriving one byte at a time — this is the
// property that makes it safe on a non-blocking socket.
TEST(RespReader, IsIncrementalByteByByte) {
  const std::string request = encode_command({"SET", "hello", "world"});
  Buffer buffer;
  RespReader reader;

  for (size_t i = 0; i + 1 < request.size(); ++i) {
    buffer.append(&request[i], 1);
    const auto result = reader.parse(buffer);
    if (result.status != RespReader::Status::Incomplete) {
      CREDIS_FAIL("parser reported completion after only " + ll2string(static_cast<int64_t>(i + 1)) +
                  " of " + ll2string(static_cast<int64_t>(request.size())) + " bytes");
    }
  }

  buffer.append(&request.back(), 1);
  const auto result = reader.parse(buffer);
  CHECK(result.status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv().size(), 3u);
  CHECK_BYTES(reader.argv()[2], "world");
}

TEST(RespReader, ParsesPipelinedCommands) {
  Buffer buffer;
  buffer.append(encode_command({"PING"}));
  buffer.append(encode_command({"GET", "a"}));
  buffer.append(encode_command({"DEL", "a", "b"}));

  RespReader reader;
  std::vector<size_t> arg_counts;
  for (int i = 0; i < 3; ++i) {
    const auto result = reader.parse(buffer);
    CHECK(result.status == RespReader::Status::Ok);
    arg_counts.push_back(reader.argv().size());
  }
  CHECK_EQ(arg_counts, (std::vector<size_t>{1, 2, 3}));
  CHECK(buffer.empty());
  CHECK(reader.parse(buffer).status == RespReader::Status::Incomplete);
}

TEST(RespReader, HandlesBinaryPayloads) {
  const std::string binary("a\0b\r\nc", 6);
  Buffer buffer;
  buffer.append(encode_command({"SET", "k", binary}));

  RespReader reader;
  CHECK(parse_all(reader, buffer).status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv()[2].size(), 6u);
  CHECK(reader.argv()[2] == binary);
}

TEST(RespReader, EmptyArgumentsAreValid) {
  Buffer buffer;
  buffer.append(encode_command({"SET", "", ""}));
  RespReader reader;
  CHECK(parse_all(reader, buffer).status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv().size(), 3u);
  CHECK(reader.argv()[1].empty());
}

TEST(RespReader, EmptyMultibulkYieldsNoCommand) {
  Buffer buffer;
  buffer.append("*0\r\n");
  RespReader reader;
  CHECK(parse_all(reader, buffer).status == RespReader::Status::Ok);
  CHECK(reader.argv().empty());

  buffer.append("*-1\r\n");
  CHECK(parse_all(reader, buffer).status == RespReader::Status::Ok);
  CHECK(reader.argv().empty());
}

TEST(RespReader, ParsesInlineCommands) {
  Buffer buffer;
  buffer.append("PING\r\n");
  RespReader reader;
  CHECK(parse_all(reader, buffer).status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv().size(), 1u);
  CHECK_BYTES(reader.argv()[0], "PING");

  // Bare LF is accepted too, so `nc` without CRLF still works.
  buffer.append("ECHO hi\n");
  CHECK(parse_all(reader, buffer).status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv().size(), 2u);
  CHECK_BYTES(reader.argv()[1], "hi");

  // Quoted arguments keep their spaces.
  buffer.append("SET k \"a b c\"\r\n");
  CHECK(parse_all(reader, buffer).status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv().size(), 3u);
  CHECK_BYTES(reader.argv()[2], "a b c");

  // Blank lines are well-formed but carry no command.
  buffer.append("\r\n");
  CHECK(parse_all(reader, buffer).status == RespReader::Status::Ok);
  CHECK(reader.argv().empty());
}

TEST(RespReader, ReportsProtocolErrors) {
  struct Case {
    const char* input;
    const char* expected_error;
  };
  const Case cases[] = {
      {"*abc\r\n", "ERR Protocol error: invalid multibulk length"},
      {"*3\r\n+PING\r\n", "ERR Protocol error: expected '$', got '+'"},
      {"*1\r\n$abc\r\n", "ERR Protocol error: invalid bulk length"},
      {"*1\r\n$-1\r\n", "ERR Protocol error: invalid bulk length"},
      {"*1\r\n$999999999999\r\n", "ERR Protocol error: invalid bulk length"},
      {"*99999999\r\n", "ERR Protocol error: invalid multibulk length"},
      {"SET \"unbalanced\r\n", "ERR Protocol error: unbalanced quotes in request"},
  };

  for (const Case& test_case : cases) {
    Buffer buffer;
    buffer.append(test_case.input);
    RespReader reader;
    const auto result = reader.parse(buffer);
    if (result.status != RespReader::Status::ProtocolError) {
      CREDIS_FAIL(std::string("expected a protocol error for input \"") +
                  testing::escape(test_case.input) + "\"");
    }
    CHECK_BYTES(result.error, test_case.expected_error);
  }
}

TEST(RespReader, RejectsOversizedHeaders) {
  // A multibulk header longer than the inline limit must be rejected rather than
  // buffered forever.
  Buffer buffer;
  buffer.append("*");
  buffer.append(std::string(kMaxInlineLen + 10, '1'));
  RespReader reader;
  const auto result = reader.parse(buffer);
  CHECK(result.status == RespReader::Status::ProtocolError);
  CHECK_BYTES(result.error, "ERR Protocol error: too big mbulk count string");
}

TEST(RespReader, ResetClearsPartialState) {
  Buffer buffer;
  buffer.append("*3\r\n$3\r\nSET\r\n");
  RespReader reader;
  CHECK(reader.parse(buffer).status == RespReader::Status::Incomplete);

  reader.reset();
  buffer.consume_all();
  buffer.append(encode_command({"PING"}));
  CHECK(reader.parse(buffer).status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv().size(), 1u);
  CHECK_BYTES(reader.argv()[0], "PING");
}

// Random bytes must never crash, hang, or consume more than was written. The
// parser is the only code reachable before authentication of any kind, so it
// gets the roughest input.
TEST(RespReader, FuzzRandomBytesTerminatesCleanly) {
  std::mt19937 rng(0xC0FFEE);
  // A byte alphabet weighted toward protocol-significant characters finds more
  // interesting states than uniform random noise.
  const std::string alphabet = "*$\r\n0123456789-+ abcXYZ\"'\\";

  for (int iteration = 0; iteration < 20000; ++iteration) {
    const size_t length = rng() % 64;
    std::string input;
    input.reserve(length);
    for (size_t i = 0; i < length; ++i) {
      input.push_back(alphabet[rng() % alphabet.size()]);
    }

    Buffer buffer;
    buffer.append(input);
    RespReader reader;

    // Parse until the reader stops making progress.
    for (int step = 0; step < 100; ++step) {
      const size_t before = buffer.readable_bytes();
      const auto result = reader.parse(buffer);
      CHECK_LE(buffer.readable_bytes(), before);
      if (result.status != RespReader::Status::Ok) break;
      if (buffer.readable_bytes() == before) break;  // no progress: stop
    }
  }
}

TEST(RespReader, FuzzSplitDeliveryMatchesWholeDelivery) {
  std::mt19937 rng(0xBEEF);

  for (int iteration = 0; iteration < 2000; ++iteration) {
    // Build a valid random command.
    const size_t argc = 1 + rng() % 5;
    std::vector<std::string> args;
    for (size_t i = 0; i < argc; ++i) {
      const size_t length = rng() % 20;
      std::string arg;
      for (size_t j = 0; j < length; ++j) arg.push_back(static_cast<char>(rng() % 256));
      args.push_back(std::move(arg));
    }
    const std::string request = encode_command(args);

    // Delivered whole.
    Buffer whole;
    whole.append(request);
    RespReader whole_reader;
    CHECK(whole_reader.parse(whole).status == RespReader::Status::Ok);
    const std::vector<std::string> whole_argv = whole_reader.argv();

    // Delivered in random chunks: the result must be identical.
    Buffer split;
    RespReader split_reader;
    std::vector<std::string> split_argv;
    size_t offset = 0;
    while (offset < request.size()) {
      const size_t chunk = 1 + rng() % 7;
      const size_t take = std::min(chunk, request.size() - offset);
      split.append(request.data() + offset, take);
      offset += take;
      const auto result = split_reader.parse(split);
      if (result.status == RespReader::Status::Ok) {
        split_argv = split_reader.argv();
      }
    }
    CHECK_EQ(split_argv, whole_argv);
  }
}

// --- writer -------------------------------------------------------------------

TEST(RespWriter, Resp2Encodings) {
  Buffer buffer;
  RespWriter writer(buffer, RespProtocol::Resp2);

  writer.simple_string("OK");
  CHECK_BYTES(buffer.view(), "+OK\r\n");
  buffer.consume_all();

  writer.error("ERR bad thing");
  CHECK_BYTES(buffer.view(), "-ERR bad thing\r\n");
  buffer.consume_all();

  writer.integer(42);
  CHECK_BYTES(buffer.view(), ":42\r\n");
  buffer.consume_all();

  writer.integer(-1);
  CHECK_BYTES(buffer.view(), ":-1\r\n");
  buffer.consume_all();

  writer.bulk("hello");
  CHECK_BYTES(buffer.view(), "$5\r\nhello\r\n");
  buffer.consume_all();

  writer.bulk("");
  CHECK_BYTES(buffer.view(), "$0\r\n\r\n");
  buffer.consume_all();

  writer.null_bulk();
  CHECK_BYTES(buffer.view(), "$-1\r\n");
  buffer.consume_all();

  writer.null_array();
  CHECK_BYTES(buffer.view(), "*-1\r\n");
  buffer.consume_all();

  writer.array(2);
  writer.bulk("a");
  writer.bulk("b");
  CHECK_BYTES(buffer.view(), "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
  buffer.consume_all();

  // RESP2 flattens maps into an array of 2n elements.
  writer.map(1);
  writer.bulk("f");
  writer.bulk("v");
  CHECK_BYTES(buffer.view(), "*2\r\n$1\r\nf\r\n$1\r\nv\r\n");
  buffer.consume_all();

  writer.set(2);
  CHECK_BYTES(buffer.view(), "*2\r\n");
  buffer.consume_all();

  writer.double_value(3.5);
  CHECK_BYTES(buffer.view(), "$3\r\n3.5\r\n");
  buffer.consume_all();

  writer.boolean(true);
  CHECK_BYTES(buffer.view(), ":1\r\n");
  buffer.consume_all();

  writer.verbatim("txt body");
  CHECK_BYTES(buffer.view(), "$8\r\ntxt body\r\n");
  buffer.consume_all();
}

TEST(RespWriter, Resp3Encodings) {
  Buffer buffer;
  RespWriter writer(buffer, RespProtocol::Resp3);

  writer.null_bulk();
  CHECK_BYTES(buffer.view(), "_\r\n");
  buffer.consume_all();

  writer.null_array();
  CHECK_BYTES(buffer.view(), "_\r\n");
  buffer.consume_all();

  writer.map(1);
  writer.bulk("f");
  writer.bulk("v");
  CHECK_BYTES(buffer.view(), "%1\r\n$1\r\nf\r\n$1\r\nv\r\n");
  buffer.consume_all();

  writer.set(2);
  CHECK_BYTES(buffer.view(), "~2\r\n");
  buffer.consume_all();

  writer.double_value(3.5);
  CHECK_BYTES(buffer.view(), ",3.5\r\n");
  buffer.consume_all();

  writer.double_value(3.0);
  CHECK_BYTES(buffer.view(), ",3\r\n");
  buffer.consume_all();

  writer.boolean(false);
  CHECK_BYTES(buffer.view(), "#f\r\n");
  buffer.consume_all();

  // The verbatim length covers the "txt:" prefix.
  writer.verbatim("body");
  CHECK_BYTES(buffer.view(), "=8\r\ntxt:body\r\n");
  buffer.consume_all();
}

TEST(RespWriter, RoundTripsThroughTheReader) {
  // A bulk reply written by the writer must parse back as the same bytes when
  // wrapped in a request frame.
  Buffer out;
  RespWriter writer(out, RespProtocol::Resp2);
  const std::string payload("bytes\0with\r\nspecials", 20);
  writer.bulk(payload);

  Buffer request;
  request.append("*2\r\n$4\r\nECHO\r\n");
  request.append(out.view());

  RespReader reader;
  CHECK(reader.parse(request).status == RespReader::Status::Ok);
  CHECK_EQ(reader.argv().size(), 2u);
  CHECK(reader.argv()[1] == payload);
}
