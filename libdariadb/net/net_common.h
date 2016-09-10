#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace dariadb {
namespace net {

const uint32_t PROTOCOL_VERSION=1;

enum class DataKinds : uint8_t {
  OK,
  ERR,
  HELLO,
  DISCONNECT,
  PING,
  PONG,
  WRITE,
  READ_INTERVAL,
  READ_TIMEPOINT
};

enum class ClientState {
  CONNECT, // connection is beginning but a while not ended.
  WORK,    // normal client.
  DISCONNECTED
};

enum class ERRORS: uint16_t{
    WRONG_PROTOCOL_VERSION
};

std::ostream &operator<<(std::ostream &stream, const ClientState &state);
std::ostream &operator<<(std::ostream &stream, const ERRORS &state);

typedef uint32_t QueryNumber;
}
}
