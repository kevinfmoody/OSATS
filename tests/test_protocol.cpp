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
  auto hello = encodeHello("UAL100", Role::Pilot);
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

  Position p{37.6213, -122.3790, 12000, 450, 275.5};
  auto pos = encodePositionUpdate(p, 50);
  CHECK(tryDecodeFrame(pos.data(), pos.size(), frame, consumed));
  BufferReader pr(frame.payload, frame.payload_size);
  Position got = readPosition(pr);
  CHECK(std::abs(got.latitude_deg - p.latitude_deg) < 1e-5);
  CHECK(std::abs(got.longitude_deg - p.longitude_deg) < 1e-5);
  CHECK(std::abs(got.heading_deg - p.heading_deg) < 0.02);

  CHECK(!tryDecodeFrame(hello.data(), 3, frame, consumed));

  std::cout << "test_protocol ok\n";
  return 0;
}
