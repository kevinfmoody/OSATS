#include "osats/client.hpp"
#include "osats/server.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr " (" << __FILE__ << ":" << __LINE__\
                << ")\n";                                                    \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

struct RunningServer {
  std::unique_ptr<osats::Server> server;
  std::thread thread;
  std::uint16_t port = 0;

  RunningServer() {
    for (int i = 0; i < 20; ++i) {
      port = static_cast<std::uint16_t>(26000 + (::getpid() + i) % 20000);
      osats::ServerConfig cfg;
      cfg.port = port;
      cfg.enable_http = false;
      try {
        server = std::make_unique<osats::Server>(cfg);
        break;
      } catch (const std::exception&) {
        server.reset();
      }
    }
    if (!server) throw std::runtime_error("could not bind a test port");
    thread = std::thread([this] {
      try {
        server->run();
      } catch (const std::exception& ex) {
        std::cerr << "server: " << ex.what() << "\n";
      }
    });
  }

  ~RunningServer() {
    if (server) server->requestStop();
    if (thread.joinable()) thread.join();
  }
};

struct Sink {
  std::mutex mu;
  osats::EntityId other = osats::kInvalidEntityId;
  std::vector<osats::EntityId> removed;
  double sample_lat = 0;
  std::uint64_t server_time_ms = 0;

  void onUpdate(const osats::Peer& peer, const std::string& callsign) {
    if (peer.callsign != callsign) return;
    std::lock_guard lock(mu);
    other = peer.id;
    sample_lat = peer.position.latitude_deg;
    server_time_ms = peer.server_time_ms;
  }

  void onRemove(osats::EntityId id) {
    std::lock_guard lock(mu);
    removed.push_back(id);
  }

  int removesOfOther() {
    std::lock_guard lock(mu);
    if (other == osats::kInvalidEntityId) return 0;
    int n = 0;
    for (osats::EntityId id : removed) {
      if (id == other) ++n;
    }
    return n;
  }

  double sampleLat() {
    std::lock_guard lock(mu);
    return sample_lat;
  }

  std::uint64_t serverTime() {
    std::lock_guard lock(mu);
    return server_time_ms;
  }
};

bool waitFor(const std::function<bool()>& pred, int ms = 2000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

std::optional<osats::Peer> findCallsign(osats::Client& client, const std::string& cs) {
  for (const auto& [id, peer] : client.peers()) {
    (void)id;
    if (peer.callsign == cs) return peer;
  }
  return std::nullopt;
}

void connectClient(osats::Client& client, std::uint16_t port, const std::string& cs) {
  client.connect("127.0.0.1", port, cs, osats::Role::Pilot, cs);
}

}  // namespace

int main() {
  using namespace osats;
  try {
    RunningServer running;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Client stationary;
    Client mover;
    Sink stationary_sink;
    Sink mover_sink;
    stationary.onPeerUpdate([&](const Peer& peer) {
      stationary_sink.onUpdate(peer, "MOVER");
    });
    stationary.onPeerRemove([&](EntityId id) { stationary_sink.onRemove(id); });
    mover.onPeerUpdate(
        [&](const Peer& peer) { mover_sink.onUpdate(peer, "STATN"); });
    mover.onPeerRemove([&](EntityId id) { mover_sink.onRemove(id); });

    connectClient(stationary, running.port, "STATN");
    Position parked{37.62, -122.38, 4000, 0, 90};
    stationary.sendPosition(parked, 50);
    FlightPlan fp;
    fp.flight_rules = 'I';
    fp.aircraft_type = "B738";
    fp.departure = "KSFO";
    fp.arrival = "KLAX";
    fp.route = "DCT";
    stationary.sendFlightPlan(fp);

    connectClient(mover, running.port, "MOVER");
    const double far_nm = geo::kMaxVisibilityNm + 50.0;
    Position far_pos = parked;
    far_pos.latitude_deg += far_nm / 60.0;
    mover.sendPosition(far_pos, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!findCallsign(mover, "STATN"));
    CHECK(!findCallsign(stationary, "MOVER"));

    // Move onto the parked aircraft. It does not transmit again.
    // North velocity lets the parked client coast between samples.
    Position close = parked;
    close.latitude_deg += 0.01;
    close.vn_fps = 2000.f;
    mover.sendPosition(close, 50);
    CHECK(waitFor([&] {
      const auto peer = findCallsign(mover, "STATN");
      return peer && peer->flight_plan && peer->flight_plan->departure == "KSFO" &&
             peer->flight_plan->arrival == "KLAX" && findCallsign(stationary, "MOVER");
    }));
    CHECK(stationary_sink.removesOfOther() == 0);
    CHECK(stationary_sink.serverTime() != 0);
    const double sample_lat = stationary_sink.sampleLat();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto coasting = findCallsign(stationary, "MOVER");
    CHECK(coasting.has_value());
    const double deg_per_s =
        (2000.0 / (geo::kKtToFps * 3600.0)) / geo::kNmPerDegreeLat;
    const double dlat = coasting->position.latitude_deg - sample_lat;
    CHECK(dlat > deg_per_s * 0.2);
    CHECK(dlat < deg_per_s * 1.5);

    mover.sendPosition(far_pos, 50);
    CHECK(waitFor([&] {
      return !findCallsign(mover, "STATN") && !findCallsign(stationary, "MOVER") &&
             stationary_sink.removesOfOther() == 1 && mover_sink.removesOfOther() == 1;
    }));

    const int removes_before_disconnect = stationary_sink.removesOfOther();
    mover.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(stationary_sink.removesOfOther() == removes_before_disconnect);
    CHECK(!findCallsign(stationary, "MOVER"));

    stationary.disconnect();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << "\n";
    return 1;
  }

  std::cout << "test_server ok\n";
  return 0;
}
