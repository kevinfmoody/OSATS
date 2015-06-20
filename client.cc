#include "client.h"
#include "aircraft.h"

#include <iostream>
#include <string>

namespace OSATS {
  Client &Client::getInstance() {
    static Client client;
    return client;
  }

  void Client::connect(std::string host, int port) {
    if (this->connected) {
      return;
    }
    struct sockaddr_in serverAddr;
    struct hostent *server;
    socklen_t addrLen = sizeof(struct sockaddr_in);
    this->socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (this->socket < 0) {
      throw std::string("Cannot open socket.");
    }
    server = gethostbyname(host.c_str());
    if (server == NULL) {
      throw std::string("Host does not exist.");
    }
    bzero((char *)&serverAddr, addrLen);
    serverAddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&serverAddr.sin_addr.s_addr,
          server->h_length);
    serverAddr.sin_port = htons(port);
    if (::connect(this->socket, (struct sockaddr *)&serverAddr, addrLen) < 0) {
      throw std::string("Cannot connect.");
    }
    std::cout << getpid() << ": Connected." << std::endl;
    this->connected = true;
    this->receiver = std::thread(Client::receiveRoutine, this->socket);
  }

  Client::~Client() {
    this->receiver.join();
  }

  void Client::disconnect() {
    if (!this->connected) {
      return;
    }
    if (close(this->socket) == -1) {
      throw std::string("Cannot disconnect.");
    }
    std::cout << getpid() << ": Disconnected." << std::endl;
    this->connected = false;
  }

  void Client::send(Message &msg) {
    if (!this->connected) {
      throw std::string("Cannot send while disconnected.");
    }
    size_t len = msg.getLen();
    int lenRemaining = len;
    while (lenRemaining > 0) {
      int lenSent = write(this->socket,
                          msg.getMsg() + (len - lenRemaining),
                          lenRemaining);
      if (lenSent < 0) {
        throw std::string("Cannot send message.");
      }
      lenRemaining -= lenSent;
    }
    std::cout << getpid() << ": Message sent." << std::endl;
  }

  void Client::receiveRoutine(int socket) {
    while (true) {
      size_t len;
      Client::receive(socket, (char *)&len, sizeof(size_t));
      char *msg = new char[sizeof(size_t) + len];
      *((size_t *)msg) = len;
      Client::receive(socket, msg + sizeof(size_t), len - sizeof(size_t));
      Message message(msg);
      switch (message.getType()) {
        case MT_AIRCRAFT_POSITION:
          Aircraft ac(message);
          break;
      }
      std::cout << getpid() << ": Message received." << std::endl;
    }
  }

  void Client::receive(int socket, char *msg, size_t len) {
    size_t lenRemaining = len;
    while (lenRemaining > 0) {
      int lenReceived = read(socket,
                             (char *)msg + (len - lenRemaining),
                             lenRemaining);
      if (lenReceived < 0) {
        throw std::string("Cannot receive message.");
      }
      lenRemaining -= lenReceived;
    }
  }
};