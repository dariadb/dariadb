#pragma once

#include "net_common.h"
#include <memory>
#include <string>
#include "../meas.h"
#include "../storage/query_param.h"
namespace dariadb {
namespace net {
class Client {
public:
  struct Param {
    std::string host;
    int port;
    Param(const std::string &_host, int _port) {
      host = _host;
      port = _port;
    }
  };
  Client(const Param &p);
  ~Client();

  void connect();
  void disconnect();

  ClientState state() const;
  size_t pings_answers() const;


 /* void write(const Meas::MeasArray&ma);
  Meas::MeasList read(const storage::QueryInterval&qi);*/
protected:
  class Private;
  std::unique_ptr<Private> _Impl;
};
}
}
