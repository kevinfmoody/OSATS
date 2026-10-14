#pragma once

#include <cstdint>
#include <string>

namespace osats {

inline constexpr std::uint32_t kProtocolMagic = 0x4F534154u; // 'OSAT'
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint16_t kDefaultListenPort = 4400;

enum class Role : std::uint8_t {
  Pilot = 1,
  Observer = 2,
  Controller = 3,
};

enum class MessageType : std::uint16_t {
  // Core session (1-99)
  Hello = 1,
  HelloAck = 2,
  Heartbeat = 3,
  Goodbye = 4,
  Error = 5,

  // Traffic (100-199)
  PositionUpdate = 100,
  PeerUpdate = 101,
  PeerRemove = 102,

  // Reserved for extensions (1000+)
  ExtensionBase = 1000,
};

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  UnsupportedVersion = 1,
  InvalidFrame = 2,
  ProtocolViolation = 3,
  CallsignTaken = 4,
  RateLimited = 5,
  Internal = 6,
};

using EntityId = std::uint32_t;
inline constexpr EntityId kInvalidEntityId = 0;

struct Position {
  double latitude_deg = 0.0;
  double longitude_deg = 0.0;
  double altitude_ft = 0.0;
  double groundspeed_kt = 0.0;
  double heading_deg = 0.0;
};

struct AircraftState {
  EntityId id = kInvalidEntityId;
  std::string callsign;
  Role role = Role::Pilot;
  Position position;
  double visibility_nm = 50.0;
  std::uint64_t last_update_ms = 0;
};

}  // namespace osats
