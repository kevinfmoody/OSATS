#ifndef _OSATS_MESSAGE_
#define _OSATS_MESSAGE_

#include <vector>

namespace OSATS {
  class Aircraft;
  class Compressible;

  enum MessageType {
    MT_AIRCRAFT_POSITION
  };

  class Message {
  public:
    Message(MessageType type, Compressible &item);
    Message(Aircraft &ac);
    Message(char *msg);
    ~Message();
    inline MessageType getType() { return this->type; }
    inline char *getMsg() { return this->msg; }
    inline size_t getLen() { return this->len; }
    inline char *getBlob() { return this->blob; }
    inline size_t getBlobSize() { return this->blobSize; }
  private:
    MessageType type;
    char *msg;
    size_t len;
    char *blob;
    size_t blobSize;
  };
};

#endif