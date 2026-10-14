# OSATS

**Open Source Air Traffic Server** — a modern, single-world C++ traffic server for pilot and radar clients.

OSATS v2 keeps the original project's goal (realistic multiplayer aircraft traffic for third-party clients) but replaces the early prototype with a portable, interest-managed design meant to avoid the classic “pick a server / melts when busy” failure modes of early-2000s FSD-style networks.

## VATSIM context (research notes)

Two common historical complaints about VATSIM:

1. **You had to choose which FSD server to connect to**
2. **Busy airspace felt slow / choppy**

### What changed

| Issue | Status today | Notes |
| --- | --- | --- |
| Manual server selection | **Mostly fixed (UX)** | Since ~2020 VATSIM routes clients automatically via proximity DNS/HTTP (`fsd.connect.vatsim.net` / [vatdns](https://github.com/vatsimnetwork/vatdns)). Modern clients (e.g. vPilot) expose **AUTOMATIC** rather than a server picker. Regional FSD entry points still exist under the hood; users rarely choose them. |
| Slow updates when crowded | **Partially fixed** | [VATSIM Velocity](https://fselite.net/content/vatsim-velocity-now-available-for-everyone/) (2022) raised nearby position updates from **0.2 Hz → 5 Hz**. Remaining “busy airport is slow” reports are often **client/sim rendering** (injecting many AI models), not the network failing to deliver packets. |

FSD itself is still a late-90s protocol lineage (CSV-ish packets, central message forwarder). VATSIM modernized operations around it; it did not become a greenfield design. That gap is what OSATS v2 targets as an open alternative architecture.

## Design goals (v2)

- **Single logical world** — clients connect to one endpoint; no user-facing server picker
- **Interest management** — spatial hash grid + distance-tiered send rates (5 Hz / 1 Hz / 0.2 Hz)
- **Robust sessions** — length-prefixed frames, heartbeats, backpressure, non-fatal I/O errors
- **Portable** — POSIX `poll()` (Linux/macOS), CMake, C++20
- **Extensible** — versioned binary protocol, reserved message-type ranges for plugins

Legacy v1 sources are preserved under [`legacy/`](legacy/).

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# terminal 1
./build/osats-server --port 4400 --verbose

# terminal 2 / 3
./build/osats-client 127.0.0.1 4400 N12345
./build/osats-client 127.0.0.1 4400 N67890

# optional load smoke test
./build/osats-loadtest 127.0.0.1 4400 50 10
```

## Protocol (v1)

Frames are little-endian:

```
[u32 length][u16 type][payload...]
```

`length` covers `type + payload` (not the length field itself).

| Type | ID | Purpose |
| --- | --- | --- |
| Hello | 1 | magic, version, role, callsign |
| HelloAck | 2 | magic, version, entity id, tick Hz |
| Heartbeat | 3 | keepalive |
| Goodbye | 4 | clean disconnect |
| Error | 5 | machine code + message |
| PositionUpdate | 100 | lat/lon/alt/gs/hdg + visibility |
| PeerUpdate | 101 | other traffic state |
| PeerRemove | 102 | traffic left range / disconnected |
| ExtensionBase | 1000+ | reserved |

Coordinates use microdegrees on the wire. Roles today: Pilot, Observer, Controller (controller-specific features can extend without breaking pilots).

## Architecture

```
Client ──TCP──► Session (framed I/O)
                    │
                    ▼
                 World ── SpatialIndex (grid)
                    │
                    ▼
          distance-tier rate limiter
                    │
                    ▼
          PeerUpdate / PeerRemove to observers
```

Broadcast cost scales with **local density**, not global population: each position update queries nearby grid cells and skips sends that are still inside their tier interval.

## Library usage

```cpp
#include "osats/client.hpp"

osats::Client client;
client.onPeerUpdate([](const osats::Peer& p) { /* render traffic */ });
client.connect("traffic.example.net", 4400, "UAL100");
client.sendPosition({37.62, -122.38, 0, 0, 90});
```

```cpp
#include "osats/server.hpp"

osats::Server server({.port = 4400, .verbose = true});
server.run();  // single-world event loop
```

## Roadmap hooks

The current node is intentionally single-process. The protocol/world split leaves room for:

- auth / CERT integration at Hello
- controller frequency / text message extensions (`MessageType >= 1000`)
- horizontal shards with boundary ghosting (still one logical world to clients)
- UDP fast-path for near-tier position updates
