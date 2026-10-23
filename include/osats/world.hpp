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

// Result of refreshing one aircraft's neighbor set.
struct ContactChange {
  EntityId other = kInvalidEntityId;
  bool entered = false;  // false means the pair left range
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
    unlinkVisibility(id);
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

  std::vector<ContactChange> updatePosition(EntityId id, const Position& pos,
                                            double visibility_nm,
                                            std::uint64_t now_ms) {
    auto* state = get(id);
    if (!state) return {};
    state->position = pos;
    state->visibility_nm = cappedVisibility(visibility_nm);
    state->last_update_ms = now_ms;
    index_.upsert(id, pos);
    ++stats_.position_updates;
    return syncContacts(id);
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

  // True when `observer` currently has `subject` in its visible set.
  bool watches(EntityId observer, EntityId subject) const {
    auto it = visible_.find(observer);
    return it != visible_.end() && it->second.count(subject) != 0;
  }

  // Drop every contact for `id` and return who was still watching. Callers
  // send PeerRemove to that list. Range exits are already gone.
  std::vector<EntityId> severContacts(EntityId id) {
    auto watchers = unlinkVisibility(id);
    eraseRateLimitsFor(id);
    return watchers;
  }

  void markPeerSent(EntityId observer, EntityId subject, std::uint64_t now_ms) {
    last_sent_ms_[rateKey(observer, subject)] = now_ms;
  }

  void forgetPeerRate(EntityId a, EntityId b) {
    last_sent_ms_.erase(rateKey(a, b));
    last_sent_ms_.erase(rateKey(b, a));
  }

  void forEachInterestedObserver(
      EntityId subject, std::uint64_t now_ms, bool ignore_rate_limit,
      const std::function<void(EntityId observer, geo::InterestTier tier)>& fn) {
    const auto* subj = get(subject);
    if (!subj) return;
    auto it = visible_.find(subject);
    if (it == visible_.end()) return;

    // Copy: the callback may refresh contacts and rehash the map.
    const std::vector<EntityId> watchers(it->second.begin(), it->second.end());
    for (EntityId observer : watchers) {
      if (observer == subject) continue;
      const auto* obs = get(observer);
      if (!obs) continue;

      const double vis = std::max(cappedVisibility(obs->visibility_nm),
                                  cappedVisibility(subj->visibility_nm));
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

  void notePeerUpdateSent() { ++stats_.peer_updates_sent; }
  void notePeerRemoveSent() { ++stats_.peer_removes_sent; }
  void noteTextSent() { ++stats_.texts_sent; }

  const std::unordered_map<EntityId, AircraftState>& entities() const {
    return entities_;
  }

 private:
  static double cappedVisibility(double visibility_nm) {
    return std::clamp(visibility_nm, 0.0, geo::kMaxVisibilityNm);
  }

  static std::uint64_t rateKey(EntityId observer, EntityId subject) {
    return (static_cast<std::uint64_t>(observer) << 32) |
           static_cast<std::uint64_t>(subject);
  }

  // Range is max(capped visibilities). The spatial query uses the cap so an
  // aircraft with a short bubble still finds one with a long bubble.
  std::vector<ContactChange> syncContacts(EntityId id) {
    std::vector<ContactChange> changes;
    const auto* self = get(id);
    if (!self) return changes;

    std::unordered_set<EntityId> now;
    candidates_.clear();
    index_.queryRadius(self->position, geo::kMaxVisibilityNm, candidates_);
    const double self_vis = cappedVisibility(self->visibility_nm);
    for (EntityId other_id : candidates_) {
      if (other_id == id) continue;
      const auto* other = get(other_id);
      if (!other) continue;
      const double range =
          std::max(self_vis, cappedVisibility(other->visibility_nm));
      if (geo::distanceNm(self->position, other->position) <= range) {
        now.insert(other_id);
      }
    }

    std::unordered_set<EntityId> prev;
    if (auto it = visible_.find(id); it != visible_.end()) prev = it->second;

    for (EntityId other_id : now) {
      if (prev.count(other_id) != 0) continue;
      visible_[id].insert(other_id);
      visible_[other_id].insert(id);
      changes.push_back(ContactChange{other_id, true});
    }
    for (EntityId other_id : prev) {
      if (now.count(other_id) != 0) continue;
      if (auto it = visible_.find(id); it != visible_.end()) {
        it->second.erase(other_id);
        if (it->second.empty()) visible_.erase(it);
      }
      if (auto oit = visible_.find(other_id); oit != visible_.end()) {
        oit->second.erase(id);
        if (oit->second.empty()) visible_.erase(oit);
      }
      changes.push_back(ContactChange{other_id, false});
    }
    return changes;
  }

  std::vector<EntityId> unlinkVisibility(EntityId id) {
    std::vector<EntityId> watchers;
    auto it = visible_.find(id);
    if (it == visible_.end()) return watchers;
    watchers.assign(it->second.begin(), it->second.end());
    visible_.erase(it);
    for (EntityId other : watchers) {
      auto oit = visible_.find(other);
      if (oit == visible_.end()) continue;
      oit->second.erase(id);
      if (oit->second.empty()) visible_.erase(oit);
    }
    return watchers;
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
  std::unordered_map<EntityId, std::unordered_set<EntityId>> visible_;
  std::unordered_map<std::uint64_t, std::uint64_t> last_sent_ms_;
  mutable std::vector<EntityId> candidates_;
  WorldStats stats_{};
};

}  // namespace osats
