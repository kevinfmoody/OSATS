#include "osats/client.hpp"

#include <poll.h>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace osats {

Client::~Client() { disconnect(); }

void Client::connect(const std::string& host, std::uint16_t port,
                     const std::string& callsign, Role role) {
  if (connected_.load()) return;
  callsign_ = callsign;
  auto sock = net::Socket::connectTcp(host, port);
  sock.setNonBlocking();
  conn_ = std::make_unique<net::FramedConnection>(std::move(sock));
  conn_->enqueue(encodeHello(callsign, role, kProtocolVersion));

  stop_.store(false);
  connected_.store(true);
  io_thread_ = std::thread([this] { ioLoop(); });

  // Wait briefly for HelloAck.
  for (int i = 0; i < 100 && entity_id_.load() == kInvalidEntityId &&
                   connected_.load();
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (entity_id_.load() == kInvalidEntityId) {
    disconnect();
    throw std::runtime_error("timed out waiting for HelloAck");
  }
}

void Client::disconnect() {
  stop_.store(true);
  if (conn_) {
    try {
      conn_->enqueue(encodeGoodbye("client disconnect"));
      conn_->writable();
    } catch (...) {
    }
  }
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  if (conn_) {
    conn_->close();
    conn_.reset();
  }
  connected_.store(false);
  entity_id_.store(kInvalidEntityId);
  std::lock_guard lock(peers_mu_);
  peers_.clear();
}

void Client::sendPosition(const Position& pos, double visibility_nm) {
  if (!conn_ || !connected_.load()) {
    throw std::runtime_error("not connected");
  }
  conn_->enqueue(encodePositionUpdate(pos, visibility_nm));
}

void Client::sendHeartbeat() {
  if (!conn_ || !connected_.load()) return;
  using clock = std::chrono::system_clock;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock::now().time_since_epoch())
                      .count();
  conn_->enqueue(encodeHeartbeat(static_cast<std::uint64_t>(ms)));
}

std::unordered_map<EntityId, Peer> Client::peers() const {
  std::lock_guard lock(peers_mu_);
  return peers_;
}

void Client::ioLoop() {
  while (!stop_.load() && conn_ && conn_->valid()) {
    pollfd pfd{conn_->fd(), POLLIN, 0};
    if (conn_->wantsWrite()) pfd.events |= POLLOUT;
    const int rc = ::poll(&pfd, 1, 50);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    if (pfd.revents & POLLIN) {
      const bool ok = conn_->readable(
          [this](const DecodedFrame& frame) { handleFrame(frame); });
      if (!ok) break;
    }
    if (pfd.revents & POLLOUT) {
      if (!conn_->writable()) break;
    } else if (conn_->wantsWrite()) {
      if (!conn_->writable()) break;
    }
  }
  connected_.store(false);
}

void Client::handleFrame(const DecodedFrame& frame) {
  try {
    BufferReader r(frame.payload, frame.payload_size);
    switch (frame.type) {
      case MessageType::HelloAck: {
        const auto magic = r.readU32();
        const auto version = r.readU16();
        const auto id = r.readU32();
        (void)r.readU16();  // tick_hz
        if (magic != kProtocolMagic || version != kProtocolVersion) {
          if (error_cb_) {
            error_cb_(ErrorCode::UnsupportedVersion, "bad hello ack");
          }
          stop_.store(true);
          return;
        }
        entity_id_.store(id);
        break;
      }
      case MessageType::PeerUpdate: {
        Peer peer;
        peer.id = r.readU32();
        peer.role = static_cast<Role>(r.readU8());
        peer.callsign = r.readString();
        peer.position = readPosition(r);
        {
          std::lock_guard lock(peers_mu_);
          peers_[peer.id] = peer;
        }
        if (peer_cb_) peer_cb_(peer);
        break;
      }
      case MessageType::PeerRemove: {
        const EntityId id = r.readU32();
        {
          std::lock_guard lock(peers_mu_);
          peers_.erase(id);
        }
        if (remove_cb_) remove_cb_(id);
        break;
      }
      case MessageType::Error: {
        const auto code = static_cast<ErrorCode>(r.readU16());
        const auto msg = r.readString();
        if (error_cb_) error_cb_(code, msg);
        if (code == ErrorCode::CallsignTaken ||
            code == ErrorCode::UnsupportedVersion) {
          stop_.store(true);
        }
        break;
      }
      case MessageType::Heartbeat:
        break;
      case MessageType::Goodbye:
        stop_.store(true);
        break;
      default:
        break;
    }
  } catch (const std::exception& ex) {
    if (error_cb_) error_cb_(ErrorCode::InvalidFrame, ex.what());
  }
}

}  // namespace osats
