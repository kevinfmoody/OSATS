#include "osats/server.hpp"

#include <poll.h>
#include <signal.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace osats {

Server::Server(ServerConfig config) : config_(config) {
  listener_ = net::Socket::listenTcp(config_.port);
  if (config_.enable_http) {
    http_ = std::make_unique<HttpApi>(
        config_.http_port,
        [this] { return buildTrafficJson(world_); },
        [this] {
          return buildStatusJson(world_, config_.port, config_.http_port,
                                 sessions_by_fd_.size());
        });
  }
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
  std::cout << "OSATS " << kServerVersion << " listening on :" << config_.port
            << " (protocol v" << kProtocolVersion << ", single world)\n";
  if (http_) {
    std::cout << "HTTP status/map on :" << config_.http_port
              << "  →  http://127.0.0.1:" << config_.http_port << "/\n";
  }

  const int tick_ms = std::max(1, 1000 / static_cast<int>(config_.tick_hz));
  while (!stop_.load()) {
    pollOnce(tick_ms);
    const auto now = nowMs();
    sweepIdle(now);
    if (now - last_stats_log_ms_ >= 5000) {
      const auto s = world_.stats();
      std::cout << "stats entities=" << s.entities
                << " pos=" << s.position_updates
                << " peer_sent=" << s.peer_updates_sent
                << " texts=" << s.texts_sent
                << " fps=" << s.flight_plans
                << " sessions=" << sessions_by_fd_.size() << "\n";
      last_stats_log_ms_ = now;
    }
  }
}

void Server::pollOnce(int timeout_ms) {
  std::vector<pollfd> pfds;
  pfds.reserve(sessions_by_fd_.size() + 8);
  pfds.push_back(pollfd{listener_.fd(), POLLIN, 0});
  const std::size_t http_listen_index = http_ ? pfds.size() : static_cast<std::size_t>(-1);
  if (http_) {
    pfds.push_back(pollfd{http_->listenFd(), POLLIN, 0});
    http_->collectPollFds(pfds);
  }
  const std::size_t session_begin = pfds.size();
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

  if (pfds[0].revents & POLLIN) acceptNew();

  if (http_) {
    if (http_listen_index < pfds.size() && (pfds[http_listen_index].revents & POLLIN)) {
      http_->acceptNew();
    }
    for (std::size_t i = http_listen_index + 1; i < session_begin; ++i) {
      const int fd = pfds[i].fd;
      if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        http_->drop(fd);
        continue;
      }
      if (pfds[i].revents & POLLIN) http_->onReadable(fd);
      if (pfds[i].revents & POLLOUT) http_->onWritable(fd);
    }
  }

  std::vector<int> to_drop;
  for (std::size_t i = session_begin; i < pfds.size(); ++i) {
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
      disconnect(*it->second, it->second->pending_disconnect ? "client goodbye"
                                                             : "connection closed");
    }
  }
}

void Server::sweepIdle(std::uint64_t now_ms) {
  if (config_.idle_timeout_ms == 0) return;
  std::vector<int> to_drop;
  for (auto& [fd, session] : sessions_by_fd_) {
    if (now_ms - session->last_heartbeat_ms > config_.idle_timeout_ms) {
      to_drop.push_back(fd);
    }
  }
  for (int fd : to_drop) {
    auto it = sessions_by_fd_.find(fd);
    if (it != sessions_by_fd_.end()) disconnect(*it->second, "idle timeout");
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
    // openfsd-style server identification before login.
    session->conn->enqueue(
        encodeServerAnnounce(kServerName, kServerVersion, kProtocolVersion));
    sessions_by_fd_.emplace(fd, std::move(session));
    if (config_.verbose) std::cout << "accepted fd=" << fd << "\n";
  }
}

void Server::onFrame(Session& session, const DecodedFrame& frame) {
  session.last_heartbeat_ms = nowMs();
  switch (frame.type) {
    case MessageType::Hello: handleHello(session, frame); break;
    case MessageType::PositionUpdate: handlePosition(session, frame); break;
    case MessageType::Heartbeat: handleHeartbeat(session, frame); break;
    case MessageType::FlightPlanSet: handleFlightPlan(session, frame); break;
    case MessageType::TextMessage: handleText(session, frame); break;
    case MessageType::RadioTune: handleRadioTune(session, frame); break;
    case MessageType::Ping: handlePing(session, frame); break;
    case MessageType::ClientQuery: handleQuery(session, frame); break;
    case MessageType::MetarRequest: handleMetar(session, frame); break;
    case MessageType::Goodbye: session.pending_disconnect = true; break;
    default:
      session.conn->enqueue(
          encodeError(ErrorCode::ProtocolViolation, "unsupported message type"));
      break;
  }
}

void Server::requireAuth(Session& session) {
  if (!session.authed) {
    throw std::runtime_error("not authenticated");
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
    std::string real_name;
    if (r.remaining() > 0) real_name = r.readString();

    if (magic != kProtocolMagic) {
      session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, "bad magic"));
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
    auto& state = world_.spawn(id, callsign, role, real_name, nowMs());
    state.visibility_nm = config_.default_visibility_nm;
    session.entity_id = id;
    session.callsign = callsign;
    session.real_name = real_name;
    session.role = role;
    session.authed = true;
    sessions_by_id_[id] = &session;

    session.conn->enqueue(
        encodeHelloAck(id, kProtocolVersion, config_.tick_hz, kServerName));
    if (config_.verbose) {
      std::cout << "hello id=" << id << " callsign=" << callsign
                << " role=" << roleName(role) << "\n";
    }
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::handlePosition(Session& session, const DecodedFrame& frame) {
  try {
    requireAuth(session);
    BufferReader r(frame.payload, frame.payload_size);
    Position pos = readPosition(r);
    double vis = config_.default_visibility_nm;
    if (r.remaining() >= 2) vis = static_cast<double>(r.readU16());

    const bool first = world_.get(session.entity_id) &&
                       world_.get(session.entity_id)->last_update_ms == 0;
    world_.updatePosition(session.entity_id, pos, vis, nowMs());

    if (first) {
      world_.forEachVisibleTo(session.entity_id, [this, &session](const AircraftState& peer) {
        session.conn->enqueue(encodePeerUpdate(peer));
        if (peer.flight_plan) {
          session.conn->enqueue(
              encodeFlightPlanUpdate(peer.id, peer.callsign, *peer.flight_plan));
        }
      });
    }
    broadcastPeerUpdate(session.entity_id);
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::ProtocolViolation, ex.what()));
  }
}

void Server::handleHeartbeat(Session& session, const DecodedFrame&) {
  session.last_heartbeat_ms = nowMs();
  session.conn->enqueue(encodeHeartbeat(session.last_heartbeat_ms));
}

void Server::handleFlightPlan(Session& session, const DecodedFrame& frame) {
  try {
    requireAuth(session);
    BufferReader r(frame.payload, frame.payload_size);
    FlightPlan fp = readFlightPlan(r);
    world_.setFlightPlan(session.entity_id, fp);
    // Notify everyone who can currently see this aircraft (force).
    const auto now = nowMs();
    world_.forEachInterestedObserver(
        session.entity_id, now, true,
        [this, &session, &fp](EntityId observer, geo::InterestTier) {
          sendTo(observer,
                 encodeFlightPlanUpdate(session.entity_id, session.callsign, fp));
        });
    if (config_.verbose) {
      std::cout << "fp " << session.callsign << " " << fp.departure << "->"
                << fp.arrival << "\n";
    }
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::handleText(Session& session, const DecodedFrame& frame) {
  try {
    requireAuth(session);
    BufferReader r(frame.payload, frame.payload_size);
    const auto scope = static_cast<TextScope>(r.readU8());
    (void)r.readString();  // client-provided from; trust session callsign
    const auto to = r.readString();
    const auto text = r.readString16();
    const auto from = session.callsign;
    auto* self = world_.get(session.entity_id);
    if (!self) return;

    auto deliver = [&](EntityId id) {
      sendTo(id, encodeTextMessage(scope, from, to, text));
      world_.noteTextSent();
    };

    switch (scope) {
      case TextScope::Private: {
        auto* target = world_.findByCallsign(to);
        if (!target) {
          session.conn->enqueue(encodeError(ErrorCode::NotFound, "callsign not found"));
          return;
        }
        deliver(target->id);
        break;
      }
      case TextScope::Radio: {
        world_.forEachOnFrequency(self->radio_hz, session.entity_id,
                                  [&](const AircraftState& ac) {
                                    // Also require mutual range for radio realism.
                                    if (geo::distanceNm(self->position, ac.position) <=
                                        std::max(self->visibility_nm, ac.visibility_nm)) {
                                      deliver(ac.id);
                                    }
                                  });
        break;
      }
      case TextScope::Broadcast:
        world_.forEach([&](const AircraftState& ac) {
          if (ac.id != session.entity_id) deliver(ac.id);
        });
        break;
      case TextScope::Wallop:
        world_.forEachController([&](const AircraftState& ac) { deliver(ac.id); });
        break;
    }
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::handleRadioTune(Session& session, const DecodedFrame& frame) {
  try {
    requireAuth(session);
    BufferReader r(frame.payload, frame.payload_size);
    world_.setRadio(session.entity_id, r.readU32());
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::handlePing(Session& session, const DecodedFrame& frame) {
  try {
    BufferReader r(frame.payload, frame.payload_size);
    session.conn->enqueue(encodePong(r.readU32()));
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::handleQuery(Session& session, const DecodedFrame& frame) {
  try {
    requireAuth(session);
    BufferReader r(frame.payload, frame.payload_size);
    const auto type = static_cast<QueryType>(r.readU8());
    const auto target = r.readString();
    std::ostringstream body;

    switch (type) {
      case QueryType::Server:
        body << kServerName << ' ' << kServerVersion << " proto=" << kProtocolVersion
             << " entities=" << world_.stats().entities;
        break;
      case QueryType::ATIS:
        body << weather_.atisFor(target);
        break;
      case QueryType::Info: {
        const auto* ac = world_.findByCallsign(target);
        if (!ac) {
          session.conn->enqueue(encodeError(ErrorCode::NotFound, "callsign not found"));
          return;
        }
        body << ac->callsign << " (" << roleName(ac->role) << ") "
             << ac->real_name << " lat=" << ac->position.latitude_deg
             << " lon=" << ac->position.longitude_deg
             << " alt=" << ac->position.altitude_ft
             << " squawk=" << ac->position.squawk;
        break;
      }
      case QueryType::FlightPlan: {
        const auto* ac = world_.findByCallsign(target);
        if (!ac || !ac->flight_plan) {
          session.conn->enqueue(encodeError(ErrorCode::NotFound, "no flight plan"));
          return;
        }
        const auto& fp = *ac->flight_plan;
        body << fp.flight_rules << ' ' << fp.aircraft_type << ' ' << fp.departure
             << '-' << fp.arrival << " FL" << (fp.cruise_alt_ft / 100) << " via "
             << fp.route;
        break;
      }
    }

    session.conn->enqueue(encodeClientQueryReply(type, target, body.str()));
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::handleMetar(Session& session, const DecodedFrame& frame) {
  try {
    requireAuth(session);
    BufferReader r(frame.payload, frame.payload_size);
    const auto icao = r.readString();
    session.conn->enqueue(encodeMetarResponse(icao, weather_.metarFor(icao)));
  } catch (const std::exception& ex) {
    session.conn->enqueue(encodeError(ErrorCode::InvalidFrame, ex.what()));
  }
}

void Server::broadcastPeerUpdate(EntityId subject, bool force) {
  const auto* state = world_.get(subject);
  if (!state) return;
  const auto now = nowMs();
  world_.forEachInterestedObserver(
      subject, now, force, [this, state](EntityId observer, geo::InterestTier) {
        sendTo(observer, encodePeerUpdate(*state));
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
    if (world_.get(id)) {
      const auto now = nowMs();
      world_.forEachInterestedObserver(
          id, now, true, [this, id](EntityId observer, geo::InterestTier) {
            sendTo(observer, encodePeerRemove(id));
            world_.notePeerRemoveSent();
          });
    }
    sessions_by_id_.erase(id);
    world_.despawn(id);
  }
  if (session.conn) session.conn->close();
  if (fd >= 0) sessions_by_fd_.erase(fd);
  if (config_.verbose) {
    std::cout << "disconnect fd=" << fd << " reason=" << reason << "\n";
  }
}

}  // namespace osats
