#include "osats/server.hpp"

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace osats {

Server::Server(ServerConfig config) : config_(config) {
  listener_ = net::Socket::listenTcp(config_.port);
}

Server::~Server() = default;

std::uint64_t Server::nowMs() {
  using clock = std::chrono::steady_clock;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          clock::now().time_since_epoch())
          .count());
}

void Server::requestStop() { stop_.store(true); }

void Server::run() {
  std::cout << "OSATS listening on port " << config_.port
            << " (protocol v" << kProtocolVersion << ", single world)\n";
  const int tick_ms = std::max(1, 1000 / static_cast<int>(config_.tick_hz));
  while (!stop_.load()) {
    pollOnce(tick_ms);
    const auto now = nowMs();
    if (now - last_stats_log_ms_ >= 5000) {
      const auto s = world_.stats();
      std::cout << "stats entities=" << s.entities
                << " pos_updates=" << s.position_updates
                << " peer_sent=" << s.peer_updates_sent
                << " removes=" << s.peer_removes_sent
                << " sessions=" << sessions_by_fd_.size() << "\n";
      last_stats_log_ms_ = now;
    }
  }
}

void Server::pollOnce(int timeout_ms) {
  std::vector<pollfd> pfds;
  pfds.reserve(sessions_by_fd_.size() + 1);
  pfds.push_back(pollfd{listener_.fd(), POLLIN, 0});
  for (auto& [fd, session] : sessions_by_fd_) {
    short events = POLLIN;
    if (session->conn->wantsWrite()) events |= POLLOUT;
    pfds.push_back(pollfd{fd, events, 0});
  }

  const int rc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);
  if (rc < 0) {
    if (errno == EINTR) return;
    throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
  }

  if (pfds[0].revents & POLLIN) {
    acceptNew();
  }

  std::vector<int> to_drop;
  for (std::size_t i = 1; i < pfds.size(); ++i) {
    const int fd = pfds[i].fd;
    auto it = sessions_by_fd_.find(fd);
    if (it == sessions_by_fd_.end()) continue;
    Session& session = *it->second;

    if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      to_drop.push_back(fd);
      continue;
    }

    if (pfds[i].revents & POLLIN) {
      const bool ok = session.conn->readable(
          [this, &session](const DecodedFrame& frame) { onFrame(session, frame); });
      if (!ok) {
        to_drop.push_back(fd);
        continue;
      }
    }

    if (pfds[i].revents & POLLOUT) {
      if (!session.conn->writable()) {
        to_drop.push_back(fd);
        continue;
      }
    }

    if (session.pending_disconnect ||
        session.conn->outboundBytes() > config_.max_outbound_bytes) {
      to_drop.push_back(fd);
    }
  }

  for (int fd : to_drop) {
    auto it = sessions_by_fd_.find(fd);
    if (it != sessions_by_fd_.end()) {
      disconnect(*it->second, it->second->pending_disconnect
                                  ? "client goodbye"
                                  : "connection closed");
    }
  }
}

void Server::acceptNew() {
  while (true) {
    auto sock = listener_.accept();
    if (!sock.valid()) break;
    auto session = std::make_unique<Session>();
    const int fd = sock.fd();
    session->conn = std::make_unique<net::FramedConnection>(std::move(sock));
    session->last_heartbeat_ms = nowMs();
    sessions_by_fd_.emplace(fd, std::move(session));
    if (config_.verbose) {
      std::cout << "accepted fd=" << fd << "\n";
    }
  }
}

void Server::onFrame(Session& session, const DecodedFrame& frame) {
  switch (frame.type) {
    case MessageType::Hello:
      handleHello(session, frame);
      break;
    case MessageType::PositionUpdate:
      handlePosition(session, frame);
      break;
    case MessageType::Heartbeat:
      handleHeartbeat(session, frame);
      break;
    case MessageType::Goodbye:
      session.pending_disconnect = true;
      break;
    default:
      session.conn->enqueue(encodeError(ErrorCode::ProtocolViolation,
                                        "unsupported message type"));
      break;
  }
}

void Server::handleHello(Session& session, const DecodedFrame& frame) {
  if (session.authed) {
    session.conn->enqueue(
        encodeError(ErrorCode::ProtocolViolation, "already authenticated"));
    return;
  }
  try {
    BufferReader r(frame.payload, frame.payload_size);
    const auto magic = r.readU32();
    const auto version = r.readU16();
    const auto role = static_cast<Role>(r.readU8());
    const auto callsign = r.readString();
    if (magic != kProtocolMagic) {
      session.conn->enqueue(
          encodeError(ErrorCode::InvalidFrame, "bad magic"));
      return;
    }
    if (version != kProtocolVersion) {
      session.conn->enqueue(
          encodeError(ErrorCode::UnsupportedVersion, "unsupported version"));
      return;
    }
    if (callsign.empty() || world_.callsignTaken(callsign)) {
      session.conn->enqueue(
          encodeError(ErrorCode::CallsignTaken, "callsign unavailable"));
      return;
    }

    const EntityId id = world_.allocateId();
    auto& state = world_.spawn(id, callsign, role);
    state.visibility_nm = config_.default_visibility_nm;
    session.entity_id = id;
    session.callsign = callsign;
    session.role = role;
    session.authed = true;
    session.last_heartbeat_ms = nowMs();
    sessions_by_id_[id] = &session;

    session.conn->enqueue(
        encodeHelloAck(id, kProtocolVersion, config_.tick_hz));

    if (config_.verbose) {
      std::cout << "hello id=" << id << " callsign=" << callsign << "\n";
    }
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::handlePosition(Session& session, const DecodedFrame& frame) {
  if (!session.authed) {
    session.conn->enqueue(
        encodeError(ErrorCode::ProtocolViolation, "not authenticated"));
    return;
  }
  try {
    BufferReader r(frame.payload, frame.payload_size);
    Position pos = readPosition(r);
    double vis = config_.default_visibility_nm;
    if (r.remaining() >= 2) {
      vis = static_cast<double>(r.readU16());
    }
    const bool first =
        world_.get(session.entity_id) &&
        world_.get(session.entity_id)->last_update_ms == 0;

    world_.updatePosition(session.entity_id, pos, vis, nowMs());

    if (first) {
      // On first fix, send currently visible peers to the joiner.
      world_.forEachVisibleTo(session.entity_id, [this, &session](const AircraftState& peer) {
        session.conn->enqueue(encodePeerUpdate(peer.id, peer.callsign, peer.role,
                                               peer.position));
      });
    }

    broadcastPeerUpdate(session.entity_id);
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::handleHeartbeat(Session& session, const DecodedFrame&) {
  session.last_heartbeat_ms = nowMs();
  session.conn->enqueue(encodeHeartbeat(session.last_heartbeat_ms));
}

void Server::broadcastPeerUpdate(EntityId subject) {
  const auto* state = world_.get(subject);
  if (!state) return;
  const auto now = nowMs();
  world_.forEachInterestedObserver(
      subject, now, [this, state](EntityId observer, geo::InterestTier) {
        sendTo(observer,
               encodePeerUpdate(state->id, state->callsign, state->role,
                                state->position));
      });
}

void Server::sendTo(EntityId id, std::vector<char> frame) {
  auto it = sessions_by_id_.find(id);
  if (it == sessions_by_id_.end() || !it->second->conn) return;
  it->second->conn->enqueue(std::move(frame));
}

void Server::disconnect(Session& session, const std::string& reason) {
  const int fd = session.conn ? session.conn->fd() : -1;
  if (session.authed) {
    const EntityId id = session.entity_id;
    // Notify observers before despawn.
    const auto* state = world_.get(id);
    if (state) {
      const auto now = nowMs();
      world_.forEachInterestedObserver(
          id, now, [this, id](EntityId observer, geo::InterestTier) {
            sendTo(observer, encodePeerRemove(id));
            world_.notePeerRemoveSent();
          });
    }
    sessions_by_id_.erase(id);
    world_.despawn(id);
  }
  if (session.conn) {
    session.conn->close();
  }
  if (fd >= 0) {
    sessions_by_fd_.erase(fd);
  }
  if (config_.verbose) {
    std::cout << "disconnect fd=" << fd << " reason=" << reason << "\n";
  }
}

}  // namespace osats
