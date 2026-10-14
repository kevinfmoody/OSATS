#include "position.h"

namespace OSATS {
  void Position::setLatitude(double latitude) {
    this->latitude = latitude;
  }

  void Position::setLongitude(double longitude) {
    this->longitude = longitude;
  }

  void Position::setPosition(double latitude, double longitude) {
    this->setLatitude(latitude);
    this->setLongitude(longitude);
  }
};