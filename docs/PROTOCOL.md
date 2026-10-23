# OSATS Protocol v2

Binary, length-prefixed, little-endian. Inspired by the *roles* of openfsd/FSD packets (`$DI`, `#AP`, `@`/`^`, `$FP`, `#TM`, `$CQ`, `$AX`, …) but redesigned for 2026: typed frames, velocities, interest tiers, and HTTP observability.

## Framing

```
[u32 length][u16 type][payload...]
```

`length` = `sizeof(type) + payload` (excludes the length field).

## Connection flow

1. TCP connect
2. Server → `ServerAnnounce` (magic, proto, name, version) — analogous to FSD `$DI`
3. Client → `Hello` (magic, proto, role, callsign, real name) — `#AP` / `#AA`
4. Server → `HelloAck` (entity id, tick Hz, server name)
5. Client → `PositionUpdate` / `FlightPlanSet` / `TextMessage` / …
6. Server → `PeerUpdate` / `PeerRemove` / replies to nearby clients

## Message types

| Type | ID | Direction | Purpose |
| --- | --- | --- | --- |
| ServerAnnounce | 1 | S→C | Server identity |
| Hello | 2 | C→S | Login |
| HelloAck | 3 | S→C | Assigned entity id |
| Heartbeat | 4 | both | Keepalive / idle detection |
| Goodbye | 5 | both | Clean disconnect |
| Error | 6 | S→C | Structured error |
| PositionUpdate | 100 | C→S | Own state + velocities + squawk |
| PeerUpdate | 101 | S→C | Other traffic |
| PeerRemove | 102 | S→C | Left range / disconnected |
| FlightPlanSet | 200 | C→S | File / amend plan |
| FlightPlanUpdate | 201 | S→C | Plan visible to peers |
| TextMessage | 210 | both | Private / radio / broadcast / wallop |
| RadioTune | 211 | C→S | Active COM frequency |
| Ping / Pong | 220 / 221 | both | RTT probe |
| ClientQuery / Reply | 230 / 231 | both | INFO / FP / ATIS / SERVER |
| MetarRequest / Response | 240 / 241 | both | Weather |

## Position payload

Microdegree lat/lon, altitude ft, GS kt, heading/pitch/bank centidegrees, N/E/D velocity ft/s (`f32`), squawk, XPDR mode (`C`/`S`/`I`), then visibility nm (`u16`) on `PositionUpdate`.

`PeerUpdate` ends with the server receive time of that position, a little-endian `u64` in milliseconds. Clients coast `vn`/`ve`/`vd` from the sample between updates. The tier intervals below are ceilings.

## Interest management

Servers fan out `PeerUpdate` using a spatial hash grid and distance tiers:

| Tier | Range | Min interval |
| --- | --- | --- |
| Near | ≤ 10 nm | 200 ms (5 Hz) |
| Mid | ≤ 30 nm | 1 s |
| Far | ≤ visibility | 5 s |

This is the modern replacement for FSD’s coarse “send to everyone in range” loop, and is what lets a single logical world stay responsive without asking users to pick a shard.
