#include "osats/client.hpp"
#include "osats/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// A human pilot tuned to KSFO tower receives a clearance from KSFO_TWR
// standing at the field. Killing the tower process drops that controller
// and frees the callsign. Scripted UNICOM traffic stays in the same world.

namespace {

constexpr std::uint32_t kTowerHz = 120500;
constexpr double kFieldLat = 37.618806;
constexpr double kFieldLon = -122.375417;

void fail(const std::string& msg) { throw std::runtime_error(msg); }

std::uint16_t freePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) fail("socket");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    fail("bind probe");
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    ::close(fd);
    fail("getsockname");
  }
  const auto port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

template <typename Pred>
bool waitFor(Pred pred, int ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

struct Child {
  pid_t pid = -1;
  int read_fd = -1;
  std::thread reader;
  std::mutex mu;
  std::vector<std::string> lines;
  bool eof = false;

  ~Child() { stop(); }

  void stop() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      pid = -1;
    }
    if (reader.joinable()) reader.join();
    if (read_fd >= 0) {
      ::close(read_fd);
      read_fd = -1;
    }
  }

  std::vector<std::string> snapshot() {
    std::lock_guard lock(mu);
    return lines;
  }
};

void startProcess(Child& child, const std::string& bin, std::uint16_t port) {
  int pipefd[2];
  if (::pipe(pipefd) < 0) fail("pipe");
  const std::string port_str = std::to_string(port);
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    fail("fork");
  }
  if (pid == 0) {
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    for (int fd = 3; fd < 256; ++fd) ::close(fd);
    ::execl(bin.c_str(), bin.c_str(), "127.0.0.1", port_str.c_str(),
            static_cast<char*>(nullptr));
    const char msg[] = "exec failed\n";
    if (::write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0) {
    }
    _exit(127);
  }
  ::close(pipefd[1]);
  child.pid = pid;
  child.read_fd = pipefd[0];
  child.reader = std::thread([&child] {
    std::string pending;
    char buf[512];
    while (true) {
      const ssize_t n = ::read(child.read_fd, buf, sizeof(buf));
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) break;
      pending.append(buf, buf + n);
      std::size_t nl = 0;
      while ((nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pending.erase(0, nl + 1);
        std::lock_guard lock(child.mu);
        child.lines.push_back(std::move(line));
      }
    }
    if (!pending.empty()) {
      std::lock_guard lock(child.mu);
      child.lines.push_back(std::move(pending));
    }
    std::lock_guard lock(child.mu);
    child.eof = true;
  });
}

bool hasReady(Child& child, const std::string& prefix) {
  for (const auto& line : child.snapshot()) {
    if (line.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: test_tower <osats-tower-bin> <osats-traffic-bin>\n";
    return 2;
  }
  const std::string tower_bin = argv[1];
  const std::string traffic_bin = argv[2];

  try {
    const auto port = freePort();
    osats::ServerConfig config;
    config.port = port;
    config.enable_http = false;
    config.verbose = false;
    osats::Server server(config);
    std::thread server_thread([&server] {
      try {
        server.run();
      } catch (const std::exception& ex) {
        std::cerr << "server: " << ex.what() << "\n";
      }
    });
    struct JoinServer {
      osats::Server& server;
      std::thread& thread;
      ~JoinServer() {
        server.requestStop();
        if (thread.joinable()) thread.join();
      }
    } join{server, server_thread};

    osats::Client human;
    std::mutex mu;
    std::unordered_map<std::string, osats::Peer> seen;
    std::unordered_map<osats::EntityId, std::string> id_to_callsign;
    std::unordered_set<std::string> removed;
    std::vector<osats::TextEvent> texts;
    std::vector<std::string> errors;

    human.onPeerUpdate([&](const osats::Peer& peer) {
      std::lock_guard lock(mu);
      id_to_callsign[peer.id] = peer.callsign;
      seen[peer.callsign] = peer;
    });
    human.onPeerRemove([&](osats::EntityId id) {
      std::lock_guard lock(mu);
      const auto it = id_to_callsign.find(id);
      if (it != id_to_callsign.end()) {
        removed.insert(it->second);
        seen.erase(it->second);
      }
    });
    human.onText([&](const osats::TextEvent& ev) {
      std::lock_guard lock(mu);
      texts.push_back(ev);
    });
    human.onError([&](osats::ErrorCode code, const std::string& msg) {
      std::lock_guard lock(mu);
      errors.push_back(std::to_string(static_cast<int>(code)) + " " + msg);
    });

    human.connect("127.0.0.1", port, "N17XX", osats::Role::Pilot, "Ada Lovelace");
    human.tuneRadio(kTowerHz);
    osats::Position self;
    self.latitude_deg = kFieldLat;
    self.longitude_deg = kFieldLon;
    self.altitude_ft = 13;
    self.heading_deg = 280;
    self.squawk = 1200;
    self.xpdr_mode = 'C';
    human.sendPosition(self, 50.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Child traffic;
    startProcess(traffic, traffic_bin, port);
    if (!waitFor([&] { return hasReady(traffic, "ready "); }, 5000)) {
      fail("timed out waiting for UNICOM traffic");
    }

    auto countTraffic = [&](bool unicom_only) {
      std::lock_guard lock(mu);
      int n = 0;
      for (const auto& [cs, peer] : seen) {
        (void)cs;
        if (peer.role != osats::Role::Pilot || peer.real_name != "OSATS Traffic") continue;
        if (unicom_only && peer.radio_hz != 122800) continue;
        ++n;
      }
      return n;
    };
    if (!waitFor([&] { return countTraffic(true) >= 2 && countTraffic(false) >= 5; }, 3000)) {
      fail("injected traffic was not visible to the pilot on tower");
    }

    Child tower;
    startProcess(tower, tower_bin, port);
    if (!waitFor([&] { return hasReady(tower, "ready KSFO_TWR "); }, 5000)) {
      fail("timed out waiting for tower ready line");
    }

    auto towerStanding = [&] {
      std::lock_guard lock(mu);
      if (!errors.empty()) return false;
      const auto it = seen.find("KSFO_TWR");
      if (it == seen.end()) return false;
      const osats::Peer& peer = it->second;
      if (peer.role != osats::Role::Controller) return false;
      if (peer.real_name != "OSATS Tower") return false;
      if (peer.radio_hz != kTowerHz) return false;
      if (peer.position.groundspeed_kt > 1.0) return false;
      const double dlat = peer.position.latitude_deg - kFieldLat;
      const double dlon = peer.position.longitude_deg - kFieldLon;
      if (dlat > 0.002 || dlat < -0.002 || dlon > 0.002 || dlon < -0.002) return false;
      int controllers = 0;
      for (const auto& [cs, other] : seen) {
        (void)cs;
        if (other.role == osats::Role::Controller) ++controllers;
      }
      return controllers == 1;
    };
    if (!waitFor(towerStanding, 3000)) {
      std::lock_guard lock(mu);
      std::ostringstream oss;
      oss << "human did not see KSFO_TWR standing at the field";
      for (const auto& [cs, peer] : seen) {
        oss << " [" << cs << " role=" << static_cast<int>(peer.role) << " radio=" << peer.radio_hz
            << " lat=" << peer.position.latitude_deg << " lon=" << peer.position.longitude_deg
            << "]";
      }
      for (const auto& err : errors) oss << " error=" << err;
      fail(oss.str());
    }

    std::string clearance;
    auto heardClearance = [&] {
      std::lock_guard lock(mu);
      for (const auto& ev : texts) {
        if (ev.from == "KSFO_TWR" && ev.scope == osats::TextScope::Radio &&
            ev.text.find("N17XX") != std::string::npos &&
            ev.text.find("hold short runway 28") != std::string::npos) {
          clearance = ev.text;
          return true;
        }
      }
      return false;
    };
    if (!waitFor(heardClearance, 10000)) {
      std::lock_guard lock(mu);
      std::ostringstream oss;
      oss << "no tower clearance on 120.500";
      for (const auto& ev : texts) oss << " [" << ev.from << ": " << ev.text << "]";
      for (const auto& err : errors) oss << " error=" << err;
      fail(oss.str());
    }

    osats::Peer tower_peer;
    {
      std::lock_guard lock(mu);
      tower_peer = seen["KSFO_TWR"];
    }

    const pid_t killed = tower.pid;
    ::kill(killed, SIGKILL);
    int status = 0;
    while (::waitpid(killed, &status, 0) < 0) {
      if (errno != EINTR) fail("waitpid");
    }
    tower.pid = -1;

    auto controllersGone = [&] {
      std::lock_guard lock(mu);
      if (!removed.count("KSFO_TWR")) return false;
      for (const auto& [cs, peer] : seen) {
        (void)cs;
        if (peer.role == osats::Role::Controller) return false;
      }
      return true;
    };
    if (!waitFor(controllersGone, 3000)) {
      fail("controllers did not go to zero after the tower process exited");
    }

    const int unicom_after = countTraffic(true);
    const int traffic_after = countTraffic(false);
    if (unicom_after < 2 || traffic_after < 5) {
      fail("injected traffic left when the tower exited");
    }

    osats::Client taken;
    taken.connect("127.0.0.1", port, "KSFO_TWR", osats::Role::Controller, "Probe");
    if (taken.entityId() == osats::kInvalidEntityId) fail("KSFO_TWR was not freed");
    const auto freed_id = taken.entityId();
    taken.disconnect();

    std::cout << "controller KSFO_TWR at " << tower_peer.position.latitude_deg << ","
              << tower_peer.position.longitude_deg << " radio=" << tower_peer.radio_hz
              << " name=" << tower_peer.real_name << "\n";
    std::cout << "text " << clearance << "\n";
    std::cout << "tower_freq=" << kTowerHz << "\n";
    std::cout << "controllers_after_exit=0\n";
    std::cout << "callsign_freed=KSFO_TWR id=" << freed_id << "\n";
    std::cout << "unicom_pilots=" << unicom_after << " traffic_pilots=" << traffic_after << "\n";
    std::cout << "test_tower ok\n";

    human.disconnect();
  } catch (const std::exception& ex) {
    std::cerr << "test_tower failed: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
