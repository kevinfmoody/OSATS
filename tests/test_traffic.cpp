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
#include <cstdlib>
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

// Drives osats-traffic as a separate process against a local server.
// A single human pilot (no controller) must see the injected sessions,
// query their flight plans, watch a later position fan out, and receive
// PeerRemove when that process is killed.

namespace {

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

struct Expected {
  std::string callsign;
  std::string city_pair;
  std::string type;
};

std::vector<Expected> parseReady(const std::string& line) {
  std::istringstream iss(line);
  std::string tag;
  iss >> tag;
  if (tag != "ready") return {};
  std::vector<Expected> out;
  std::string tok;
  while (iss >> tok) {
    const auto eq = tok.find('=');
    const auto slash = tok.find('/');
    if (eq == std::string::npos || slash == std::string::npos || slash < eq) {
      fail("bad ready token " + tok);
    }
    Expected e;
    e.callsign = tok.substr(0, eq);
    e.city_pair = tok.substr(eq + 1, slash - (eq + 1));
    e.type = tok.substr(slash + 1);
    if (e.callsign.empty() || e.city_pair.empty() || e.type.empty()) {
      fail("incomplete ready token " + tok);
    }
    out.push_back(std::move(e));
  }
  return out;
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

void startTraffic(Child& child, const std::string& bin, std::uint16_t port) {
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
    const char msg[] = "exec osats-traffic failed\n";
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: test_traffic <osats-traffic-bin>\n";
    return 2;
  }
  const std::string traffic_bin = argv[1];

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
    std::unordered_map<std::string, std::string> plan_replies;
    std::vector<std::string> texts;
    std::vector<std::string> errors;

    human.onPeerUpdate([&](const osats::Peer& peer) {
      std::lock_guard lock(mu);
      id_to_callsign[peer.id] = peer.callsign;
      seen[peer.callsign] = peer;
    });
    human.onPeerRemove([&](osats::EntityId id) {
      std::lock_guard lock(mu);
      const auto it = id_to_callsign.find(id);
      if (it != id_to_callsign.end()) removed.insert(it->second);
    });
    human.onReply([&](osats::QueryType type, const std::string& target, const std::string& body) {
      if (type != osats::QueryType::FlightPlan) return;
      std::lock_guard lock(mu);
      plan_replies[target] = body;
    });
    human.onText([&](const osats::TextEvent& ev) {
      std::lock_guard lock(mu);
      texts.push_back(ev.from + ": " + ev.text);
    });
    human.onError([&](osats::ErrorCode code, const std::string& msg) {
      std::lock_guard lock(mu);
      errors.push_back(std::to_string(static_cast<int>(code)) + " " + msg);
    });

    human.connect("127.0.0.1", port, "N17XX", osats::Role::Pilot, "Ada Lovelace");
    human.tuneRadio(122800);
    osats::Position self;
    self.latitude_deg = 37.618806;
    self.longitude_deg = -122.375417;
    self.altitude_ft = 13;
    self.heading_deg = 280;
    self.squawk = 1200;
    self.xpdr_mode = 'C';
    human.sendPosition(self, 50.0);

    Child traffic;
    startTraffic(traffic, traffic_bin, port);
    if (!waitFor(
            [&] {
              for (const auto& line : traffic.snapshot()) {
                if (line.rfind("ready ", 0) == 0) return true;
              }
              return false;
            },
            5000)) {
      fail("timed out waiting for traffic ready line");
    }

    std::vector<Expected> expected;
    for (const auto& line : traffic.snapshot()) {
      if (line.rfind("ready ", 0) == 0) {
        expected = parseReady(line);
        break;
      }
    }
    if (expected.size() < 4) fail("traffic launched fewer than 4 aircraft");

    bool has_arrival = false;
    bool has_departure = false;
    for (const auto& e : expected) {
      if (e.city_pair.size() >= 5 && e.city_pair.compare(e.city_pair.size() - 5, 5, "-KSFO") == 0) {
        has_arrival = true;
      }
      if (e.city_pair.rfind("KSFO-", 0) == 0) has_departure = true;
    }
    if (!has_arrival || !has_departure) {
      fail("fleet needs both a KSFO arrival and a KSFO departure");
    }

    auto allVisible = [&] {
      std::lock_guard lock(mu);
      if (!errors.empty()) return false;
      if (seen.size() < expected.size()) return false;
      for (const auto& e : expected) {
        const auto it = seen.find(e.callsign);
        if (it == seen.end()) return false;
        const osats::Peer& peer = it->second;
        if (peer.role != osats::Role::Pilot) return false;
        if (peer.real_name != "OSATS Traffic") return false;
        if (peer.radio_hz != 122800) return false;
        // Tracks stay inside about 20 nm of the field, under half a degree.
        const double dlat = peer.position.latitude_deg - self.latitude_deg;
        const double dlon = peer.position.longitude_deg - self.longitude_deg;
        if (dlat > 0.45 || dlat < -0.45 || dlon > 0.55 || dlon < -0.55) return false;
      }
      for (const auto& [cs, peer] : seen) {
        (void)cs;
        if (peer.role == osats::Role::Controller) return false;
      }
      return true;
    };
    if (!waitFor(allVisible, 3000)) {
      std::lock_guard lock(mu);
      std::ostringstream oss;
      oss << "human did not see the injected pilots";
      for (const auto& [cs, peer] : seen) {
        oss << " [" << cs << " role=" << static_cast<int>(peer.role) << " radio=" << peer.radio_hz
            << " name=" << peer.real_name << " lat=" << peer.position.latitude_deg
            << " lon=" << peer.position.longitude_deg << "]";
      }
      for (const auto& err : errors) oss << " error=" << err;
      fail(oss.str());
    }

    std::unordered_map<std::string, osats::Position> first_pos;
    {
      std::lock_guard lock(mu);
      for (const auto& e : expected) first_pos[e.callsign] = seen[e.callsign].position;
    }
    for (const auto& e : expected) human.query(osats::QueryType::FlightPlan, e.callsign);

    std::string moved_callsign;
    osats::Position moved_from{};
    osats::Position moved_to{};
    auto progressed = [&] {
      std::lock_guard lock(mu);
      if (!errors.empty()) return false;
      if (plan_replies.size() < expected.size()) return false;
      for (const auto& e : expected) {
        const auto it = plan_replies.find(e.callsign);
        if (it == plan_replies.end()) return false;
        if (it->second.find(e.city_pair) == std::string::npos) return false;
        if (it->second.find(e.type) == std::string::npos) return false;
      }
      bool heard_call = false;
      for (const auto& line : texts) {
        if (line.find("KSFO traffic") != std::string::npos) heard_call = true;
      }
      if (!heard_call) return false;
      for (const auto& e : expected) {
        const auto it = seen.find(e.callsign);
        if (it == seen.end()) continue;
        const auto& was = first_pos[e.callsign];
        const auto& now = it->second.position;
        const double dlat = now.latitude_deg - was.latitude_deg;
        const double dlon = now.longitude_deg - was.longitude_deg;
        if (dlat > 1e-5 || dlat < -1e-5 || dlon > 1e-5 || dlon < -1e-5) {
          moved_callsign = e.callsign;
          moved_from = was;
          moved_to = now;
          return true;
        }
      }
      return false;
    };
    if (!waitFor(progressed, 5000)) {
      std::lock_guard lock(mu);
      std::ostringstream oss;
      oss << "missing plan query, position fanout, or position call";
      oss << " plans=" << plan_replies.size() << " texts=" << texts.size();
      for (const auto& [cs, body] : plan_replies) oss << " [" << cs << " " << body << "]";
      for (const auto& err : errors) oss << " error=" << err;
      fail(oss.str());
    }

    std::vector<std::string> plan_lines;
    std::string text_line;
    {
      std::lock_guard lock(mu);
      plan_lines.reserve(expected.size());
      for (const auto& e : expected) plan_lines.push_back(e.callsign + ": " + plan_replies[e.callsign]);
      text_line = texts.front();
    }

    const pid_t killed = traffic.pid;
    ::kill(killed, SIGKILL);
    int status = 0;
    while (::waitpid(killed, &status, 0) < 0) {
      if (errno != EINTR) fail("waitpid");
    }
    traffic.pid = -1;

    auto allGone = [&] {
      std::lock_guard lock(mu);
      if (removed.size() < expected.size()) return false;
      for (const auto& e : expected) {
        if (!removed.count(e.callsign)) return false;
      }
      return true;
    };
    if (!waitFor(allGone, 3000)) {
      std::lock_guard lock(mu);
      std::ostringstream oss;
      oss << "PeerRemove missing after process kill, removed=" << removed.size();
      fail(oss.str());
    }

    const auto still = human.peers();
    for (const auto& e : expected) {
      for (const auto& [id, peer] : still) {
        (void)id;
        if (peer.callsign == e.callsign) fail(e.callsign + " still present after PeerRemove");
      }
    }

    std::cout << "human N17XX saw " << expected.size() << " pilots on 122800:";
    for (const auto& e : expected) std::cout << " " << e.callsign;
    std::cout << "\n";
    for (const auto& line : plan_lines) std::cout << "plan " << line << "\n";
    std::cout << "position fanout " << moved_callsign << " " << moved_from.latitude_deg << ","
              << moved_from.longitude_deg << " -> " << moved_to.latitude_deg << ","
              << moved_to.longitude_deg << "\n";
    std::cout << "radio " << text_line << "\n";
    std::cout << "controllers=0\n";
    std::cout << "PeerRemove " << expected.size() << " after killing traffic pid " << killed
              << "\n";
    std::cout << "test_traffic ok\n";

    human.disconnect();
  } catch (const std::exception& ex) {
    std::cerr << "test_traffic failed: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
