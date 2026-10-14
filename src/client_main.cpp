#include "osats/client.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

// Minimal non-interactive demo client (see osats-cli for the full UI).
int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: osats-client <host> <port> [callsign]\n";
    return 2;
  }
  const std::string host = argv[1];
  const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
  std::string callsign = argc >= 4 ? argv[3] : ("N" + std::to_string(::getpid()));

  osats::Client client;
  std::cout << std::unitbuf;
  client.onAnnounce([](const std::string& n, const std::string& v) {
    std::cout << "announce " << n << " " << v << "\n";
  });
  client.onPeerUpdate([](const osats::Peer& peer) {
    std::cout << "peer " << peer.callsign << " lat=" << peer.position.latitude_deg
              << " lon=" << peer.position.longitude_deg << "\n";
  });
  client.onText([](const osats::TextEvent& ev) {
    std::cout << "text [" << ev.from << "] " << ev.text << "\n";
  });
  client.onError([](osats::ErrorCode code, const std::string& msg) {
    std::cerr << "error " << static_cast<int>(code) << ": " << msg << "\n";
  });

  try {
    client.connect(host, port, callsign, osats::Role::Pilot, "Demo Pilot");
    std::cout << "connected as " << callsign << " id=" << client.entityId() << "\n";
    client.tuneRadio(122800);
    client.requestMetar("KSFO");

    osats::Position pos;
    pos.latitude_deg = 37.6213;
    pos.longitude_deg = -122.3790;
    pos.squawk = 1200;

    osats::FlightPlan fp;
    fp.departure = "KSFO";
    fp.arrival = "KLAX";
    fp.aircraft_type = "B738";
    fp.cruise_alt_ft = 35000;
    fp.route = "SSTIK3 FREBY RZS SADDE7";
    client.sendFlightPlan(fp);

    while (client.connected()) {
      client.sendPosition(pos);
      client.sendHeartbeat();
      pos.longitude_deg += 0.001;
      pos.groundspeed_kt = 180;
      pos.heading_deg = 90;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  } catch (const std::exception& ex) {
    std::cerr << "fatal: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
