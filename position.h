#ifndef _OSATS_POSITION_
#define _OSATS_POSITION_

#include <cmath>

namespace OSATS {
  class Position {
  public:
    Position() : latitude(0), longitude(0) {};
    Position(double latitude, double longitude)
      : latitude(latitude), longitude(longitude) {};
    inline double getLatitude() { return this->latitude; }
    inline double getLongitude() { return this->longitude; }
    void setLatitude(double latitude);
    void setLongitude(double longitude);
    void setPosition(double latitude, double longitude);
  private:
    double latitude;
    double longitude;
  };
};

#endif