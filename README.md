# OSATS

**Open Source Air Traffic Server** — a 2026-ready, single-world C++ traffic platform for pilot, ATC, and observer clients.

OSATS takes the *job* of classic FSD networks ([openfsd](https://github.com/renorris/openfsd) / VATSIM) — shared multiplayer world, ranged position broadcast, flight plans, radio text, weather queries — and rebuilds it with a modern binary protocol, interest-managed fanout, and first-class HTTP observability. Clients connect to **one endpoint**. No server picker.

Legacy v1 sources live in [`legacy/`](legacy/). Protocol details: [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Why not just FSD?

[openfsd](https://github.com/renorris/openfsd) is an excellent open implementation of the VATSIM-style FSD protocol: plaintext line packets, post-office + R-tree spatial routing, flight plans, METAR, web admin. It’s the right reference for *what an air-traffic network must do*.

OSATS deliberately does **not** speak FSD. It keeps the product surface and replaces the 1990s wire format:

| Classic FSD (openfsd) | OSATS v2.1 |
| --- | --- |
| CSV/`$DI`/`#AP`/`@`/`^` text lines | Versioned length-prefixed binary frames |
| Manual / proxied FSD entry nodes | Single logical world, one TCP port |
| Coarse range rebroadcast | Spatial grid + 5 Hz / 1 Hz / 0.2 Hz tiers |
| Optional web admin | Built-in live map + JSON API |
| Auth/CERT complexity | Open demo auth hooks (Hello) |

VATSIM itself has already fixed a lot of the old UX (automatic server selection, Velocity 5 Hz). The remaining opportunity is an open, greenfield protocol that’s easy to extend without NDAs or CSV archaeology.

## Features

- **Pilot / ATC / observer** roles
- **Velocity-aware positions** (pitch/bank/heading + N/E/D ft/s, squawk)
- **Flight plans** with peer fanout
- **Radio tune + text** (private, frequency, broadcast, wallop)
- **Ping/pong**, **client queries** (INFO / FP / ATIS / SERVER), **METAR**
- **HTTP live map** at `:4401/` plus `/api/v1/traffic` and `/api/v1/status`
- **Interactive CLI** (`osats-cli`) and simple demo client
- **Docker** image + compose
- Idle timeout, backpressure, graceful SIGINT shutdown

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# terminal 1 — server + live map
./build/osats-server --port 4400 --http-port 4401 --verbose
# open http://127.0.0.1:4401/

# terminal 2 — interactive client
./build/osats-cli
osats> connect 127.0.0.1 4400 N17XX pilot "Ada Lovelace"
osats> pos 37.62 -122.38 0 0 90
osats> radio 122.8
osats> fp KSFO KLAX B738 I 35000 SSTIK3 FREBY RZS SADDE7
osats> move 120 180
osats> radio-say checking in on unicom
osats> metar KSFO
osats> peers
```

Or with Docker:

```bash
docker compose up --build
```

## Binaries

| Binary | Purpose |
| --- | --- |
| `osats-server` | Traffic server + HTTP map/API |
| `osats-cli` | Simple interactive pilot/ATC client |
| `osats-client` | Non-interactive demo loop |
| `osats-loadtest` | Concurrent client soak tool |
| `osats-traffic` | Scripted KSFO pilots on 122.800 (UNICOM) |

## Injected traffic

`osats-traffic` is a separate process. It opens one pilot session per aircraft, files a flight plan, flies a scripted path around KSFO, tunes **122.800**, and makes scheduled UNICOM position calls. It does not read peer traffic or answer radio calls. Real name on every session is `OSATS Traffic`.

```bash
# terminal 1
./build/osats-server --port 4400 --http-port 4401

# terminal 2 — one human pilot, no controller
./build/osats-cli
osats> connect 127.0.0.1 4400 N17XX pilot "Ada Lovelace"
osats> pos 37.619 -122.375 13 0 280
osats> radio 122.8
osats> plan UAL482

# terminal 3
./build/osats-traffic 127.0.0.1 4400
```

Stop that process (Ctrl-C, or kill it) and the pilot sees each aircraft leave. A short run is `osats-traffic 127.0.0.1 4400 30`. `test_traffic` covers connect, position fanout, plan query, and `PeerRemove`.

## HTTP API

| Path | Response |
| --- | --- |
| `GET /` | Live traffic map (auto-refresh) |
| `GET /health` | `ok` |
| `GET /api/v1/status` | Server counters JSON |
| `GET /api/v1/traffic` | All online entities JSON |

## Library sketch

```cpp
#include "osats/client.hpp"

osats::Client client;
client.onText([](const osats::TextEvent& ev){ /* radio */ });
client.connect("127.0.0.1", 4400, "UAL100", osats::Role::Pilot, "Crew");
client.tuneRadio(122800);
client.sendFlightPlan({.flight_rules='I', .aircraft_type="B738",
                       .departure="KSFO", .arrival="KLAX", .route="DCT"});
client.sendPosition({.latitude_deg=37.62, .longitude_deg=-122.38, .squawk=2201});
```

## Roadmap

- Token/JWT auth at Hello (openfsd-style CERT without copying FSD)
- Controller frequency ownership + handoff messages
- Optional UDP fast-path for Near-tier positions
- Horizontal shards with boundary ghosting (still one world to clients)
