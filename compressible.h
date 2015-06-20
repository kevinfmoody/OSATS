#ifndef _OSATS_COMPRESSIBLE_
#define _OSATS_COMPRESSIBLE_

#include "position.h"
#include "config.h"

#include <string>
#include <vector>

#define COMPRESSIBLE_HEADER_SIZE 2

namespace OSATS {
  class Message;

  class Compressible {
  public:
    Compressible(size_t dataSize, std::string &identifier);
    Compressible(size_t dataSize, Message &msg);
    ~Compressible();
    char *getBlob();
    size_t getBlobSize();
    inline std::string &getIdentifier() { return this->identifier; }
    inline Position &getPosition() { return this->position; }
    inline double getVisibilityRange() { return this->visibilityRange; }
    void setPosition(Position position);
    void setVisibilityRange(double range);
  protected:
    int getDataPoint(size_t index);
    void setDataPoint(size_t index, int value);
  private:
    int *data;
    size_t dataSize;
    char *blob;
    size_t blobSize;
    std::string identifier;
    Position position;
    double visibilityRange = 60;
  };
};

#endif