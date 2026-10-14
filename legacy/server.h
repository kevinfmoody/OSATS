#ifndef _OSATS_SERVER_
#define _OSATS_SERVER_

#include "config.h"
#include "store.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/event.h>

namespace OSATS {
  class Aircraft;
  class Message;

  class Server {
  public:
    Server(int port);
  private:
    void handleMessage(int socket);
    void broadcastAircraftPosition(int socket, Message &msg);
    void send(int socket, Message &msg);
    void receive(int socket, char *msg, size_t len);
    Store aircraftStore;
  };
};

#endif