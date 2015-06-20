# OSATS
**An Open Source Air Traffic Server in C++**

OSATS is a platform designed to handle thousands of aircraft and air traffic controllers, facilitating realistic message transmission between them, and allowing for third-party developers to build networks, pilot clients, radar clients, and more.

# Setting Up a Server
```c++
#include "server.h"

int main(int argc, char *argv[]) {
  // Start a server at the port of the provided argument
  OSATS::Server s(atoi(argv[1]));
  return 0;
}
```

# Setting Up a Pilot Client
```c++
#include "osats.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <string>

int main(int argc, char *argv[]) {
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
```
