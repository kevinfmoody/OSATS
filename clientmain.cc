#include "osats.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <cmath>

#define NUMSOCKETS 2

int main(int argc, char *argv[]) {
  for (int i = 1; i < NUMSOCKETS; i *= 2) {
    fork();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds((int)(round(getpid() * (5000.0 / NUMSOCKETS))) % 5000));
  // Obtain a reference to the traffic client
  OSATS::Client &client = OSATS::Client::getInstance();
  try {
    // Generate the "callsign"
    char *buf = (char *)malloc(8);
    sprintf(buf, "N%06i", getpid());
    std::string callsign(buf);
    // Connect to the traffic server
    client.connect(argv[1], atoi(argv[2]));
    // Build an aircraft object
    std::string identifier(callsign);
    OSATS::Aircraft aircraft(identifier);
    aircraft.setPosition(OSATS::Position(0, 0));
    // Build a message based on the aircraft
    OSATS::Message message(aircraft);
    // Keep sending messages to the server
    while (true) {
      // Send the aircraft message to the server
      client.send(message);
      // Wait five seconds before sending another message
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  } catch (std::string err) {
    // Print out errors if they occur
    std::cerr << err << std::endl;
  }
  return 0;
}