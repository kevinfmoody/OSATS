#include "compressible.h"
#include "message.h"

namespace OSATS {
  Compressible::Compressible(size_t dataSize, std::string &identifier)
      : dataSize(dataSize), identifier(identifier) {
    size_t strLen = ceil((double)(identifier.size() + 1) / sizeof(int))
                    * sizeof(int);
    this->blobSize = strLen + (COMPRESSIBLE_HEADER_SIZE + this->dataSize)
                     * sizeof(int);
    this->blob = new char[this->blobSize];
    strcpy(this->blob, identifier.c_str());
    this->data = (int *)(this->blob + strLen);
  }

  Compressible::Compressible(size_t dataSize, Message &msg)
      : dataSize(dataSize) {
    this->blobSize = msg.getBlobSize();
    char *blob = msg.getBlob();
    this->identifier = std::string(blob);
    size_t strLen = blobSize - (COMPRESSIBLE_HEADER_SIZE + this->dataSize)
                    * sizeof(int);
    this->blob = new char[this->blobSize];
    memcpy(this->blob, blob, this->blobSize);
    this->data = (int *)(this->blob + strLen);
    this->position = Position((double)this->data[0] /
                              OSATS_COMPRESSIBLE_GEO_PRECISION,
                              (double)this->data[1] /
                              OSATS_COMPRESSIBLE_GEO_PRECISION);
  }

  Compressible::~Compressible() {
    delete this->blob;
  }

  char *Compressible::getBlob() {
    return this->blob;
  }

  size_t Compressible::getBlobSize() {
    return this->blobSize;
  }

  void Compressible::setVisibilityRange(double range) {
    this->visibilityRange = range;
  }

  void Compressible::setPosition(Position position) {
    this->position = position;
    this->data[0] = round(position.getLatitude() *
                          OSATS_COMPRESSIBLE_GEO_PRECISION);
    this->data[1] = round(position.getLongitude() *
                          OSATS_COMPRESSIBLE_GEO_PRECISION);
  }

  int Compressible::getDataPoint(size_t index) {
    return this->data[COMPRESSIBLE_HEADER_SIZE + index];
  }

  void Compressible::setDataPoint(size_t index, int value) {
    this->data[COMPRESSIBLE_HEADER_SIZE + index] = value;
  }
};