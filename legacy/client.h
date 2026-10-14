#ifndef _OSATS_CLIENT_
#define _OSATS_CLIENT_

#include "config.h"
#include "message.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 

namespace OSATS {
  class Client {
  public:
    static Client &getInstance();
    ~Client();
    void connect(std::string host, int port);
    void disconnect();
    void send(Message &msg);
  private:
    Client() : connected(false) {};
    Client(Client const&) = delete;
    void operator=(Client const&) = delete;
    int socket;
    bool connected;
    std::thread receiver;
    static void receiveRoutine(int socket);
    static void receive(int socket, char *msg, size_t len);
  };
};

#endif