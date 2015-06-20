#ifndef _OSATS_STORE_
#define _OSATS_STORE_

#include <unordered_map>
#include <unordered_set>
#include <map>
#include <string>
#include <ctime>

namespace OSATS {
  class Compressible;

  typedef struct {
    int socket;
    Compressible *compressible;
    std::unordered_set<int> neighbors;
  } StoreItem;

  class Store {
  public:
    void put(int socket, Compressible *compressible);
    void remove(int socket);
    std::unordered_set<int> &getNeighbors(int socket);
  private:
    void removePosition(StoreItem *storeItem);
    void updateNeighbors(StoreItem *storeItem);
    void addNeighbor(StoreItem *storeItem, int neighbor,
                     double visibilityRange);
    std::unordered_map<int, StoreItem *> store;
    std::multimap<double, int> latitudeStore;
    std::multimap<double, int> longitudeStore;
  };
};

#endif