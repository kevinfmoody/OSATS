#include "osats/client.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: osats-client <host> <port> [callsign]\n";
    return 2;
  }
  const std::string host = argv[1];
  const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
  std::string callsign = argc >= 4 ? argv[3] : ("N" + std::to_string(::getpid()));

  osats::Client client;
  client.onPeerUpdate([](const osats::Peer& peer) {
    std::cout << "peer " << peer.callsign << " id=" << peer.id
              << " lat=" << peer.position.latitude_deg
              << " lon=" << peer.position.longitude_deg << "\n";
  });
  client.onPeerRemove([](osats::EntityId id) {
    std::cout << "peer removed id=" << id << "\n";
  });
  client.onError([](osats::ErrorCode code, const std::string& msg) {
    std::cerr << "error " << static_cast<int>(code) << ": " << msg << "\n";
  });

  std::cout << std::unitbuf;
  try {
    client.connect(host, port, callsign);
    std::cout << "connected as " << callsign << " id=" << client.entityId() << "\n";

    osats::Position pos;
    pos.latitude_deg = 37.6213;
    pos.longitude_deg = -122.3790;
    pos.altitude_ft = 0;
    pos.groundspeed_kt = 0;
    pos.heading_deg = 90;

    while (client.connected()) {
      client.sendPosition(pos);
      client.sendHeartbeat();
      pos.longitude_deg += 0.001;  // crawl east
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  } catch (const std::exception& ex) {
    std::cerr << "fatal: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
