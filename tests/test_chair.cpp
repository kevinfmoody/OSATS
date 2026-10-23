#include "osats/client.hpp"
#include "osats/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// osats-tower holds KSFO_TWR, then exits. A human takes that callsign as a
// controller at the field on 120.500. The roster is the injected fleet only.
// Pattern aircraft are ordinary pilots, and a clearance from this session
// changes the track the same way a pilot client's text does.

namespace {

constexpr std::uint32_t kTowerHz = 120500;
constexpr std::uint32_t kUnicomHz = 122800;
constexpr double kFieldLat = 37.618806;
constexpr double kFieldLon = -122.375417;
constexpr const char* kChair = "KSFO_TWR";

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

double headingDelta(double a, double b) {
  double d = std::fmod(std::abs(a - b), 360.0);
  if (d > 180.0) d = 360.0 - d;
  return d;
}

bool nearField(const osats::Position& pos) {
  const double dlat = pos.latitude_deg - kFieldLat;
  const double dlon = pos.longitude_deg - kFieldLon;
  return dlat < 0.25 && dlat > -0.25 && dlon < 0.30 && dlon > -0.30;
}

osats::Position fieldPosition() {
  osats::Position pos;
  pos.latitude_deg = kFieldLat;
  pos.longitude_deg = kFieldLon;
  pos.altitude_ft = 13;
  pos.groundspeed_kt = 0;
  pos.heading_deg = 280;
  pos.squawk = 1200;
  pos.xpdr_mode = 'C';
  return pos;
}

struct Child {
  pid_t pid = -1;
  int read_fd = -1;
  std::thread reader;
  std::mutex mu;
  std::vector<std::string> lines;

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
    std::cerr << "Usage: test_chair <osats-traffic-bin> <osats-tower-bin>\n";
    return 2;
  }
  const std::string traffic_bin = argv[1];
  const std::string tower_bin = argv[2];

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

    osats::Client watch;
    std::mutex mu;
    std::unordered_map<std::string, osats::Peer> seen;
    std::unordered_map<osats::EntityId, std::string> id_to_callsign;
    std::unordered_set<std::string> removed;
    std::vector<std::string> errors;

    watch.onPeerUpdate([&](const osats::Peer& peer) {
      std::lock_guard lock(mu);
      id_to_callsign[peer.id] = peer.callsign;
      seen[peer.callsign] = peer;
      removed.erase(peer.callsign);
    });
    watch.onPeerRemove([&](osats::EntityId id) {
      std::lock_guard lock(mu);
      const auto it = id_to_callsign.find(id);
      if (it != id_to_callsign.end()) {
        removed.insert(it->second);
        seen.erase(it->second);
      }
    });
    watch.onError([&](osats::ErrorCode code, const std::string& msg) {
      std::lock_guard lock(mu);
      errors.push_back(std::to_string(static_cast<int>(code)) + " " + msg);
    });

    watch.connect("127.0.0.1", port, "N17XX", osats::Role::Pilot, "Ramp");
    watch.tuneRadio(kTowerHz);
    watch.sendPosition(fieldPosition(), 50.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto watchPeer = [&](const std::string& callsign) -> std::optional<osats::Peer> {
      std::lock_guard lock(mu);
      const auto it = seen.find(callsign);
      if (it == seen.end()) return std::nullopt;
      return it->second;
    };
    auto watchDetail = [&] {
      std::lock_guard lock(mu);
      std::ostringstream oss;
      for (const auto& [cs, peer] : seen) {
        oss << " [" << cs << " role=" << static_cast<int>(peer.role) << " radio=" << peer.radio_hz
            << " name=" << peer.real_name << "]";
      }
      for (const auto& err : errors) oss << " error=" << err;
      return oss.str();
    };

    Child traffic;
    startProcess(traffic, traffic_bin, port);
    if (!waitFor([&] { return hasReady(traffic, "ready "); }, 5000)) {
      fail("timed out waiting for traffic");
    }

    Child tower;
    startProcess(tower, tower_bin, port);
    if (!waitFor([&] { return hasReady(tower, "ready KSFO_TWR "); }, 5000)) {
      fail("timed out waiting for tower");
    }

    if (!waitFor(
            [&] {
              auto peer = watchPeer(kChair);
              if (!peer) return false;
              const double dlat = peer->position.latitude_deg - kFieldLat;
              const double dlon = peer->position.longitude_deg - kFieldLon;
              return peer->role == osats::Role::Controller && peer->real_name == "OSATS Tower" &&
                     peer->radio_hz == kTowerHz && dlat < 0.002 && dlat > -0.002 &&
                     dlon < 0.002 && dlon > -0.002;
            },
            3000)) {
      fail("watcher did not see OSATS Tower holding KSFO_TWR" + watchDetail());
    }
    std::cout << "tower_occupied=KSFO_TWR name=OSATS Tower\n";

    {
      osats::Client probe;
      std::atomic<int> probe_code{-1};
      probe.onError([&](osats::ErrorCode code, const std::string&) {
        probe_code.store(static_cast<int>(code));
      });
      bool rejected = false;
      try {
        probe.connect("127.0.0.1", port, kChair, osats::Role::Controller, "Blocked");
      } catch (const std::exception&) {
        rejected = true;
      }
      if (!rejected || probe_code.load() != static_cast<int>(osats::ErrorCode::CallsignTaken)) {
        probe.disconnect();
        fail("KSFO_TWR was not locked while osats-tower was up, code=" +
             std::to_string(probe_code.load()));
      }
    }
    std::cout << "callsign_taken_while_tower_up=KSFO_TWR\n";

    const pid_t killed = tower.pid;
    ::kill(killed, SIGKILL);
    int status = 0;
    while (::waitpid(killed, &status, 0) < 0) {
      if (errno != EINTR) fail("waitpid tower");
    }
    tower.pid = -1;

    if (!waitFor(
            [&] {
              std::lock_guard lock(mu);
              if (!removed.count(kChair)) return false;
              for (const auto& [cs, peer] : seen) {
                (void)cs;
                if (peer.role == osats::Role::Controller) return false;
              }
              return true;
            },
            3000)) {
      fail("KSFO_TWR was not freed when osats-tower exited" + watchDetail());
    }
    std::cout << "tower_exit controllers=0\n";

    osats::Client human;
    std::mutex hmu;
    std::unordered_map<std::string, osats::Peer> hseen;
    std::unordered_map<osats::EntityId, std::string> hid;
    std::unordered_set<std::string> hremoved;
    std::vector<osats::TextEvent> texts;
    std::vector<std::string> herrors;

    human.onPeerUpdate([&](const osats::Peer& peer) {
      std::lock_guard lock(hmu);
      hid[peer.id] = peer.callsign;
      hseen[peer.callsign] = peer;
      hremoved.erase(peer.callsign);
    });
    human.onPeerRemove([&](osats::EntityId id) {
      std::lock_guard lock(hmu);
      const auto it = hid.find(id);
      if (it != hid.end()) {
        hremoved.insert(it->second);
        hseen.erase(it->second);
      }
    });
    human.onText([&](const osats::TextEvent& ev) {
      std::lock_guard lock(hmu);
      texts.push_back(ev);
    });
    human.onError([&](osats::ErrorCode code, const std::string& msg) {
      std::lock_guard lock(hmu);
      herrors.push_back(std::to_string(static_cast<int>(code)) + " " + msg);
    });

    human.connect("127.0.0.1", port, kChair, osats::Role::Controller, "Ada Lovelace");
    if (human.entityId() == osats::kInvalidEntityId) fail("human could not hold KSFO_TWR");
    const auto chair_id = human.entityId();
    human.tuneRadio(kTowerHz);
    human.sendPosition(fieldPosition(), 50.0);

    if (!waitFor(
            [&] {
              auto peer = watchPeer(kChair);
              if (!peer) return false;
              const double dlat = peer->position.latitude_deg - kFieldLat;
              const double dlon = peer->position.longitude_deg - kFieldLon;
              return peer->role == osats::Role::Controller && peer->real_name == "Ada Lovelace" &&
                     peer->radio_hz == kTowerHz && peer->position.groundspeed_kt <= 1.0 &&
                     dlat < 0.002 && dlat > -0.002 && dlon < 0.002 && dlon > -0.002;
            },
            3000)) {
      fail("watcher did not see the human holding KSFO_TWR" + watchDetail());
    }
    std::cout << "chair KSFO_TWR id=" << chair_id << " role=controller name=Ada Lovelace radio="
              << kTowerHz << "\n";

    watch.disconnect();

    auto humanPeer = [&](const std::string& callsign) -> std::optional<osats::Peer> {
      std::lock_guard lock(hmu);
      const auto it = hseen.find(callsign);
      if (it == hseen.end()) return std::nullopt;
      return it->second;
    };
    auto humanDetail = [&] {
      std::lock_guard lock(hmu);
      std::ostringstream oss;
      for (const auto& [cs, peer] : hseen) {
        oss << " [" << cs << " role=" << static_cast<int>(peer.role) << " radio=" << peer.radio_hz
            << " name=" << peer.real_name << " gs=" << peer.position.groundspeed_kt
            << " hdg=" << peer.position.heading_deg << " squawk=" << peer.position.squawk
            << "]";
      }
      for (const auto& err : herrors) oss << " error=" << err;
      oss << " texts:";
      for (const auto& ev : texts) oss << " [" << ev.from << ": " << ev.text << "]";
      return oss.str();
    };
    auto heard = [&](const std::string& from, const std::string& fragment) {
      std::lock_guard lock(hmu);
      for (const auto& ev : texts) {
        if (ev.from == from && ev.scope == osats::TextScope::Radio &&
            ev.text.find(fragment) != std::string::npos) {
          return true;
        }
      }
      return false;
    };

    auto thinRoster = [&] {
      std::lock_guard lock(hmu);
      if (!herrors.empty()) return false;
      if (!hremoved.count("N17XX")) return false;
      if (hseen.count("N17XX") != 0 || hseen.count(kChair) != 0) return false;
      int pattern = 0;
      int unicom = 0;
      for (const auto& [cs, peer] : hseen) {
        if (peer.role != osats::Role::Pilot || peer.real_name != "OSATS Traffic") return false;
        const bool pattern_cs = cs == "SWA2104" || cs == "DAL415" || cs == "N172SP";
        const bool unicom_cs = cs == "AAL142" || cs == "UAL482";
        if (pattern_cs && peer.radio_hz == kTowerHz && nearField(peer.position)) {
          ++pattern;
          continue;
        }
        if (unicom_cs && peer.radio_hz == kUnicomHz) {
          ++unicom;
          continue;
        }
        return false;
      }
      return pattern == 3 && unicom == 2 && hseen.size() == 5;
    };
    if (!waitFor(thinRoster, 3000)) {
      fail("human roster was not the injected pilots" + humanDetail());
    }
    std::cout << "roster pilots=5 pattern=3 unicom=2 controllers=0\n";

    const auto dal_before = *humanPeer("DAL415");
    const double dal_gs_before = dal_before.position.groundspeed_kt;
    const double dal_hdg_before = dal_before.position.heading_deg;
    human.sendText(osats::TextScope::Radio, "120.500",
                   "DAL415, at the field, hold short runway 28");
    if (!waitFor(
            [&] {
              auto dal = humanPeer("DAL415");
              return dal && dal->position.groundspeed_kt <= 1.0 &&
                     heard("DAL415", "holding short");
            },
            5000)) {
      fail("DAL415 did not read back hold short to the human tower" + humanDetail());
    }
    std::cout << "human tower DAL415 hold short, gs " << dal_gs_before << " -> "
              << humanPeer("DAL415")->position.groundspeed_kt << "\n";

    if (headingDelta(dal_hdg_before, 40.0) < 20.0) {
      fail("DAL415 was already on heading 040 before the clearance");
    }
    human.sendText(osats::TextScope::Radio, "120.500", "DAL415 fly heading 040");
    if (!waitFor(
            [&] {
              auto dal = humanPeer("DAL415");
              return dal && dal->position.groundspeed_kt <= 1.0 &&
                     headingDelta(dal->position.heading_deg, 40.0) < 2.0 &&
                     heard("DAL415", "heading 40");
            },
            5000)) {
      fail("DAL415 did not copy heading 040 for the human tower" + humanDetail());
    }
    std::cout << "human tower DAL415 heading " << dal_hdg_before << " -> "
              << humanPeer("DAL415")->position.heading_deg << "\n";

    human.sendText(osats::TextScope::Radio, "120.500", "DAL415 fly runway heading");
    if (!waitFor(
            [&] {
              auto dal = humanPeer("DAL415");
              return dal && dal->position.groundspeed_kt <= 1.0 &&
                     headingDelta(dal->position.heading_deg, 280.0) < 2.0 &&
                     heard("DAL415", "runway heading");
            },
            5000)) {
      fail("DAL415 did not copy runway heading for the human tower" + humanDetail());
    }
    std::cout << "human tower DAL415 runway heading "
              << humanPeer("DAL415")->position.heading_deg << "\n";

    const auto swa_squawk = humanPeer("SWA2104")->position.squawk;
    if (swa_squawk == 4261) fail("SWA2104 squawk was already 4261");
    human.sendText(osats::TextScope::Radio, "120.500", "SWA2104 squawk 4261");
    if (!waitFor(
            [&] {
              auto swa = humanPeer("SWA2104");
              return swa && swa->role == osats::Role::Pilot && swa->position.squawk == 4261 &&
                     heard("SWA2104", "squawk 4261");
            },
            5000)) {
      fail("SWA2104 did not copy squawk 4261 for the human tower" + humanDetail());
    }
    std::cout << "human tower SWA2104 squawk " << swa_squawk << " -> 4261\n";

    const auto lineup_from = *humanPeer("DAL415");
    human.sendText(osats::TextScope::Radio, "120.500", "DAL415, runway 28, line up and wait");
    if (!waitFor(
            [&] {
              auto dal = humanPeer("DAL415");
              if (!dal || dal->role != osats::Role::Pilot) return false;
              const double dlat = dal->position.latitude_deg - lineup_from.position.latitude_deg;
              const double dlon = dal->position.longitude_deg - lineup_from.position.longitude_deg;
              return heard("DAL415", "lining up") && dal->position.groundspeed_kt <= 1.0 &&
                     std::hypot(dlat, dlon) > 0.0002 &&
                     headingDelta(dal->position.heading_deg, 280.0) < 5.0;
            },
            15000)) {
      fail("DAL415 did not line up for the human tower" + humanDetail());
    }
    const auto lined = *humanPeer("DAL415");
    std::cout << "human tower DAL415 lined up, hdg " << lined.position.heading_deg << " at "
              << lined.position.latitude_deg << "," << lined.position.longitude_deg << "\n";

    if (!thinRoster()) fail("roster changed while the human worked the pattern" + humanDetail());
    std::cout << "test_chair ok\n";

    human.disconnect();
    traffic.stop();
  } catch (const std::exception& ex) {
    std::cerr << "test_chair failed: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
