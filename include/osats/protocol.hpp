#pragma once

#include "osats/types.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace osats {

// Wire framing (little-endian payloads):
//   [u32 length][u16 type][payload...]
// length = sizeof(type) + payload size (excludes the length field).
inline constexpr std::size_t kFrameHeaderSize = 6;

class BufferWriter {
 public:
  explicit BufferWriter(std::size_t reserve = 64) { buf_.reserve(reserve); }

  void writeU8(std::uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void writeU16(std::uint16_t v) {
    buf_.push_back(static_cast<char>(v & 0xff));
    buf_.push_back(static_cast<char>((v >> 8) & 0xff));
  }
  void writeU32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
  }
  void writeI32(std::int32_t v) { writeU32(static_cast<std::uint32_t>(v)); }
  void writeI16(std::int16_t v) { writeU16(static_cast<std::uint16_t>(v)); }
  void writeU64(std::uint64_t v) {
    writeU32(static_cast<std::uint32_t>(v & 0xffffffffu));
    writeU32(static_cast<std::uint32_t>((v >> 32) & 0xffffffffu));
  }
  void writeF32(float v) {
    static_assert(sizeof(float) == 4);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU32(bits);
  }

  void writeString(std::string_view s) {
    if (s.size() > 255) throw std::runtime_error("string exceeds 255 bytes");
    writeU8(static_cast<std::uint8_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }

  void writeString16(std::string_view s) {
    if (s.size() > 65535) throw std::runtime_error("string exceeds 65535 bytes");
    writeU16(static_cast<std::uint16_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }

  void writeBytes(const void* data, std::size_t n) {
    const auto* p = static_cast<const char*>(data);
    buf_.insert(buf_.end(), p, p + n);
  }

  std::vector<char>& data() { return buf_; }
  const std::vector<char>& data() const { return buf_; }

 private:
  std::vector<char> buf_;
};

class BufferReader {
 public:
  BufferReader(const char* data, std::size_t size) : data_(data), size_(size) {}

  std::uint8_t readU8() {
    need(1);
    return static_cast<std::uint8_t>(data_[off_++]);
  }
  std::uint16_t readU16() {
    need(2);
    auto v = static_cast<std::uint16_t>(static_cast<std::uint8_t>(data_[off_])) |
             (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data_[off_ + 1])) << 8);
    off_ += 2;
    return v;
  }
  std::uint64_t readU64() {
    const auto lo = readU32();
    const auto hi = readU32();
    return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
  }
  std::uint32_t readU32() {
    need(4);
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[off_ + i])) << (8 * i);
    }
    off_ += 4;
    return v;
  }
  std::int32_t readI32() { return static_cast<std::int32_t>(readU32()); }
  std::int16_t readI16() { return static_cast<std::int16_t>(readU16()); }
  float readF32() {
    const auto bits = readU32();
    float v = 0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }

  std::string readString() {
    const auto n = readU8();
    need(n);
    std::string s(data_ + off_, data_ + off_ + n);
    off_ += n;
    return s;
  }

  std::string readString16() {
    const auto n = readU16();
    need(n);
    std::string s(data_ + off_, data_ + off_ + n);
    off_ += n;
    return s;
  }

  std::size_t remaining() const { return size_ - off_; }

 private:
  void need(std::size_t n) const {
    if (off_ + n > size_) throw std::runtime_error("truncated buffer");
  }

  const char* data_;
  std::size_t size_;
  std::size_t off_ = 0;
};

inline std::int32_t encodeCoord(double deg) {
  return static_cast<std::int32_t>(std::llround(deg * 1'000'000.0));
}
inline double decodeCoord(std::int32_t v) { return static_cast<double>(v) / 1'000'000.0; }
inline std::int32_t encodeAltitude(double ft) {
  return static_cast<std::int32_t>(std::llround(ft));
}
inline std::int16_t encodeGs(double kt) {
  return static_cast<std::int16_t>(std::llround(kt));
}
inline std::int16_t encodeHeading(double deg) {
  double n = std::fmod(deg, 360.0);
  if (n < 0) n += 360.0;
  return static_cast<std::int16_t>(std::llround(n * 100.0));
}
inline double decodeHeading(std::int16_t v) { return static_cast<double>(v) / 100.0; }
inline std::int16_t encodeAttitude(double deg) {
  return static_cast<std::int16_t>(std::llround(deg * 100.0));
}
inline double decodeAttitude(std::int16_t v) { return static_cast<double>(v) / 100.0; }

inline void writePosition(BufferWriter& w, const Position& p) {
  w.writeI32(encodeCoord(p.latitude_deg));
  w.writeI32(encodeCoord(p.longitude_deg));
  w.writeI32(encodeAltitude(p.altitude_ft));
  w.writeI16(encodeGs(p.groundspeed_kt));
  w.writeI16(encodeHeading(p.heading_deg));
  w.writeI16(encodeAttitude(p.pitch_deg));
  w.writeI16(encodeAttitude(p.bank_deg));
  w.writeF32(p.vn_fps);
  w.writeF32(p.ve_fps);
  w.writeF32(p.vd_fps);
  w.writeU16(p.squawk);
  w.writeU8(static_cast<std::uint8_t>(p.xpdr_mode));
}

inline Position readPosition(BufferReader& r) {
  Position p;
  p.latitude_deg = decodeCoord(r.readI32());
  p.longitude_deg = decodeCoord(r.readI32());
  p.altitude_ft = static_cast<double>(r.readI32());
  p.groundspeed_kt = static_cast<double>(r.readI16());
  p.heading_deg = decodeHeading(r.readI16());
  if (r.remaining() >= 2 + 2 + 12 + 2 + 1) {
    p.pitch_deg = decodeAttitude(r.readI16());
    p.bank_deg = decodeAttitude(r.readI16());
    p.vn_fps = r.readF32();
    p.ve_fps = r.readF32();
    p.vd_fps = r.readF32();
    p.squawk = r.readU16();
    p.xpdr_mode = static_cast<char>(r.readU8());
  }
  return p;
}

inline void writeFlightPlan(BufferWriter& w, const FlightPlan& fp) {
  w.writeU8(static_cast<std::uint8_t>(fp.flight_rules));
  w.writeString(fp.aircraft_type);
  w.writeString(fp.departure);
  w.writeString(fp.arrival);
  w.writeString(fp.alternate);
  w.writeU16(fp.cruise_alt_ft);
  w.writeU16(fp.tas_kt);
  w.writeString16(fp.route);
  w.writeString16(fp.remarks);
}

inline FlightPlan readFlightPlan(BufferReader& r) {
  FlightPlan fp;
  fp.flight_rules = static_cast<char>(r.readU8());
  fp.aircraft_type = r.readString();
  fp.departure = r.readString();
  fp.arrival = r.readString();
  fp.alternate = r.readString();
  fp.cruise_alt_ft = r.readU16();
  fp.tas_kt = r.readU16();
  fp.route = r.readString16();
  fp.remarks = r.readString16();
  return fp;
}

inline std::vector<char> encodeFrame(MessageType type, const std::vector<char>& payload) {
  BufferWriter w(kFrameHeaderSize + payload.size());
  const auto length = static_cast<std::uint32_t>(2 + payload.size());
  w.writeU32(length);
  w.writeU16(static_cast<std::uint16_t>(type));
  if (!payload.empty()) w.writeBytes(payload.data(), payload.size());
  return w.data();
}

inline std::vector<char> encodeServerAnnounce(std::string_view name,
                                              std::string_view version,
                                              std::uint16_t proto) {
  BufferWriter body;
  body.writeU32(kProtocolMagic);
  body.writeU16(proto);
  body.writeString(name);
  body.writeString(version);
  return encodeFrame(MessageType::ServerAnnounce, body.data());
}

inline std::vector<char> encodeHello(std::string_view callsign, Role role,
                                     std::string_view real_name = "",
                                     std::uint16_t client_version = kProtocolVersion) {
  BufferWriter body;
  body.writeU32(kProtocolMagic);
  body.writeU16(client_version);
  body.writeU8(static_cast<std::uint8_t>(role));
  body.writeString(callsign);
  body.writeString(real_name);
  return encodeFrame(MessageType::Hello, body.data());
}

inline std::vector<char> encodeHelloAck(EntityId id, std::uint16_t server_version,
                                        std::uint16_t tick_hz,
                                        std::string_view server_name = kServerName) {
  BufferWriter body;
  body.writeU32(kProtocolMagic);
  body.writeU16(server_version);
  body.writeU32(id);
  body.writeU16(tick_hz);
  body.writeString(server_name);
  return encodeFrame(MessageType::HelloAck, body.data());
}

inline std::vector<char> encodeHeartbeat(std::uint64_t client_time_ms) {
  BufferWriter body;
  body.writeU32(static_cast<std::uint32_t>(client_time_ms & 0xffffffffu));
  body.writeU32(static_cast<std::uint32_t>((client_time_ms >> 32) & 0xffffffffu));
  return encodeFrame(MessageType::Heartbeat, body.data());
}

inline std::vector<char> encodePositionUpdate(const Position& p, double visibility_nm) {
  BufferWriter body;
  writePosition(body, p);
  body.writeU16(static_cast<std::uint16_t>(std::llround(visibility_nm)));
  return encodeFrame(MessageType::PositionUpdate, body.data());
}

inline std::vector<char> encodePeerUpdate(const AircraftState& ac,
                                         std::uint64_t server_time_ms) {
  BufferWriter body;
  body.writeU32(ac.id);
  body.writeU8(static_cast<std::uint8_t>(ac.role));
  body.writeString(ac.callsign);
  body.writeString(ac.real_name);
  writePosition(body, ac.position);
  body.writeU32(ac.radio_hz);
  body.writeU8(ac.flight_plan ? 1 : 0);
  body.writeU64(server_time_ms);
  return encodeFrame(MessageType::PeerUpdate, body.data());
}

inline std::vector<char> encodePeerRemove(EntityId id) {
  BufferWriter body;
  body.writeU32(id);
  return encodeFrame(MessageType::PeerRemove, body.data());
}

inline std::vector<char> encodeError(ErrorCode code, std::string_view message) {
  BufferWriter body;
  body.writeU16(static_cast<std::uint16_t>(code));
  body.writeString(message);
  return encodeFrame(MessageType::Error, body.data());
}

inline std::vector<char> encodeGoodbye(std::string_view reason) {
  BufferWriter body;
  body.writeString(reason);
  return encodeFrame(MessageType::Goodbye, body.data());
}

inline std::vector<char> encodeFlightPlanSet(const FlightPlan& fp) {
  BufferWriter body;
  writeFlightPlan(body, fp);
  return encodeFrame(MessageType::FlightPlanSet, body.data());
}

inline std::vector<char> encodeFlightPlanUpdate(EntityId id, std::string_view callsign,
                                                const FlightPlan& fp) {
  BufferWriter body;
  body.writeU32(id);
  body.writeString(callsign);
  writeFlightPlan(body, fp);
  return encodeFrame(MessageType::FlightPlanUpdate, body.data());
}

inline std::vector<char> encodeTextMessage(TextScope scope, std::string_view from,
                                           std::string_view to_or_freq,
                                           std::string_view text) {
  BufferWriter body;
  body.writeU8(static_cast<std::uint8_t>(scope));
  body.writeString(from);
  body.writeString(to_or_freq);
  body.writeString16(text);
  return encodeFrame(MessageType::TextMessage, body.data());
}

inline std::vector<char> encodeRadioTune(std::uint32_t radio_hz) {
  BufferWriter body;
  body.writeU32(radio_hz);
  return encodeFrame(MessageType::RadioTune, body.data());
}

inline std::vector<char> encodePing(std::uint32_t nonce) {
  BufferWriter body;
  body.writeU32(nonce);
  return encodeFrame(MessageType::Ping, body.data());
}

inline std::vector<char> encodePong(std::uint32_t nonce) {
  BufferWriter body;
  body.writeU32(nonce);
  return encodeFrame(MessageType::Pong, body.data());
}

inline std::vector<char> encodeClientQuery(QueryType type, std::string_view target) {
  BufferWriter body;
  body.writeU8(static_cast<std::uint8_t>(type));
  body.writeString(target);
  return encodeFrame(MessageType::ClientQuery, body.data());
}

inline std::vector<char> encodeClientQueryReply(QueryType type, std::string_view target,
                                                std::string_view body_text) {
  BufferWriter body;
  body.writeU8(static_cast<std::uint8_t>(type));
  body.writeString(target);
  body.writeString16(body_text);
  return encodeFrame(MessageType::ClientQueryReply, body.data());
}

inline std::vector<char> encodeMetarRequest(std::string_view icao) {
  BufferWriter body;
  body.writeString(icao);
  return encodeFrame(MessageType::MetarRequest, body.data());
}

inline std::vector<char> encodeMetarResponse(std::string_view icao,
                                             std::string_view metar) {
  BufferWriter body;
  body.writeString(icao);
  body.writeString16(metar);
  return encodeFrame(MessageType::MetarResponse, body.data());
}

struct DecodedFrame {
  MessageType type{};
  const char* payload = nullptr;
  std::size_t payload_size = 0;
};

inline bool tryDecodeFrame(const char* data, std::size_t size, DecodedFrame& out,
                           std::size_t& consumed) {
  if (size < kFrameHeaderSize) return false;
  BufferReader r(data, size);
  const auto length = r.readU32();
  if (length < 2 || length > (1u << 20)) {
    throw std::runtime_error("invalid frame length");
  }
  if (size < 4 + length) return false;
  out.type = static_cast<MessageType>(r.readU16());
  out.payload = data + kFrameHeaderSize;
  out.payload_size = length - 2;
  consumed = 4 + length;
  return true;
}

}  // namespace osats
