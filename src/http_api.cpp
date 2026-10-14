#include "osats/http_api.hpp"

#include "osats/types.hpp"

#include <poll.h>

#include <cstdio>
#include <cstring>
#include <sstream>

namespace osats {
namespace {

bool parseRequestLine(const std::string& req, std::string& method, std::string& path) {
  const auto end = req.find("\r\n");
  if (end == std::string::npos) return false;
  std::istringstream line(req.substr(0, end));
  std::string version;
  line >> method >> path >> version;
  return !method.empty() && !path.empty();
}

}  // namespace

std::string jsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

std::string buildTrafficJson(const World& world) {
  std::ostringstream oss;
  oss << "{\"server\":\"" << kServerName << "\",\"version\":\"" << kServerVersion
      << "\",\"protocol\":" << kProtocolVersion << ",\"traffic\":[";
  bool first = true;
  world.forEach([&](const AircraftState& ac) {
    if (!first) oss << ',';
    first = false;
    oss << '{'
        << "\"id\":" << ac.id << ','
        << "\"callsign\":\"" << jsonEscape(ac.callsign) << "\","
        << "\"name\":\"" << jsonEscape(ac.real_name) << "\","
        << "\"role\":\"" << roleName(ac.role) << "\","
        << "\"lat\":" << ac.position.latitude_deg << ','
        << "\"lon\":" << ac.position.longitude_deg << ','
        << "\"alt\":" << ac.position.altitude_ft << ','
        << "\"gs\":" << ac.position.groundspeed_kt << ','
        << "\"hdg\":" << ac.position.heading_deg << ','
        << "\"squawk\":" << ac.position.squawk << ','
        << "\"radio\":" << ac.radio_hz;
    if (ac.flight_plan) {
      oss << ",\"fp\":{"
          << "\"dep\":\"" << jsonEscape(ac.flight_plan->departure) << "\","
          << "\"arr\":\"" << jsonEscape(ac.flight_plan->arrival) << "\","
          << "\"ac\":\"" << jsonEscape(ac.flight_plan->aircraft_type) << "\""
          << '}';
    }
    oss << '}';
  });
  oss << "]}";
  return oss.str();
}

std::string buildStatusJson(const World& world, std::uint16_t port,
                            std::uint16_t http_port, std::size_t sessions) {
  const auto s = world.stats();
  std::ostringstream oss;
  oss << '{'
      << "\"name\":\"" << kServerName << "\","
      << "\"version\":\"" << kServerVersion << "\","
      << "\"protocol\":" << kProtocolVersion << ','
      << "\"port\":" << port << ','
      << "\"http_port\":" << http_port << ','
      << "\"sessions\":" << sessions << ','
      << "\"entities\":" << s.entities << ','
      << "\"position_updates\":" << s.position_updates << ','
      << "\"peer_updates_sent\":" << s.peer_updates_sent << ','
      << "\"texts_sent\":" << s.texts_sent << ','
      << "\"flight_plans\":" << s.flight_plans
      << '}';
  return oss.str();
}

std::string liveMapHtml() {
  return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>OSATS Live Traffic</title>
<style>
  :root { --bg:#0b1220; --panel:#121a2b; --ink:#e8eefc; --muted:#8fa0c1; --accent:#3dd6c6; --line:#24314d; }
  * { box-sizing: border-box; }
  body { margin:0; font-family: "IBM Plex Sans", "Segoe UI", sans-serif; background:radial-gradient(1200px 600px at 20% -10%, #1a2b4d, var(--bg)); color:var(--ink); }
  header { padding:1.1rem 1.4rem; display:flex; justify-content:space-between; align-items:baseline; border-bottom:1px solid var(--line); }
  h1 { margin:0; font-size:1.25rem; letter-spacing:.04em; }
  h1 span { color:var(--accent); }
  #meta { color:var(--muted); font-size:.9rem; }
  main { display:grid; grid-template-columns: 1.4fr .9fr; gap:0; height:calc(100vh - 64px); }
  #map { position:relative; background:linear-gradient(180deg,#0e1830,#0b1220); overflow:hidden; }
  canvas { width:100%; height:100%; display:block; }
  aside { border-left:1px solid var(--line); background:rgba(18,26,43,.92); padding:1rem; overflow:auto; }
  table { width:100%; border-collapse:collapse; font-size:.86rem; }
  th,td { text-align:left; padding:.45rem .35rem; border-bottom:1px solid var(--line); }
  th { color:var(--muted); font-weight:600; }
  .tag { color:var(--accent); font-weight:600; }
  @media (max-width:900px){ main{grid-template-columns:1fr;} aside{border-left:0;border-top:1px solid var(--line);} }
</style>
</head>
<body>
<header>
  <h1><span>OSATS</span> Live Traffic</h1>
  <div id="meta">connecting…</div>
</header>
<main>
  <section id="map"><canvas id="c"></canvas></section>
  <aside>
    <table>
      <thead><tr><th>Callsign</th><th>Role</th><th>Alt</th><th>GS</th><th>Route</th></tr></thead>
      <tbody id="rows"></tbody>
    </table>
  </aside>
</main>
<script>
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');
const rows = document.getElementById('rows');
const meta = document.getElementById('meta');
function resize(){ canvas.width = canvas.clientWidth * devicePixelRatio; canvas.height = canvas.clientHeight * devicePixelRatio; ctx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0); }
addEventListener('resize', resize); resize();

function project(lat, lon, bounds, w, h){
  const x = (lon - bounds.minLon) / (bounds.maxLon - bounds.minLon || 1) * w;
  const y = (bounds.maxLat - lat) / (bounds.maxLat - bounds.minLat || 1) * h;
  return [x,y];
}

async function tick(){
  try {
    const res = await fetch('/api/v1/traffic');
    const data = await res.json();
    const traffic = data.traffic || [];
    meta.textContent = traffic.length + ' online · protocol v' + data.protocol;
    rows.innerHTML = traffic.map(t => {
      const route = t.fp ? (t.fp.dep + '→' + t.fp.arr) : '—';
      return `<tr><td class="tag">${t.callsign}</td><td>${t.role}</td><td>${Math.round(t.alt)}</td><td>${Math.round(t.gs)}</td><td>${route}</td></tr>`;
    }).join('');

    const w = canvas.clientWidth, h = canvas.clientHeight;
    ctx.clearRect(0,0,w,h);
    ctx.fillStyle = '#10203a';
    for (let i=0;i<24;i++){ ctx.fillRect((i*97)%w, (i*53)%h, 2, 2); }

    if (!traffic.length) return;
    let minLat=90,maxLat=-90,minLon=180,maxLon=-180;
    for (const t of traffic){ minLat=Math.min(minLat,t.lat); maxLat=Math.max(maxLat,t.lat); minLon=Math.min(minLon,t.lon); maxLon=Math.max(maxLon,t.lon); }
    const pad = 0.35;
    const bounds = { minLat:minLat-pad, maxLat:maxLat+pad, minLon:minLon-pad, maxLon:maxLon+pad };
    for (const t of traffic){
      const [x,y] = project(t.lat, t.lon, bounds, w, h);
      ctx.save();
      ctx.translate(x,y);
      ctx.rotate(t.hdg * Math.PI/180);
      ctx.fillStyle = t.role === 'controller' ? '#f0c674' : '#3dd6c6';
      ctx.beginPath(); ctx.moveTo(0,-8); ctx.lineTo(6,8); ctx.lineTo(0,4); ctx.lineTo(-6,8); ctx.closePath(); ctx.fill();
      ctx.restore();
      ctx.fillStyle = '#e8eefc';
      ctx.font = '12px IBM Plex Sans, sans-serif';
      ctx.fillText(t.callsign, x+10, y-6);
    }
  } catch (e) { meta.textContent = 'offline'; }
}
tick(); setInterval(tick, 1000);
</script>
</body>
</html>)HTML";
}

HttpApi::HttpApi(std::uint16_t port, SnapshotFn traffic, StatsFn status)
    : traffic_(std::move(traffic)), status_(std::move(status)) {
  listener_ = net::Socket::listenTcp(port);
}

void HttpApi::acceptNew() {
  while (true) {
    auto sock = listener_.accept();
    if (!sock.valid()) break;
    auto c = std::make_unique<Conn>();
    const int fd = sock.fd();
    c->sock = std::move(sock);
    conns_.emplace(fd, std::move(c));
  }
}

void HttpApi::collectPollFds(std::vector<pollfd>& pfds) const {
  for (const auto& [fd, c] : conns_) {
    short events = POLLIN;
    if (!c->out.empty()) events |= POLLOUT;
    pfds.push_back(pollfd{fd, events, 0});
  }
}

void HttpApi::onReadable(int fd) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  Conn& c = *it->second;
  char buf[4096];
  while (true) {
    const int n = c.sock.recvSome(buf, sizeof(buf));
    if (n == 0) { drop(fd); return; }
    if (n < 0) break;
    c.in.append(buf, buf + n);
    if (c.in.size() > (1u << 20)) { drop(fd); return; }
  }
  if (c.in.find("\r\n\r\n") != std::string::npos) {
    handleRequest(c, c.in);
    c.in.clear();
  }
}

void HttpApi::onWritable(int fd) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  Conn& c = *it->second;
  while (c.out_off < c.out.size()) {
    const int n = c.sock.sendSome(c.out.data() + c.out_off, c.out.size() - c.out_off);
    if (n < 0) return;
    c.out_off += static_cast<std::size_t>(n);
  }
  if (c.closing) drop(fd);
  else {
    c.out.clear();
    c.out_off = 0;
  }
}

void HttpApi::drop(int fd) {
  conns_.erase(fd);
}

void HttpApi::handleRequest(Conn& c, const std::string& req) {
  std::string method, path;
  if (!parseRequestLine(req, method, path)) {
    c.out = httpResponse(400, "text/plain", "bad request");
    c.closing = true;
    return;
  }
  if (method != "GET") {
    c.out = httpResponse(405, "text/plain", "method not allowed");
    c.closing = true;
    return;
  }

  if (path == "/" || path == "/index.html") {
    c.out = httpResponse(200, "text/html; charset=utf-8", liveMapHtml());
  } else if (path == "/health") {
    c.out = httpResponse(200, "text/plain", "ok\n");
  } else if (path == "/api/v1/status") {
    c.out = httpResponse(200, "application/json", status_());
  } else if (path == "/api/v1/traffic") {
    c.out = httpResponse(200, "application/json", traffic_());
  } else {
    c.out = httpResponse(404, "text/plain", "not found");
  }
  c.closing = true;
  c.out_off = 0;
}

std::string HttpApi::httpResponse(int code, const char* content_type,
                                  const std::string& body) {
  const char* reason = code == 200 ? "OK" : code == 404 ? "Not Found" : "Error";
  std::ostringstream oss;
  oss << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "\r\n"
      << body;
  return oss.str();
}

}  // namespace osats
