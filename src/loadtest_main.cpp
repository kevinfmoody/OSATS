#include "osats/client.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "Usage: osats-loadtest <host> <port> <clients> [seconds]\n";
    return 2;
  }
  const std::string host = argv[1];
  const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
  const int n = std::atoi(argv[3]);
  const int seconds = argc >= 5 ? std::atoi(argv[4]) : 15;

  std::atomic<int> connected{0};
  std::atomic<int> peer_events{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(n));

  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&, i] {
      osats::Client client;
      client.onPeerUpdate([&](const osats::Peer&) { peer_events.fetch_add(1); });
      const std::string cs = "LT" + std::to_string(i);
      try {
        client.connect(host, port, cs);
        connected.fetch_add(1);

        std::mt19937 rng(static_cast<unsigned>(i * 9973 + 42));
        std::uniform_real_distribution<double> dlat(-0.5, 0.5);
        std::uniform_real_distribution<double> dlon(-0.5, 0.5);

        osats::Position pos;
        // Cluster around SFO so neighbors see each other.
        pos.latitude_deg = 37.62 + dlat(rng) * 0.1;
        pos.longitude_deg = -122.38 + dlon(rng) * 0.1;
        pos.altitude_ft = 5000;
        pos.groundspeed_kt = 250;
        pos.heading_deg = 90;

        const auto end =
            std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < end && client.connected()) {
          client.sendPosition(pos, 40.0);
          pos.longitude_deg += 0.0005;
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        client.disconnect();
      } catch (const std::exception& ex) {
        std::cerr << "client " << i << " failed: " << ex.what() << "\n";
      }
    });
  }

  for (auto& t : threads) t.join();
  std::cout << "connected_peakish=" << connected.load()
            << " peer_events=" << peer_events.load() << "\n";
  return 0;
}
