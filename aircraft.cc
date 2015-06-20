#include "aircraft.h"
#include "message.h"

#include <iostream>

namespace OSATS {
  Aircraft::Aircraft(std::string &identifier)
    : Compressible(AIRCRAFT_DATA_SIZE, identifier) {};

  Aircraft::Aircraft(Message &msg) : Compressible(AIRCRAFT_DATA_SIZE, msg) {
    this->altitude = this->getDataPoint(AD_ALTITUDE);
    this->groundspeed = this->getDataPoint(AD_GROUNDSPEED);
    this->heading = this->getDataPoint(AD_HEADING);
  }

  void Aircraft::setAltitude(double altitude) {
    this->altitude = altitude;
    this->setDataPoint(AD_ALTITUDE, round(altitude));
  }

  void Aircraft::setGroundspeed(double groundspeed) {
    this->groundspeed = groundspeed;
    this->setDataPoint(AD_GROUNDSPEED, round(groundspeed));
  }

  void Aircraft::setHeading(double heading) {
    this->heading = heading;
    this->setDataPoint(AD_HEADING, round(heading));
  }

  void Aircraft::dump() {
    std::cout << this->getIdentifier() << std::endl;
    std::cout << this->getAltitude() << std::endl;
    std::cout << this->getGroundspeed() << std::endl;
    std::cout << this->getHeading() << std::endl;
    Position &pos = this->getPosition();
    std::cout << pos.getLatitude() << std::endl;
    std::cout << pos.getLongitude() << std::endl;
  }
};