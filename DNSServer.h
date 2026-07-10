#pragma once

#include <cstdint>

#include "IPAddress.h"

class DNSServer {
 public:
  DNSServer();
  ~DNSServer();

  bool start(uint16_t port, const char* domain_name, IPAddress resolved_ip);
  void processNextRequest();
  void stop();

 private:
  int socket_ = -1;
  uint16_t port_ = 53;
  IPAddress ip_;
};
