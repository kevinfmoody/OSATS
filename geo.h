#ifndef _OSATS_GEO_
#define _OSATS_GEO_

#include <algorithm>
#include <cmath>

#include "config.h"

#define OSATS_NAUTICAL_MILES_TO_DEGREES(nm) (nm / 60)
#define OSATS_DEGREES_TO_RADIANS(deg) (deg * M_PI / 180)
#define OSATS_RADIANS_TO_DEGREES(rad) (rad * 180 / M_PI)

namespace OSATS {
  namespace Geo {
    void bounds(Position &pos, double radius,
                     double &minLat, double &maxLat,
                     double &minLon, double &maxLon);
    double distanceInDegreesSquared(Position &a, Position &b);
    void handleOverflow(double &lat, double &lon);
    
    void bounds(Position &pos, double radius,
                     double &minLat, double &maxLat,
                     double &minLon, double &maxLon) {
      double range = OSATS_NAUTICAL_MILES_TO_DEGREES(radius);
      double lat = pos.getLatitude();
      double lon = pos.getLongitude();
      minLat = lat - range;
      maxLat = lat + range;
      double scale = 1.0 / std::min(cos(OSATS_DEGREES_TO_RADIANS(minLat)),
                                    cos(OSATS_DEGREES_TO_RADIANS(maxLat)));
      minLon = lon - scale * range;
      maxLon = lon + scale * range;
      handleOverflow(minLat, minLon);
      handleOverflow(maxLat, maxLon);
    }

    double distanceInDegreesSquared(Position &a, Position &b) {
      double latA = a.getLatitude();
      double lonA = a.getLongitude();
      double latB = b.getLatitude();
      double lonB = b.getLongitude();
      double scale = 1.0 / ((cos(OSATS_DEGREES_TO_RADIANS(latA)) +
                             cos(OSATS_DEGREES_TO_RADIANS(latB))) / 2);
      double lat = latA - latB;
      double lon = scale * (lonA - lonB);
      return lat * lat + lon * lon;
    }

    void handleOverflow(double &lat, double &lon) {
      if (lat < -90) {
        lat += 180;
      } else if (lat > 90) {
        lat -= 180;
      }
      if (lon < -180) {
        lon += 360;
      } else if (lon > 180) {
        lon -= 360;
      }
    }
  };
};

#endif