#pragma once

#include "osats/geo.hpp"
#include "osats/types.hpp"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace osats {

// Uniform geographic hash grid for interest queries.
class SpatialIndex {
 public:
  explicit SpatialIndex(double cell_size_deg = 0.25)  // ~15 nm at equator
      : cell_size_deg_(cell_size_deg) {}

  void upsert(EntityId id, const Position& pos) {
    const Cell cell = cellFor(pos);
    auto it = locations_.find(id);
    if (it != locations_.end()) {
      if (it->second.cell == cell) {
        it->second.pos = pos;
        return;
      }
      eraseFromCell(it->second.cell, id);
      it->second = {cell, pos};
    } else {
      locations_.emplace(id, EntityLoc{cell, pos});
    }
    cells_[cell].insert(id);
  }

  void remove(EntityId id) {
    auto it = locations_.find(id);
    if (it == locations_.end()) return;
    eraseFromCell(it->second.cell, id);
    locations_.erase(it);
  }

  // Gather candidate entity ids within a bounding circle (nm).
  void queryRadius(const Position& center, double radius_nm,
                   std::vector<EntityId>& out) const {
    out.clear();
    double min_lat, max_lat, min_lon, max_lon;
    geo::boundsNm(center, radius_nm, min_lat, max_lat, min_lon, max_lon);

    const int min_x = static_cast<int>(std::floor(min_lat / cell_size_deg_));
    const int max_x = static_cast<int>(std::floor(max_lat / cell_size_deg_));

    // Longitude may wrap; handle simple non-wrap and wrap cases.
    if (min_lon <= max_lon) {
      const int min_y = static_cast<int>(std::floor(min_lon / cell_size_deg_));
      const int max_y = static_cast<int>(std::floor(max_lon / cell_size_deg_));
      collect(min_x, max_x, min_y, max_y, out);
    } else {
      const int min_y1 = static_cast<int>(std::floor(min_lon / cell_size_deg_));
      const int max_y1 = static_cast<int>(std::floor(180.0 / cell_size_deg_));
      const int min_y2 = static_cast<int>(std::floor(-180.0 / cell_size_deg_));
      const int max_y2 = static_cast<int>(std::floor(max_lon / cell_size_deg_));
      collect(min_x, max_x, min_y1, max_y1, out);
      collect(min_x, max_x, min_y2, max_y2, out);
    }
  }

  bool contains(EntityId id) const { return locations_.count(id) != 0; }

  const Position* positionOf(EntityId id) const {
    auto it = locations_.find(id);
    if (it == locations_.end()) return nullptr;
    return &it->second.pos;
  }

  std::size_t size() const { return locations_.size(); }

 private:
  struct Cell {
    int x = 0;
    int y = 0;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y; }
  };

  struct CellHash {
    std::size_t operator()(const Cell& c) const noexcept {
      return (static_cast<std::size_t>(c.x) * 1315423911u) ^
             static_cast<std::size_t>(c.y);
    }
  };

  struct EntityLoc {
    Cell cell;
    Position pos;
  };

  Cell cellFor(const Position& pos) const {
    return Cell{static_cast<int>(std::floor(pos.latitude_deg / cell_size_deg_)),
                static_cast<int>(std::floor(pos.longitude_deg / cell_size_deg_))};
  }

  void eraseFromCell(const Cell& cell, EntityId id) {
    auto it = cells_.find(cell);
    if (it == cells_.end()) return;
    it->second.erase(id);
    if (it->second.empty()) cells_.erase(it);
  }

  void collect(int min_x, int max_x, int min_y, int max_y,
               std::vector<EntityId>& out) const {
    for (int x = min_x; x <= max_x; ++x) {
      for (int y = min_y; y <= max_y; ++y) {
        auto it = cells_.find(Cell{x, y});
        if (it == cells_.end()) continue;
        out.insert(out.end(), it->second.begin(), it->second.end());
      }
    }
  }

  double cell_size_deg_;
  std::unordered_map<EntityId, EntityLoc> locations_;
  std::unordered_map<Cell, std::unordered_set<EntityId>, CellHash> cells_;
};

}  // namespace osats
