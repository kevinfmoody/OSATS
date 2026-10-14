#ifndef _OSATS_AIRCRAFT_
#define _OSATS_AIRCRAFT_

#include "compressible.h"

#include <string>
#include <vector>

#define AIRCRAFT_DATA_SIZE 3

namespace OSATS {
  class Message;

  enum AircraftData {
    AD_ALTITUDE,
    AD_GROUNDSPEED,
    AD_HEADING
  };

  class Aircraft : public Compressible {
  public:
    Aircraft(std::string &identifier);
    Aircraft(Message &message);
    inline double getAltitude() { return this->altitude; }
    inline double getGroundspeed() { return this->groundspeed; }
    inline double getHeading() { return this->heading; }
    void setAltitude(double altitude);
    void setGroundspeed(double groundspeed);
    void setHeading(double heading);
    void dump();
  private:
    double altitude;
    double groundspeed;
    double heading;
  };
};

#endif