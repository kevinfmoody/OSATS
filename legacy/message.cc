#include "message.h"
#include "aircraft.h"
#include "compressible.h"

namespace OSATS {
  Message::Message(MessageType type, Compressible &item) : type(type) {
    this->blobSize = item.getBlobSize();
    this->len = 2 * sizeof(int) + this->blobSize;
    this->msg = new char[this->len];
    this->blob = this->msg + 2 * sizeof(int);
    int *header = (int *)this->msg;
    header[0] = this->len;
    header[1] = this->type;
    memcpy(this->blob, item.getBlob(), blobSize);
  }

  Message::Message(Aircraft &ac) : Message(MT_AIRCRAFT_POSITION, ac) {}

  Message::Message(char *msg) {
    int *header = (int *)msg;
    this->len = header[0];
    this->type = (MessageType)header[1];
    this->msg = new char[this->len];
    this->blob = this->msg + 2 * sizeof(int);
    this->blobSize = this->len - 2 * sizeof(int);
    memcpy(this->msg, msg, this->len);
  }

  Message::~Message() {
    delete this->msg;
  }
};