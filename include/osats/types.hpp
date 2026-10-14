#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace osats {

inline constexpr std::uint32_t kProtocolMagic = 0x4F534154u;  // 'OSAT'
inline constexpr std::uint16_t kProtocolVersion = 2;
inline constexpr std::uint16_t kDefaultListenPort = 4400;
inline constexpr std::uint16_t kDefaultHttpPort = 4401;
inline constexpr const char* kServerName = "OSATS";
inline constexpr const char* kServerVersion = "2.1.0";

enum class Role : std::uint8_t {
  Pilot = 1,
  Observer = 2,
  Controller = 3,
};

enum class MessageType : std::uint16_t {
  // Core session (1-99)
  ServerAnnounce = 1,  // server -> client on accept (like FSD $DI)
  Hello = 2,
  HelloAck = 3,
  Heartbeat = 4,
  Goodbye = 5,
  Error = 6,

  // Traffic (100-199)
  PositionUpdate = 100,
  PeerUpdate = 101,
  PeerRemove = 102,

  // Flight planning / messaging (200-299) — inspired by openfsd $FP/#TM/$CQ
  FlightPlanSet = 200,
  FlightPlanUpdate = 201,
  TextMessage = 210,
  RadioTune = 211,
  Ping = 220,
  Pong = 221,
  ClientQuery = 230,
  ClientQueryReply = 231,
  MetarRequest = 240,
  MetarResponse = 241,

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
  NotFound = 7,
};

enum class TextScope : std::uint8_t {
  Private = 1,     // to one callsign
  Radio = 2,       // same frequency, in range
  Broadcast = 3,   // all connected (ops/admin style)
  Wallop = 4,      // to controllers
};

enum class QueryType : std::uint8_t {
  Info = 1,        // real name / role / location summary
  FlightPlan = 2,  // filed FP for target
  ATIS = 3,        // simple ATIS string for airport
  Server = 4,      // server info
};

using EntityId = std::uint32_t;
inline constexpr EntityId kInvalidEntityId = 0;

struct Position {
  double latitude_deg = 0.0;
  double longitude_deg = 0.0;
  double altitude_ft = 0.0;
  double groundspeed_kt = 0.0;
  double heading_deg = 0.0;
  double pitch_deg = 0.0;
  double bank_deg = 0.0;
  // Velocity (ft/s) in local NED-ish axes for smooth client extrapolation.
  float vn_fps = 0.f;
  float ve_fps = 0.f;
  float vd_fps = 0.f;
  std::uint16_t squawk = 1200;
  char xpdr_mode = 'C';  // C / S / I (ident)
};

struct FlightPlan {
  char flight_rules = 'I';  // I / V
  std::string aircraft_type;
  std::string departure;
  std::string arrival;
  std::string alternate;
  std::uint16_t cruise_alt_ft = 0;
  std::uint16_t tas_kt = 0;
  std::string route;
  std::string remarks;
};

struct AircraftState {
  EntityId id = kInvalidEntityId;
  std::string callsign;
  std::string real_name;
  Role role = Role::Pilot;
  Position position;
  double visibility_nm = 50.0;
  std::uint32_t radio_hz = 122800;  // 122.800 MHz * 1000
  std::optional<FlightPlan> flight_plan;
  std::uint64_t last_update_ms = 0;
  std::uint64_t connected_ms = 0;
};

inline const char* roleName(Role r) {
  switch (r) {
    case Role::Pilot: return "pilot";
    case Role::Observer: return "observer";
    case Role::Controller: return "controller";
  }
  return "unknown";
}

}  // namespace osats
