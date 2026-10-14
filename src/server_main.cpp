#include "osats/server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
osats::Server* g_server = nullptr;

void onSignal(int) {
  if (g_server) g_server->requestStop();
}
}  // namespace

int main(int argc, char** argv) {
  osats::ServerConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
      config.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if ((arg == "--http-port") && i + 1 < argc) {
      config.http_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--no-http") {
      config.enable_http = false;
    } else if (arg == "--verbose" || arg == "-v") {
      config.verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: osats-server [--port N] [--http-port N] [--no-http] [--verbose]\n";
      return 0;
    } else if (arg[0] != '-') {
      config.port = static_cast<std::uint16_t>(std::atoi(arg.c_str()));
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      return 2;
    }
  }

  try {
    osats::Server server(config);
    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    server.run();
  } catch (const std::exception& ex) {
    std::cerr << "fatal: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
