#include "osats/world.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr "\n";                              \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

int main() {
  using namespace osats;
  World world;
  const auto a = world.allocateId();
  const auto b = world.allocateId();
  world.spawn(a, "AAA", Role::Pilot, "Alice", 0);
  world.spawn(b, "BBB", Role::Controller, "Bob", 0);
  CHECK(world.callsignTaken("AAA"));
  CHECK(world.findByCallsign("BBB")->role == Role::Controller);

  Position pa{37.0, -122.0, 0, 0, 0};
  Position pb{37.01, -122.01, 0, 0, 0};
  world.updatePosition(a, pa, 50, 1000);
  world.updatePosition(b, pb, 50, 1000);

  FlightPlan fp;
  fp.departure = "KSFO";
  fp.arrival = "KLAX";
  world.setFlightPlan(a, fp);
  CHECK(world.get(a)->flight_plan->arrival == "KLAX");

  world.setRadio(a, 122800);
  world.setRadio(b, 122800);
  int on_freq = 0;
  world.forEachOnFrequency(122800, a, [&](const AircraftState&) { ++on_freq; });
  CHECK(on_freq == 1);

  std::set<EntityId> observers;
  world.forEachInterestedObserver(a, 2000, false, [&](EntityId obs, geo::InterestTier) {
    observers.insert(obs);
  });
  CHECK(observers.count(b) == 1);

  observers.clear();
  world.forEachInterestedObserver(a, 2100, false, [&](EntityId obs, geo::InterestTier) {
    observers.insert(obs);
  });
  CHECK(observers.empty());

  observers.clear();
  world.forEachInterestedObserver(a, 2100, true, [&](EntityId obs, geo::InterestTier) {
    observers.insert(obs);
  });
  CHECK(observers.count(b) == 1);

  // The larger bubble wins. A's own 10 nm range would miss B at 25 nm.
  {
    const auto s = world.allocateId();
    const auto t = world.allocateId();
    world.spawn(s, "SSS", Role::Pilot, "Short", 0);
    world.spawn(t, "TTT", Role::Pilot, "Tall", 0);
    Position ps{0, 0, 0, 0, 0};
    Position pt{0, 25.0 / 60.0, 0, 0, 0};
    world.updatePosition(t, pt, 40, 3000);
    const auto changes = world.updatePosition(s, ps, 10, 3000);
    bool entered = false;
    for (const auto& ch : changes) {
      if (ch.other == t && ch.entered) entered = true;
    }
    CHECK(entered);
    CHECK(world.watches(s, t));
    CHECK(world.watches(t, s));
  }

  // Visibility above the server cap cannot pull in traffic past the cap.
  // Flying out drops the contact, so a later disconnect has nobody left.
  {
    const auto s = world.allocateId();
    const auto near = world.allocateId();
    const auto far_id = world.allocateId();
    world.spawn(s, "CAP", Role::Pilot, "Cap", 0);
    world.spawn(near, "INN", Role::Pilot, "In", 0);
    world.spawn(far_id, "OUT", Role::Pilot, "Out", 0);
    const double cap = geo::kPilotVisibilityNm;
    const Position origin{10, 0, 0, 0, 0};
    const Position inside{10 + (cap - 10.0) / 60.0, 0, 0, 0, 0};
    const Position outside{10 + (cap + 20.0) / 60.0, 0, 0, 0, 0};
    world.updatePosition(far_id, outside, 100000, 4000);
    world.updatePosition(near, inside, 100000, 4000);
    const auto changes = world.updatePosition(s, origin, 100000, 4000);
    CHECK(world.get(s) != nullptr);
    CHECK(std::abs(world.get(s)->visibility_nm - cap) < 1e-9);
    bool saw_near = false;
    bool saw_far = false;
    for (const auto& ch : changes) {
      if (ch.other == near && ch.entered) saw_near = true;
      if (ch.other == far_id && ch.entered) saw_far = true;
    }
    CHECK(saw_near);
    CHECK(!saw_far);
    CHECK(world.watches(s, near));
    CHECK(!world.watches(s, far_id));

    const Position flown{0, 5, 0, 0, 0};
    const auto left = world.updatePosition(near, flown, 50, 5000);
    bool exited = false;
    for (const auto& ch : left) {
      if (ch.other == s && !ch.entered) exited = true;
    }
    CHECK(exited);
    CHECK(!world.watches(s, near));
    CHECK(!world.watches(near, s));
    CHECK(world.severContacts(near).empty());
  }

  // A controller may look past the pilot cap. Two pilots at 80 nm stay apart.
  {
    const auto traffic = world.allocateId();
    const auto pilot = world.allocateId();
    const auto atc = world.allocateId();
    world.spawn(traffic, "TRF", Role::Pilot, "Traffic", 0);
    world.spawn(pilot, "PLT", Role::Pilot, "Pilot", 0);
    world.spawn(atc, "ATC", Role::Controller, "Center", 0);
    const Position origin{20, 0, 0, 0, 0};
    const Position out80{20 + 80.0 / 60.0, 0, 0, 0, 0};
    world.updatePosition(traffic, out80, 1000, 6000);
    const auto pilot_changes = world.updatePosition(pilot, origin, 1000, 6000);
    CHECK(world.get(pilot) != nullptr);
    CHECK(std::abs(world.get(pilot)->visibility_nm - geo::kPilotVisibilityNm) < 1e-9);
    bool pilot_saw = false;
    for (const auto& ch : pilot_changes) {
      if (ch.other == traffic && ch.entered) pilot_saw = true;
    }
    CHECK(!pilot_saw);

    const auto atc_changes = world.updatePosition(atc, origin, 1000, 6000);
    CHECK(world.get(atc) != nullptr);
    CHECK(std::abs(world.get(atc)->visibility_nm - geo::kControllerVisibilityNm) < 1e-9);
    bool atc_saw = false;
    for (const auto& ch : atc_changes) {
      if (ch.other == traffic && ch.entered) atc_saw = true;
    }
    CHECK(atc_saw);
    CHECK(world.watches(atc, traffic));
    CHECK(world.watches(traffic, atc));
  }

  world.despawn(a);
  CHECK(!world.callsignTaken("AAA"));
  CHECK(world.get(a) == nullptr);

  std::cout << "test_world ok\n";
  return 0;
}
