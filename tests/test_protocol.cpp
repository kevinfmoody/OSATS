#include "osats/protocol.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr " (" << __FILE__ << ":" << __LINE__\
                << ")\n";                                                    \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

int main() {
  using namespace osats;
  auto hello = encodeHello("UAL100", Role::Pilot, "Test Pilot");
  DecodedFrame frame;
  std::size_t consumed = 0;
  CHECK(tryDecodeFrame(hello.data(), hello.size(), frame, consumed));
  CHECK(frame.type == MessageType::Hello);
  CHECK(consumed == hello.size());

  BufferReader r(frame.payload, frame.payload_size);
  CHECK(r.readU32() == kProtocolMagic);
  CHECK(r.readU16() == kProtocolVersion);
  CHECK(r.readU8() == static_cast<std::uint8_t>(Role::Pilot));
  CHECK(r.readString() == "UAL100");
  CHECK(r.readString() == "Test Pilot");

  Position p{37.6213, -122.3790, 12000, 450, 275.5};
  p.pitch_deg = 2.5;
  p.bank_deg = -5.0;
  p.vn_fps = 10.f;
  p.ve_fps = 20.f;
  p.squawk = 2201;
  p.xpdr_mode = 'C';
  auto pos = encodePositionUpdate(p, 50);
  CHECK(tryDecodeFrame(pos.data(), pos.size(), frame, consumed));
  BufferReader pr(frame.payload, frame.payload_size);
  Position got = readPosition(pr);
  CHECK(std::abs(got.latitude_deg - p.latitude_deg) < 1e-5);
  CHECK(std::abs(got.longitude_deg - p.longitude_deg) < 1e-5);
  CHECK(std::abs(got.heading_deg - p.heading_deg) < 0.02);
  CHECK(got.squawk == 2201);

  FlightPlan fp;
  fp.flight_rules = 'I';
  fp.aircraft_type = "B738";
  fp.departure = "KSFO";
  fp.arrival = "KLAX";
  fp.route = "SSTIK3 FREBY";
  auto fp_frame = encodeFlightPlanSet(fp);
  CHECK(tryDecodeFrame(fp_frame.data(), fp_frame.size(), frame, consumed));
  BufferReader fr(frame.payload, frame.payload_size);
  FlightPlan got_fp = readFlightPlan(fr);
  CHECK(got_fp.departure == "KSFO");
  CHECK(got_fp.route == "SSTIK3 FREBY");

  auto text = encodeTextMessage(TextScope::Radio, "N1", "122.800", "hello");
  CHECK(tryDecodeFrame(text.data(), text.size(), frame, consumed));
  CHECK(frame.type == MessageType::TextMessage);

  CHECK(!tryDecodeFrame(hello.data(), 3, frame, consumed));

  std::cout << "test_protocol ok\n";
  return 0;
}
