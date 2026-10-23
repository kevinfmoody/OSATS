#include "osats/client.hpp"
#include "osats/net/socket.hpp"
#include "osats/protocol.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Measurement harness. It does not change fanout, distance, or the protocol.
// Each client stamps a sample counter into the existing squawk field so a
// receiver can match a PeerUpdate back to the send without a new wire field.
// Age is receive time minus that send time. Fanout bytes are the PeerUpdate
// frames actually delivered, reconstructed with encodePeerUpdate (the flight
// plan bit does not change the frame size). CPU is the osats-server process.

namespace {

enum class Layout { Terminal, Spread };

const char* layoutName(Layout layout) {
  return layout == Layout::Terminal ? "terminal" : "spread";
}

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string fmtFixed(double value, int precision) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(precision);
  out << value;
  return out.str();
}

osats::Position makePosition(Layout layout, int index, int n) {
  osats::Position pos;
  pos.altitude_ft = 5000;
  pos.groundspeed_kt = 180;
  pos.heading_deg = 90;
  pos.xpdr_mode = 'C';
  if (layout == Layout::Spread) {
    // 20 nm north-south. Adjacent pairs are mid (1 Hz), two hops are far
    // (5 s), and three hops fall outside the 50 nm visibility sent below.
    // North-south distance does not use the longitude scale.
    pos.latitude_deg = 36.0 + static_cast<double>(index) * (20.0 / 60.0);
    pos.longitude_deg = -122.0;
    return pos;
  }

  // Pack the whole set into a 2 nm box at KSFO. The diagonal stays inside
  // the 10 nm near tier, including the current east-west scale at ~37.6N.
  const int count = std::max(n, 1);
  const int cols = std::max(
      1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))));
  const double span_deg = 2.0 / 60.0;
  const double step = cols <= 1 ? 0.0 : span_deg / static_cast<double>(cols - 1);
  const int row = index / cols;
  const int col = index % cols;
  pos.latitude_deg = 37.619 + static_cast<double>(row) * step;
  pos.longitude_deg = -122.375 + static_cast<double>(col) * step;
  return pos;
}

std::string makeCallsign(Layout layout, int index) {
  return std::string(1, layout == Layout::Terminal ? 'T' : 'S') +
         std::to_string(index);
}

std::optional<int> callsignIndex(const std::string& callsign) {
  if (callsign.size() < 2) return std::nullopt;
  if (callsign[0] != 'T' && callsign[0] != 'S') return std::nullopt;
  int value = 0;
  for (std::size_t i = 1; i < callsign.size(); ++i) {
    if (callsign[i] < '0' || callsign[i] > '9') return std::nullopt;
    value = value * 10 + (callsign[i] - '0');
  }
  return value;
}

std::size_t peerUpdateBytes(const osats::Peer& peer) {
  osats::AircraftState state;
  state.id = peer.id;
  state.role = peer.role;
  state.callsign = peer.callsign;
  state.real_name = peer.real_name;
  state.position = peer.position;
  state.radio_hz = peer.radio_hz;
  state.flight_plan = std::nullopt;
  return osats::encodePeerUpdate(state).size();
}

double percentile(std::vector<int64_t> values, double p) {
  std::sort(values.begin(), values.end());
  if (values.size() == 1) return static_cast<double>(values[0]);
  const double rank = p * static_cast<double>(values.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(rank));
  const auto hi = static_cast<std::size_t>(std::ceil(rank));
  const double weight = rank - static_cast<double>(lo);
  return static_cast<double>(values[lo]) * (1.0 - weight) +
         static_cast<double>(values[hi]) * weight;
}

struct CpuSample {
  unsigned long long exec_ns = 0;
  int64_t wall_ns = 0;
};

std::optional<CpuSample> readCpuFromStat(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(in, line)) return std::nullopt;
  const auto paren = line.rfind(')');
  if (paren == std::string::npos) return std::nullopt;
  std::istringstream rest(line.substr(paren + 1));
  std::string skip;
  // state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt
  for (int i = 0; i < 11; ++i) rest >> skip;
  unsigned long utime = 0;
  unsigned long stime = 0;
  rest >> utime >> stime;
  if (!rest) return std::nullopt;
  const long ticks = ::sysconf(_SC_CLK_TCK);
  if (ticks <= 0) return std::nullopt;
  CpuSample sample;
  sample.exec_ns = (static_cast<unsigned long long>(utime) + stime) *
                   1'000'000'000ull / static_cast<unsigned long long>(ticks);
  sample.wall_ns = nowNs();
  return sample;
}

// schedstat is nanoseconds. stat ticks (usually 10 ms) round a short soak to 0.
std::optional<CpuSample> readCpu(pid_t pid) {
  const std::string path = "/proc/" + std::to_string(pid) + "/schedstat";
  std::ifstream sched(path);
  unsigned long long exec_ns = 0;
  if (sched >> exec_ns) {
    CpuSample sample;
    sample.exec_ns = exec_ns;
    sample.wall_ns = nowNs();
    return sample;
  }
  return readCpuFromStat(pid);
}

std::optional<double> cpuPercent(const CpuSample& start, const CpuSample& end) {
  const double wall_s =
      static_cast<double>(end.wall_ns - start.wall_ns) / 1'000'000'000.0;
  if (wall_s <= 0.0) return std::nullopt;
  if (end.exec_ns < start.exec_ns) return std::nullopt;
  const double cpu_s =
      static_cast<double>(end.exec_ns - start.exec_ns) / 1'000'000'000.0;
  return 100.0 * cpu_s / wall_s;
}

std::set<unsigned long> listenInodes(std::uint16_t port) {
  std::set<unsigned long> inodes;
  std::ifstream in("/proc/net/tcp");
  std::string line;
  std::getline(in, line);
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    std::string sl;
    std::string local;
    std::string remote;
    std::string state;
    std::string tx;
    std::string tr;
    std::string retr;
    std::string uid;
    std::string timeout;
    std::string inode;
    iss >> sl >> local >> remote >> state >> tx >> tr >> retr >> uid >> timeout >>
        inode;
    if (!iss || state != "0A") continue;
    const auto colon = local.find(':');
    if (colon == std::string::npos) continue;
    const auto local_port =
        static_cast<unsigned long>(std::strtoul(local.c_str() + colon + 1, nullptr, 16));
    if (local_port != port) continue;
    inodes.insert(std::strtoul(inode.c_str(), nullptr, 10));
  }
  return inodes;
}

pid_t findListenPid(std::uint16_t port) {
  const auto inodes = listenInodes(port);
  if (inodes.empty()) return -1;
  DIR* proc = ::opendir("/proc");
  if (!proc) return -1;
  pid_t found = -1;
  while (dirent* ent = ::readdir(proc)) {
    if (ent->d_name[0] < '1' || ent->d_name[0] > '9') continue;
    const std::string fd_dir = std::string("/proc/") + ent->d_name + "/fd";
    DIR* fds = ::opendir(fd_dir.c_str());
    if (!fds) continue;
    while (dirent* fd = ::readdir(fds)) {
      if (fd->d_name[0] == '.') continue;
      char link[64];
      const std::string path = fd_dir + "/" + fd->d_name;
      const ssize_t n = ::readlink(path.c_str(), link, sizeof(link) - 1);
      if (n <= 0) continue;
      link[n] = '\0';
      unsigned long inode = 0;
      if (std::sscanf(link, "socket:[%lu]", &inode) == 1 && inodes.count(inode) != 0) {
        found = static_cast<pid_t>(std::atoi(ent->d_name));
        break;
      }
    }
    ::closedir(fds);
    if (found > 0) break;
  }
  ::closedir(proc);
  return found;
}

bool tcpPortOpen(const std::string& host, std::uint16_t port) {
  try {
    auto sock = osats::net::Socket::connectTcp(host, port);
    return sock.valid();
  } catch (const std::exception&) {
    return false;
  }
}

pid_t g_server_pid = -1;

void onSignal(int) {
  if (g_server_pid > 0) {
    ::kill(g_server_pid, SIGTERM);
  }
  std::_Exit(130);
}

class ServerProcess {
 public:
  ServerProcess() = default;
  ~ServerProcess() { stop(); }

  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  bool spawn(const std::string& bin, std::uint16_t port) {
    if (::access(bin.c_str(), X_OK) != 0) {
      std::cerr << "osats-loadtest: cannot execute " << bin << "\n";
      return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
      std::cerr << "osats-loadtest: fork failed\n";
      return false;
    }
    if (pid == 0) {
      const int devnull = ::open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDOUT_FILENO);
        ::close(devnull);
      }
      const std::string port_text = std::to_string(port);
      ::execl(bin.c_str(), "osats-server", "--port", port_text.c_str(), "--no-http",
              static_cast<char*>(nullptr));
      std::perror("osats-loadtest: exec osats-server");
      std::_Exit(127);
    }
    pid_ = pid;
    g_server_pid = pid;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const pid_t done = ::waitpid(pid_, &status, WNOHANG);
      if (done == pid_) {
        pid_ = -1;
        g_server_pid = -1;
        std::cerr << "osats-loadtest: server exited during startup\n";
        return false;
      }
      if (tcpPortOpen("127.0.0.1", port)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cerr << "osats-loadtest: server did not accept connections\n";
    stop();
    return false;
  }

  void stop() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGTERM);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const pid_t done = ::waitpid(pid_, &status, WNOHANG);
      if (done == pid_ || (done < 0 && errno == ECHILD)) {
        pid_ = -1;
        g_server_pid = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, nullptr, 0);
    pid_ = -1;
    g_server_pid = -1;
  }

  pid_t pid() const { return pid_; }

 private:
  pid_t pid_ = -1;
};

struct Shared {
  std::mutex send_mu;
  std::vector<std::vector<int64_t>> sent_ns;
  std::mutex age_mu;
  std::vector<int64_t> ages_ns;
  std::atomic<bool> go{false};
  std::atomic<bool> record{false};
  std::atomic<int64_t> window_start_ns{0};
  std::atomic<int64_t> send_until_ns{0};
  std::atomic<int> connected{0};
  std::atomic<std::uint64_t> peer_events{0};
  std::atomic<std::uint64_t> fanout_bytes{0};
};

struct RunResult {
  int connected = 0;
  std::uint64_t peer_events = 0;
  std::uint64_t fanout_bytes = 0;
  std::vector<int64_t> ages_ns;
  std::optional<double> cpu_pct;
};

void noteSend(Shared& shared, int index, std::uint16_t seq, int64_t stamped) {
  std::lock_guard lock(shared.send_mu);
  auto& row = shared.sent_ns[static_cast<std::size_t>(index)];
  if (row.size() <= seq) row.resize(static_cast<std::size_t>(seq) + 1, 0);
  row[seq] = stamped;
}

void onPeer(Shared& shared, int n, const osats::Peer& peer) {
  if (!shared.record.load()) return;
  const auto now = nowNs();
  const auto bytes = peerUpdateBytes(peer);
  shared.peer_events.fetch_add(1, std::memory_order_relaxed);
  shared.fanout_bytes.fetch_add(bytes, std::memory_order_relaxed);

  const auto sender = callsignIndex(peer.callsign);
  if (!sender || *sender < 0 || *sender >= n) return;
  int64_t sent = 0;
  {
    std::lock_guard lock(shared.send_mu);
    const auto& row = shared.sent_ns[static_cast<std::size_t>(*sender)];
    if (peer.position.squawk < row.size()) sent = row[peer.position.squawk];
  }
  const auto start = shared.window_start_ns.load();
  if (sent == 0 || sent < start) return;
  const auto age = now - sent;
  if (age < 0) return;
  std::lock_guard lock(shared.age_mu);
  shared.ages_ns.push_back(age);
}

RunResult runLayout(Layout layout, const std::string& host, std::uint16_t port,
                    int n, int seconds, pid_t server_pid) {
  Shared shared;
  shared.sent_ns.resize(static_cast<std::size_t>(n));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(n));

  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&, i] {
      osats::Client client;
      client.onPeerUpdate(
          [&](const osats::Peer& peer) { onPeer(shared, n, peer); });
      const std::string callsign = makeCallsign(layout, i);
      try {
        client.connect(host, port, callsign);
        shared.connected.fetch_add(1);
        osats::Position pos = makePosition(layout, i, n);
        std::uint16_t seq = 0;
        while (!shared.go.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (nowNs() < shared.send_until_ns.load() && client.connected()) {
          if (seq == std::numeric_limits<std::uint16_t>::max()) break;
          ++seq;
          pos.squawk = seq;
          noteSend(shared, i, seq, nowNs());
          try {
            client.sendPosition(pos, 50.0);
          } catch (const std::exception&) {
            break;
          }
          const auto until = shared.send_until_ns.load();
          if (nowNs() >= until) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        while (shared.record.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        client.disconnect();
      } catch (const std::exception& ex) {
        std::cerr << "client " << callsign << " failed: " << ex.what() << "\n";
      }
    });
  }

  const auto connect_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(std::max(5, n));
  while (shared.connected.load() < n &&
         std::chrono::steady_clock::now() < connect_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const auto send_for = std::chrono::seconds(seconds);
  const int64_t start_ns = nowNs();
  shared.window_start_ns.store(start_ns);
  shared.send_until_ns.store(start_ns +
                              std::chrono::duration_cast<std::chrono::nanoseconds>(send_for)
                                  .count());

  std::optional<CpuSample> cpu_start;
  if (server_pid > 0) cpu_start = readCpu(server_pid);
  shared.record.store(true);
  shared.go.store(true);
  std::this_thread::sleep_for(send_for);
  // One extra server tick so the last fanout is inside the CPU sample.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::optional<CpuSample> cpu_end;
  if (server_pid > 0) cpu_end = readCpu(server_pid);

  // Keep receivers up long enough for the last fanout to land.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  shared.record.store(false);

  for (auto& thread : threads) thread.join();

  RunResult result;
  result.connected = shared.connected.load();
  result.peer_events = shared.peer_events.load();
  result.fanout_bytes = shared.fanout_bytes.load();
  result.ages_ns.swap(shared.ages_ns);
  if (cpu_start && cpu_end) result.cpu_pct = cpuPercent(*cpu_start, *cpu_end);
  return result;
}

void printResult(Layout layout, int clients, int seconds, const RunResult& result) {
  const bool have_age = !result.ages_ns.empty();
  const std::string p50 =
      have_age ? fmtFixed(percentile(result.ages_ns, 0.50) / 1'000'000.0, 2) : "na";
  const std::string p99 =
      have_age ? fmtFixed(percentile(result.ages_ns, 0.99) / 1'000'000.0, 2) : "na";
  const std::string cpu =
      result.cpu_pct ? fmtFixed(*result.cpu_pct, 2) : std::string("na");
  const auto bps = seconds > 0 ? result.fanout_bytes / static_cast<std::uint64_t>(seconds)
                               : 0;
  std::cout << "layout=" << layoutName(layout) << " clients=" << clients
            << " seconds=" << seconds << " connected=" << result.connected
            << " peer_events=" << result.peer_events
            << " age_samples=" << result.ages_ns.size() << " age_p50_ms=" << p50
            << " age_p99_ms=" << p99 << " fanout_bytes=" << result.fanout_bytes
            << " fanout_Bps=" << bps << " server_cpu_pct=" << cpu << "\n";
}

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 4400;
  int clients = 16;
  int seconds = 5;
  std::string server_bin;
  bool attach = false;
  bool host_set = false;
  bool port_set = false;
  std::vector<Layout> layouts{Layout::Terminal, Layout::Spread};
};

void printUsage(std::ostream& out) {
  out << "Usage:\n"
      << "  osats-loadtest --server BIN [--port N] [--clients N] [--seconds N]\n"
      << "  osats-loadtest <host> <port> <clients> [seconds]\n"
      << "\n"
      << "One run measures a terminal cluster and a spread-out set.\n"
      << "It logs peer-update age p50/p99 (ms), delivered fanout bytes, and\n"
      << "osats-server CPU (100 = one core) for each layout.\n"
      << "Defaults: clients=16 seconds=5 port=4400. Both layouts send every\n"
      << "200 ms at 50 nm visibility. --layout terminal|spread|both selects one.\n";
}

bool parseLayout(const std::string& text, std::vector<Layout>& layouts) {
  layouts.clear();
  if (text == "both") {
    layouts.push_back(Layout::Terminal);
    layouts.push_back(Layout::Spread);
    return true;
  }
  if (text == "terminal" || text == "cluster") {
    layouts.push_back(Layout::Terminal);
    return true;
  }
  if (text == "spread") {
    layouts.push_back(Layout::Spread);
    return true;
  }
  return false;
}

bool parseIntArg(const char* text, int& out) {
  if (!text || !*text) return false;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0') return false;
  if (value <= 0 || value > std::numeric_limits<int>::max()) return false;
  out = static_cast<int>(value);
  return true;
}

bool parseArgs(int argc, char** argv, Options& opt) {
  if (argc >= 2 && std::string(argv[1]) == "--help") return false;
  if (argc >= 4 && argv[1][0] != '-') {
    opt.attach = true;
    opt.host = argv[1];
    opt.host_set = true;
    int port = 0;
    if (!parseIntArg(argv[2], port) || port > 65535) return false;
    opt.port = static_cast<std::uint16_t>(port);
    opt.port_set = true;
    if (!parseIntArg(argv[3], opt.clients)) return false;
    if (argc >= 5 && !parseIntArg(argv[4], opt.seconds)) return false;
    if (argc > 5) return false;
    return true;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--host") {
      const char* value = need("--host");
      if (!value) return false;
      opt.host = value;
      opt.host_set = true;
    } else if (arg == "--port") {
      const char* value = need("--port");
      int port = 0;
      if (!value || !parseIntArg(value, port) || port > 65535) return false;
      opt.port = static_cast<std::uint16_t>(port);
      opt.port_set = true;
    } else if (arg == "--clients") {
      const char* value = need("--clients");
      if (!value || !parseIntArg(value, opt.clients)) return false;
    } else if (arg == "--seconds") {
      const char* value = need("--seconds");
      if (!value || !parseIntArg(value, opt.seconds)) return false;
    } else if (arg == "--server") {
      const char* value = need("--server");
      if (!value) return false;
      opt.server_bin = value;
    } else if (arg == "--layout") {
      const char* value = need("--layout");
      if (!value || !parseLayout(value, opt.layouts)) return false;
    } else if (arg == "--help" || arg == "-h") {
      return false;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return true;
}

std::string siblingServer(const char* argv0) {
  const std::string path = argv0 ? argv0 : "";
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return "osats-server";
  return path.substr(0, slash + 1) + "osats-server";
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, opt)) {
    printUsage(std::cerr);
    return 2;
  }

  ServerProcess spawned;
  pid_t server_pid = -1;
  if (!opt.server_bin.empty()) {
    if (!spawned.spawn(opt.server_bin, opt.port)) return 1;
    server_pid = spawned.pid();
    opt.host = "127.0.0.1";
    std::cerr << "osats-loadtest: spawned server pid=" << server_pid
              << " port=" << opt.port << "\n";
  } else if (!opt.attach && !opt.host_set && !opt.port_set) {
    const std::string bin = siblingServer(argv[0]);
    if (::access(bin.c_str(), X_OK) != 0) {
      std::cerr << "osats-loadtest: pass --server BIN or host port clients\n";
      printUsage(std::cerr);
      return 2;
    }
    if (!spawned.spawn(bin, opt.port)) return 1;
    server_pid = spawned.pid();
    opt.host = "127.0.0.1";
    std::cerr << "osats-loadtest: spawned server pid=" << server_pid
              << " port=" << opt.port << "\n";
  } else {
    server_pid = findListenPid(opt.port);
    if (server_pid > 0) {
      std::cerr << "osats-loadtest: server pid=" << server_pid << "\n";
    } else {
      std::cerr << "osats-loadtest: no listener on port " << opt.port
                << "; server_cpu_pct=na\n";
    }
  }

  ::signal(SIGINT, onSignal);
  ::signal(SIGTERM, onSignal);

  int failed = 0;
  for (std::size_t i = 0; i < opt.layouts.size(); ++i) {
    if (i > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    const RunResult result =
        runLayout(opt.layouts[i], opt.host, opt.port, opt.clients, opt.seconds,
                  server_pid);
    printResult(opt.layouts[i], opt.clients, opt.seconds, result);
    if (result.connected <= 0) failed = 1;
  }
  return failed;
}
