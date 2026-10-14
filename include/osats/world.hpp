#pragma once

#include "osats/spatial_index.hpp"
#include "osats/types.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osats {

struct WorldStats {
  std::uint64_t entities = 0;
  std::uint64_t position_updates = 0;
  std::uint64_t peer_updates_sent = 0;
  std::uint64_t peer_removes_sent = 0;
  std::uint64_t texts_sent = 0;
  std::uint64_t flight_plans = 0;
};

class World {
 public:
  EntityId allocateId() { return next_id_++; }

  bool callsignTaken(const std::string& callsign) const {
    return callsigns_.count(callsign) != 0;
  }

  AircraftState* findByCallsign(const std::string& callsign) {
    auto it = by_callsign_.find(callsign);
    return it == by_callsign_.end() ? nullptr : get(it->second);
  }

  const AircraftState* findByCallsign(const std::string& callsign) const {
    auto it = by_callsign_.find(callsign);
    return it == by_callsign_.end() ? nullptr : get(it->second);
  }

  AircraftState& spawn(EntityId id, std::string callsign, Role role,
                       std::string real_name, std::uint64_t now_ms) {
    AircraftState state;
    state.id = id;
    state.callsign = callsign;
    state.real_name = std::move(real_name);
    state.role = role;
    state.connected_ms = now_ms;
    callsigns_.insert(callsign);
    by_callsign_[callsign] = id;
    auto [it, _] = entities_.emplace(id, std::move(state));
    return it->second;
  }

  void despawn(EntityId id) {
    auto it = entities_.find(id);
    if (it == entities_.end()) return;
    callsigns_.erase(it->second.callsign);
    by_callsign_.erase(it->second.callsign);
    index_.remove(id);
    eraseRateLimitsFor(id);
    entities_.erase(it);
  }

  AircraftState* get(EntityId id) {
    auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
  }

  const AircraftState* get(EntityId id) const {
    auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
  }

  void updatePosition(EntityId id, const Position& pos, double visibility_nm,
                      std::uint64_t now_ms) {
    auto* state = get(id);
    if (!state) return;
    state->position = pos;
    state->visibility_nm = visibility_nm;
    state->last_update_ms = now_ms;
    index_.upsert(id, pos);
    ++stats_.position_updates;
  }

  void setFlightPlan(EntityId id, FlightPlan fp) {
    auto* state = get(id);
    if (!state) return;
    state->flight_plan = std::move(fp);
    ++stats_.flight_plans;
  }

  void setRadio(EntityId id, std::uint32_t hz) {
    if (auto* state = get(id)) state->radio_hz = hz;
  }

  void forEachInterestedObserver(
      EntityId subject, std::uint64_t now_ms, bool ignore_rate_limit,
      const std::function<void(EntityId observer, geo::InterestTier tier)>& fn) {
    const auto* subj = get(subject);
    if (!subj) return;

    candidates_.clear();
    index_.queryRadius(subj->position, subj->visibility_nm, candidates_);

    for (EntityId observer : candidates_) {
      if (observer == subject) continue;
      const auto* obs = get(observer);
      if (!obs) continue;

      const double vis = std::max(obs->visibility_nm, subj->visibility_nm);
      const double dist = geo::distanceNm(obs->position, subj->position);
      const auto tier = geo::classifyTier(dist, vis);
      if (tier == geo::InterestTier::None) continue;

      if (!ignore_rate_limit) {
        const auto key = rateKey(observer, subject);
        const auto min_ms = geo::minIntervalMs(tier);
        auto& last = last_sent_ms_[key];
        if (last != 0 && now_ms - last < min_ms) continue;
        last = now_ms;
      }
      fn(observer, tier);
      ++stats_.peer_updates_sent;
    }
  }

  void forEachVisibleTo(
      EntityId observer,
      const std::function<void(const AircraftState& peer)>& fn) const {
    const auto* obs = get(observer);
    if (!obs) return;
    std::vector<EntityId> ids;
    index_.queryRadius(obs->position, obs->visibility_nm, ids);
    for (EntityId id : ids) {
      if (id == observer) continue;
      const auto* peer = get(id);
      if (!peer) continue;
      if (geo::distanceNm(obs->position, peer->position) <= obs->visibility_nm) {
        fn(*peer);
      }
    }
  }

  void forEachOnFrequency(
      std::uint32_t hz, EntityId except,
      const std::function<void(const AircraftState&)>& fn) const {
    for (const auto& [id, ac] : entities_) {
      (void)id;
      if (ac.id == except) continue;
      if (ac.radio_hz == hz) fn(ac);
    }
  }

  void forEachController(const std::function<void(const AircraftState&)>& fn) const {
    for (const auto& [id, ac] : entities_) {
      (void)id;
      if (ac.role == Role::Controller) fn(ac);
    }
  }

  void forEach(const std::function<void(const AircraftState&)>& fn) const {
    for (const auto& [id, ac] : entities_) {
      (void)id;
      fn(ac);
    }
  }

  WorldStats stats() const {
    WorldStats s = stats_;
    s.entities = entities_.size();
    return s;
  }

  void notePeerRemoveSent() { ++stats_.peer_removes_sent; }
  void noteTextSent() { ++stats_.texts_sent; }

  const std::unordered_map<EntityId, AircraftState>& entities() const {
    return entities_;
  }

 private:
  static std::uint64_t rateKey(EntityId observer, EntityId subject) {
    return (static_cast<std::uint64_t>(observer) << 32) |
           static_cast<std::uint64_t>(subject);
  }

  void eraseRateLimitsFor(EntityId id) {
    for (auto it = last_sent_ms_.begin(); it != last_sent_ms_.end();) {
      const EntityId observer = static_cast<EntityId>(it->first >> 32);
      const EntityId subject = static_cast<EntityId>(it->first & 0xffffffffu);
      if (observer == id || subject == id) {
        it = last_sent_ms_.erase(it);
      } else {
        ++it;
      }
    }
  }

  EntityId next_id_ = 1;
  std::unordered_map<EntityId, AircraftState> entities_;
  std::unordered_map<std::string, EntityId> by_callsign_;
  std::unordered_set<std::string> callsigns_;
  SpatialIndex index_;
  std::unordered_map<std::uint64_t, std::uint64_t> last_sent_ms_;
  mutable std::vector<EntityId> candidates_;
  WorldStats stats_{};
};

}  // namespace osats
