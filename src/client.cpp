#include "osats/client.hpp"

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <stdexcept>

namespace osats {

Client::~Client() { disconnect(); }

void Client::enqueue(std::vector<char> frame) {
  if (!conn_ || !connected_.load()) throw std::runtime_error("not connected");
  conn_->enqueue(std::move(frame));
}

void Client::connect(const std::string& host, std::uint16_t port,
                     const std::string& callsign, Role role,
                     const std::string& real_name) {
  if (connected_.load()) return;
  callsign_ = callsign;
  auto sock = net::Socket::connectTcp(host, port);
  sock.setNonBlocking();
  conn_ = std::make_unique<net::FramedConnection>(std::move(sock));

  stop_.store(false);
  announced_.store(false);
  connected_.store(true);
  io_thread_ = std::thread([this] { ioLoop(); });

  // Wait for ServerAnnounce then send Hello.
  for (int i = 0; i < 100 && !announced_.load() && connected_.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  conn_->enqueue(encodeHello(callsign, role, real_name, kProtocolVersion));

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
  if (conn_ && connected_.load()) {
    try {
      conn_->enqueue(encodeGoodbye("client disconnect"));
      conn_->writable();
    } catch (...) {
    }
  }
  if (io_thread_.joinable()) io_thread_.join();
  if (conn_) {
    conn_->close();
    conn_.reset();
  }
  connected_.store(false);
  entity_id_.store(kInvalidEntityId);
  std::lock_guard lock(mu_);
  peers_.clear();
  texts_.clear();
  server_name_.clear();
}

std::string Client::serverName() const {
  std::lock_guard lock(mu_);
  return server_name_;
}

void Client::sendPosition(const Position& pos, double visibility_nm) {
  enqueue(encodePositionUpdate(pos, visibility_nm));
}

void Client::sendHeartbeat() {
  using clock = std::chrono::system_clock;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock::now().time_since_epoch())
                      .count();
  enqueue(encodeHeartbeat(static_cast<std::uint64_t>(ms)));
}

void Client::sendFlightPlan(const FlightPlan& fp) {
  enqueue(encodeFlightPlanSet(fp));
}

void Client::sendText(TextScope scope, const std::string& to_or_freq,
                      const std::string& text) {
  enqueue(encodeTextMessage(scope, callsign_, to_or_freq, text));
}

void Client::tuneRadio(std::uint32_t radio_hz) {
  enqueue(encodeRadioTune(radio_hz));
}

void Client::ping(std::uint32_t nonce) { enqueue(encodePing(nonce)); }

void Client::query(QueryType type, const std::string& target) {
  enqueue(encodeClientQuery(type, target));
}

void Client::requestMetar(const std::string& icao) {
  enqueue(encodeMetarRequest(icao));
}

std::unordered_map<EntityId, Peer> Client::peers() const {
  std::lock_guard lock(mu_);
  return peers_;
}

std::vector<TextEvent> Client::drainTexts() {
  std::lock_guard lock(mu_);
  std::vector<TextEvent> out;
  out.swap(texts_);
  return out;
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
      const bool ok =
          conn_->readable([this](const DecodedFrame& frame) { handleFrame(frame); });
      if (!ok) break;
    }
    if ((pfd.revents & POLLOUT) || conn_->wantsWrite()) {
      if (!conn_->writable()) break;
    }
  }
  connected_.store(false);
}

void Client::handleFrame(const DecodedFrame& frame) {
  try {
    BufferReader r(frame.payload, frame.payload_size);
    switch (frame.type) {
      case MessageType::ServerAnnounce: {
        (void)r.readU32();
        (void)r.readU16();
        const auto name = r.readString();
        const auto version = r.readString();
        {
          std::lock_guard lock(mu_);
          server_name_ = name + " " + version;
        }
        announced_.store(true);
        if (announce_cb_) announce_cb_(name, version);
        break;
      }
      case MessageType::HelloAck: {
        const auto magic = r.readU32();
        const auto version = r.readU16();
        const auto id = r.readU32();
        (void)r.readU16();
        if (r.remaining() > 0) {
          std::lock_guard lock(mu_);
          server_name_ = r.readString();
        }
        if (magic != kProtocolMagic || version != kProtocolVersion) {
          if (error_cb_) error_cb_(ErrorCode::UnsupportedVersion, "bad hello ack");
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
        if (r.remaining() > 0) peer.real_name = r.readString();
        peer.position = readPosition(r);
        if (r.remaining() >= 4) peer.radio_hz = r.readU32();
        if (r.remaining() >= 1) (void)r.readU8();  // has_fp flag
        {
          std::lock_guard lock(mu_);
          auto it = peers_.find(peer.id);
          if (it != peers_.end() && it->second.flight_plan) {
            peer.flight_plan = it->second.flight_plan;
          }
          peers_[peer.id] = peer;
        }
        if (peer_cb_) peer_cb_(peer);
        break;
      }
      case MessageType::PeerRemove: {
        const EntityId id = r.readU32();
        {
          std::lock_guard lock(mu_);
          peers_.erase(id);
        }
        if (remove_cb_) remove_cb_(id);
        break;
      }
      case MessageType::FlightPlanUpdate: {
        const EntityId id = r.readU32();
        const auto cs = r.readString();
        FlightPlan fp = readFlightPlan(r);
        std::lock_guard lock(mu_);
        auto& peer = peers_[id];
        peer.id = id;
        peer.callsign = cs;
        peer.flight_plan = std::move(fp);
        break;
      }
      case MessageType::TextMessage: {
        TextEvent ev;
        ev.scope = static_cast<TextScope>(r.readU8());
        ev.from = r.readString();
        ev.to = r.readString();
        ev.text = r.readString16();
        {
          std::lock_guard lock(mu_);
          texts_.push_back(ev);
        }
        if (text_cb_) text_cb_(ev);
        break;
      }
      case MessageType::MetarResponse: {
        const auto icao = r.readString();
        const auto metar = r.readString16();
        if (metar_cb_) metar_cb_(icao, metar);
        break;
      }
      case MessageType::ClientQueryReply: {
        const auto type = static_cast<QueryType>(r.readU8());
        const auto target = r.readString();
        const auto body = r.readString16();
        if (reply_cb_) reply_cb_(type, target, body);
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
      case MessageType::Pong:
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
