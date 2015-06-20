#include "store.h"
#include "compressible.h"
#include "message.h"
#include "geo.h"

namespace OSATS {
  void Store::put(int socket, Compressible *compressible) {
    StoreItem *storeItem;
    auto result = this->store.find(socket);
    if (result == this->store.end()) {
      storeItem = new StoreItem();
      storeItem->socket = socket;
      this->store[socket] = storeItem;
    } else {
      storeItem = result->second;
      this->removePosition(storeItem);
      delete storeItem->compressible;
    }
    storeItem->compressible = compressible;
    Position &pos = compressible->getPosition();
    this->latitudeStore.insert(std::pair<double, int>
                               (pos.getLatitude(), socket));
    this->longitudeStore.insert(std::pair<double, int>
                                (pos.getLongitude(), socket));
    this->updateNeighbors(storeItem);
  }

  void Store::remove(int socket) {
    auto result = this->store.find(socket);
    if (result == this->store.end()) {
      return;
    }
    StoreItem *storeItem = result->second;
    this->removePosition(storeItem);
    delete storeItem->compressible;
    delete storeItem;
    this->store.erase(result);
  }

  void Store::removePosition(StoreItem *storeItem) {
    Position &resultPos = storeItem->compressible->getPosition();
    double lat = resultPos.getLatitude();
    auto latRange = this->latitudeStore.equal_range(lat);
    for (auto it = latRange.first; it != latRange.second; ++it) {
      if (it->second == storeItem->socket) {
        this->latitudeStore.erase(it);
        break;
      }
    }
    double lon = resultPos.getLongitude();
    auto lonRange = this->longitudeStore.equal_range(lon);
    for (auto it = lonRange.first; it != lonRange.second; ++it) {
      if (it->second == storeItem->socket) {
        this->longitudeStore.erase(it);
        break;
      }
    }
  }

  std::unordered_set<int> &Store::getNeighbors(int socket) {
    return this->store[socket]->neighbors;
  }

  void Store::updateNeighbors(StoreItem *storeItem) {
    Compressible *compressible = storeItem->compressible;
    Position &pos = compressible->getPosition();
    double lat = pos.getLatitude();
    double lon = pos.getLongitude();
    double visibilityRange = compressible->getVisibilityRange();
    double minLat, maxLat, minLon, maxLon;
    Geo::bounds(pos, visibilityRange,
                minLat, maxLat, minLon, maxLon);
    if (minLon < maxLon) {
      auto minLonIt = this->longitudeStore.lower_bound(minLon);
      auto maxLonIt = this->longitudeStore.upper_bound(maxLon);
      for (auto it = minLonIt; it != maxLonIt; ++it) {
        this->addNeighbor(storeItem, it->second, visibilityRange);
      }
    } else {
      auto lowerLonIt = this->longitudeStore.upper_bound(maxLon);
      for (auto it = this->longitudeStore.begin(); it != lowerLonIt; ++it) {
        this->addNeighbor(storeItem, it->second, visibilityRange);
      }
      auto upperLonIt = this->longitudeStore.lower_bound(minLon);
      for (auto it = upperLonIt; it != this->longitudeStore.end(); ++it) {
        this->addNeighbor(storeItem, it->second, visibilityRange);
      }
    }
  }

  void Store::addNeighbor(StoreItem *storeItem, int neighbor, double visibilityRange) {
    if (storeItem->socket == neighbor) {
      return;
    }
    visibilityRange = OSATS_NAUTICAL_MILES_TO_DEGREES(visibilityRange);
    visibilityRange = visibilityRange * visibilityRange;
    StoreItem *neighborStoreItem = this->store[neighbor];
    double distance = Geo::distanceInDegreesSquared
                      (neighborStoreItem->compressible->getPosition(),
                       storeItem->compressible->getPosition());
    if (distance <= visibilityRange) {
      neighborStoreItem->neighbors.insert(storeItem->socket);
    }
  }
};