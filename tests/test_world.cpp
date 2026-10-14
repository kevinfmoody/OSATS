#include "osats/world.hpp"

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
  world.spawn(a, "AAA", Role::Pilot);
  world.spawn(b, "BBB", Role::Pilot);
  CHECK(world.callsignTaken("AAA"));

  Position pa{37.0, -122.0, 0, 0, 0};
  Position pb{37.01, -122.01, 0, 0, 0};
  world.updatePosition(a, pa, 50, 1000);
  world.updatePosition(b, pb, 50, 1000);

  std::set<EntityId> observers;
  world.forEachInterestedObserver(a, 2000, [&](EntityId obs, geo::InterestTier) {
    observers.insert(obs);
  });
  CHECK(observers.count(b) == 1);

  observers.clear();
  world.forEachInterestedObserver(a, 2100, [&](EntityId obs, geo::InterestTier) {
    observers.insert(obs);
  });
  CHECK(observers.empty());

  observers.clear();
  world.forEachInterestedObserver(a, 2300, [&](EntityId obs, geo::InterestTier) {
    observers.insert(obs);
  });
  CHECK(observers.count(b) == 1);

  world.despawn(a);
  CHECK(!world.callsignTaken("AAA"));
  CHECK(world.get(a) == nullptr);

  std::cout << "test_world ok\n";
  return 0;
}
