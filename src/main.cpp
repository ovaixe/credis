#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "server.h"
#include "store/dict.h"
#include "util/log.h"
#include "util/strings.h"
#include "util/time.h"

namespace credis {
namespace {

Server* g_server = nullptr;

// Only async-signal-safe work happens here: EventLoop::stop() writes a byte to an
// eventfd, which wakes epoll_wait so the loop can exit on the main thread.
extern "C" void handle_shutdown_signal(int) {
  if (g_server != nullptr) g_server->stop();
}

void print_usage() {
  fprintf(stderr,
          "Usage: credis-server [/path/to/credis.conf] [options]\n"
          "\n"
          "Options:\n"
          "  --port <n>            Port to listen on (default 6379)\n"
          "  --bind <addr>         Address to bind (default 127.0.0.1, '*' for all)\n"
          "  --databases <n>       Number of databases (default 16)\n"
          "  --maxclients <n>      Maximum concurrent clients (default 10000)\n"
          "  --loglevel <level>    debug | verbose | notice | warning\n"
          "  --hz <n>              Background task frequency (default 10)\n"
          "  --dir <path>          Working directory\n"
          "  --help                Show this message\n");
}

bool parse_log_level(const std::string& value, LogLevel* out) {
  if (value == "debug") *out = LogLevel::Debug;
  else if (value == "verbose") *out = LogLevel::Verbose;
  else if (value == "notice") *out = LogLevel::Notice;
  else if (value == "warning") *out = LogLevel::Warning;
  else return false;
  return true;
}

// Applies one "name value" setting from either the config file or the command
// line. Returns false with *error set on bad input.
bool apply_setting(ServerConfig* config, const std::string& name, const std::string& value,
                   std::string* error) {
  int64_t number = 0;
  const bool numeric = string2ll(value, &number);

  if (name == "port") {
    if (!numeric || number < 0 || number > 65535) {
      *error = "invalid port";
      return false;
    }
    config->port = static_cast<uint16_t>(number);
  } else if (name == "bind") {
    config->bind_addr = value;
  } else if (name == "databases") {
    if (!numeric || number < 1 || number > 16384) {
      *error = "invalid number of databases";
      return false;
    }
    config->databases = static_cast<int>(number);
  } else if (name == "maxclients") {
    if (!numeric || number < 1) {
      *error = "invalid maxclients";
      return false;
    }
    config->maxclients = static_cast<size_t>(number);
  } else if (name == "hz") {
    if (!numeric || number < 1 || number > 500) {
      *error = "invalid hz, must be between 1 and 500";
      return false;
    }
    config->hz = static_cast<int>(number);
  } else if (name == "tcp-backlog") {
    if (!numeric || number < 1) {
      *error = "invalid tcp-backlog";
      return false;
    }
    config->tcp_backlog = static_cast<int>(number);
  } else if (name == "loglevel") {
    if (!parse_log_level(value, &config->loglevel)) {
      *error = "invalid loglevel";
      return false;
    }
  } else if (name == "logfile") {
    config->logfile = value;
  } else if (name == "dir") {
    config->dir = value;
  } else if (name == "appendonly" || name == "save" || name == "daemonize" ||
             name == "protected-mode" || name == "maxmemory") {
    // Recognized but unimplemented: accepted so an existing redis.conf loads.
  } else {
    *error = "unknown setting '" + name + "'";
    return false;
  }
  return true;
}

bool load_config_file(const std::string& path, ServerConfig* config, std::string* error) {
  std::ifstream file(path);
  if (!file) {
    *error = "cannot open config file '" + path + "'";
    return false;
  }

  std::string line;
  int line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    // Strip comments and skip blank lines.
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);

    std::vector<std::string> tokens;
    if (!split_args(line, &tokens)) {
      *error = "unbalanced quotes on line " + ll2string(line_number);
      return false;
    }
    if (tokens.empty()) continue;
    if (tokens.size() < 2) {
      *error = "missing value for '" + tokens[0] + "' on line " + ll2string(line_number);
      return false;
    }

    // Multi-word values (e.g. "bind 127.0.0.1 ::1") are rejoined.
    std::string value = tokens[1];
    for (size_t i = 2; i < tokens.size(); ++i) value += " " + tokens[i];

    std::string setting_error;
    if (!apply_setting(config, to_lower(tokens[0]), value, &setting_error)) {
      *error = setting_error + " on line " + ll2string(line_number);
      return false;
    }
  }
  return true;
}

bool parse_command_line(int argc, char** argv, ServerConfig* config, std::string* error) {
  int i = 1;
  // A bare first argument is the config file path, as in redis-server.
  if (argc > 1 && argv[1][0] != '-') {
    if (!load_config_file(argv[1], config, error)) return false;
    i = 2;
  }

  for (; i < argc; ++i) {
    std::string option = argv[i];
    if (option == "--help" || option == "-h") {
      print_usage();
      std::exit(0);
    }
    if (option.rfind("--", 0) != 0) {
      *error = "unexpected argument '" + option + "'";
      return false;
    }
    if (i + 1 >= argc) {
      *error = "missing value for '" + option + "'";
      return false;
    }
    if (!apply_setting(config, to_lower(option.substr(2)), argv[i + 1], error)) return false;
    ++i;
  }
  return true;
}

}  // namespace
}  // namespace credis

int main(int argc, char** argv) {
  using namespace credis;

  ServerConfig config;
  std::string error;
  if (!parse_command_line(argc, argv, &config, &error)) {
    fprintf(stderr, "credis: %s\n", error.c_str());
    return 1;
  }
  set_log_level(config.loglevel);

  // Randomize the hash seed before any key is stored, so bucket placement is not
  // predictable from outside the process.
  set_hash_seed(static_cast<uint64_t>(ustime()) ^ (static_cast<uint64_t>(getpid()) << 32));

  // A write to a disconnected client must surface as EPIPE, not kill the server.
  // Sends also pass MSG_NOSIGNAL; this covers everything else.
  signal(SIGPIPE, SIG_IGN);

  Server server(std::move(config));
  g_server = &server;

  struct sigaction action {};
  action.sa_handler = handle_shutdown_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;  // no SA_RESTART: let epoll_wait return EINTR
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  if (!server.start(&error)) {
    CREDIS_LOG_WARNING("Failed to start: {}", error);
    return 1;
  }

  server.run();

  g_server = nullptr;
  CREDIS_LOG_NOTICE("credis is now ready to exit, bye bye...");
  return 0;
}
