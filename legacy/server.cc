#include "server.h"
#include "message.h"
#include "aircraft.h"

#include <iostream>
#include <string>

namespace OSATS {
  Server::Server(int port) {
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addrLen = sizeof(struct sockaddr_in);
    int conn, events;
    int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
      throw std::string("Cannot open socket.");
    }
    bzero((char *)&serverAddr, addrLen);
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);
    if (bind(socket,
             (struct sockaddr *)&serverAddr,
             sizeof(struct sockaddr_in)) < 0) {
      throw std::string("Cannot bind.");
    }
    listen(socket, SOMAXCONN);
    int kq = kqueue();
    if (kq < 0) {
      throw std::string("Cannot build kernel queue.");
    }
    struct kevent evSet;
    struct kevent evList[32];
    EV_SET(&evSet, socket, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &evSet, 1, NULL, 0, NULL) == -1) {
      throw std::string("Cannot setup listen event.");
    }
    std::cout << "Listening on port " << port << "..." << std::endl;
    while (true) {
      events = kevent(kq, NULL, 0, evList, 32, NULL);
      if (events < 1) {
        throw std::string("Cannot retrieve events.");
      } else {
        for (int i = 0; i < events; i++) {
          if (evList[i].ident == socket) {
            conn = accept(socket,
                          (struct sockaddr *)&clientAddr,
                          &addrLen);
            if (conn < 0) {
              throw std::string("Cannot accept connection.");
            }
            EV_SET(&evSet, conn, EVFILT_READ, EV_ADD, 0, 0, NULL);
            if (kevent(kq, &evSet, 1, NULL, 0, NULL) == -1) {
              throw std::string("Cannot setup client event.");
            }
            std::cout << "New connection accepted." << std::endl;
          } else if (evList[i].flags & EV_EOF) {
            EV_SET(&evSet, evList[i].ident, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            if (kevent(kq, &evSet, 1, NULL, 0, NULL) == -1) {
              throw std::string("Cannot delete client event.");
            }
            this->aircraftStore.remove(evList[i].ident);
            std::cout << "Connection closed." << std::endl;
          } else if (evList[i].flags & EVFILT_READ) {
            this->handleMessage(evList[i].ident);
          }
        }
      }
    }
  }

  void Server::handleMessage(int socket) {
    size_t len;
    this->receive(socket, (char *)&len, sizeof(size_t));
    char *msg = new char[sizeof(size_t) + len];
    *((size_t *)msg) = len;
    this->receive(socket, msg + sizeof(size_t), len - sizeof(size_t));
    Message message(msg);
    switch (message.getType()) {
      case MT_AIRCRAFT_POSITION:
        Aircraft *ac = new Aircraft(message);
        this->aircraftStore.put(socket, ac);
        this->broadcastAircraftPosition(socket, message);
        break;
    }
    std::cout << "Message received." << std::endl;
  }

  void Server::broadcastAircraftPosition(int socket, Message &msg) {
    std::unordered_set<int> &neighbors
      = this->aircraftStore.getNeighbors(socket);
    for (int neighbor : neighbors) {
      this->send(neighbor, msg);
    }
  }

  void Server::send(int socket, Message &msg) {
    size_t len = msg.getLen();
    int lenRemaining = len;
    while (lenRemaining > 0) {
      int lenSent = write(socket,
                          msg.getMsg() + (len - lenRemaining),
                          lenRemaining);
      if (lenSent < 0) {
        throw std::string("Cannot send message.");
      }
      lenRemaining -= lenSent;
    }
    std::cout << "Message sent." << std::endl;
  }

  void Server::receive(int socket, char *msg, size_t len) {
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