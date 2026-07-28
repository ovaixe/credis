#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace credis {

// Commands operate on a connection; `Client` is the name used throughout the
// command layer, matching how Redis's own command implementations read.
class Connection;
using Client = Connection;

using CommandProc = void (*)(Client&);

enum CommandFlags : uint32_t {
  kCmdWrite = 1u << 0,     // may modify the keyspace
  kCmdReadonly = 1u << 1,  // never modifies the keyspace
  kCmdAdmin = 1u << 2,     // administrative (CONFIG, SHUTDOWN, DEBUG)
  kCmdFast = 1u << 3,      // O(1) or close to it
  kCmdNoDb = 1u << 4,      // does not touch the selected database
};

struct Command {
  std::string_view name;
  CommandProc proc;
  // Total argument count including the command name. Negative means "at least
  // this many", following Redis's convention.
  int arity;
  uint32_t flags;
  // Key positions, for introspection via COMMAND. 0 means the command takes no
  // keys at fixed positions.
  int first_key;
  int last_key;
  int key_step;
};

// Case-insensitive lookup. Returns nullptr for unknown commands.
const Command* lookup_command(std::string_view name);

// Every registered command, in table order.
const std::vector<Command>& all_commands();

// True when `argc` satisfies the command's arity rule.
bool arity_ok(const Command& command, size_t argc);

// Canonical Redis error strings. Clients match on these, so they are copied
// verbatim rather than paraphrased.
namespace err {
inline constexpr std::string_view kWrongType =
    "WRONGTYPE Operation against a key holding the wrong kind of value";
inline constexpr std::string_view kNotInteger = "ERR value is not an integer or out of range";
inline constexpr std::string_view kNotFloat = "ERR value is not a valid float";
inline constexpr std::string_view kSyntax = "ERR syntax error";
inline constexpr std::string_view kIndexOutOfRange = "ERR index out of range";
inline constexpr std::string_view kNoSuchKey = "ERR no such key";
inline constexpr std::string_view kSourceDestSame = "ERR source and destination objects are the same";
inline constexpr std::string_view kOutOfRange = "ERR value is out of range, must be positive";
inline constexpr std::string_view kSelectOutOfRange = "ERR DB index is out of range";
inline constexpr std::string_view kInvalidExpire = "ERR invalid expire time";
inline constexpr std::string_view kIncrOverflow = "ERR increment or decrement would overflow";
inline constexpr std::string_view kNanResult = "ERR resulting score is not a number (NaN)";
inline constexpr std::string_view kMinOrMaxNotFloat = "ERR min or max is not a float";
inline constexpr std::string_view kMinOrMaxNotValid =
    "ERR min or max not valid string range item";
inline constexpr std::string_view kBitOutOfRange =
    "ERR bit offset is not an integer or out of range";
}  // namespace err

// --- shared reply helpers ---

void reply_wrong_type(Client& client);
void reply_wrong_args(Client& client, std::string_view command_name);
// Replies with an error naming the subcommand, e.g.
// "ERR Unknown CLIENT subcommand or wrong number of arguments for 'foo'".
void reply_unknown_subcommand(Client& client, std::string_view container,
                              std::string_view subcommand);

}  // namespace credis
