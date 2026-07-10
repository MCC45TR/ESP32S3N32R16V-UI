#include "DNSServer.h"

#include <array>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <lwip/inet.h>
#include <lwip/sockets.h>
}

namespace {

struct DnsHeader {
  uint16_t id;
  uint16_t flags;
  uint16_t qdcount;
  uint16_t ancount;
  uint16_t nscount;
  uint16_t arcount;
};

static constexpr uint16_t kDnsResponseFlags = 0x8180U;

size_t skip_question(const uint8_t* data, const size_t len) {
  size_t offset = sizeof(DnsHeader);
  while (offset < len) {
    const uint8_t part_len = data[offset++];
    if (part_len == 0U) {
      break;
    }
    offset += part_len;
  }
  if (offset + 4U > len) {
    return 0U;
  }
  return offset + 4U;
}

}  // namespace

DNSServer::DNSServer() = default;

DNSServer::~DNSServer() { stop(); }

bool DNSServer::start(uint16_t port, const char*, IPAddress resolved_ip) {
  stop();

  socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_ < 0) {
    return false;
  }

  port_ = port;
  ip_ = resolved_ip;

  const int enable = 1;
  ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    stop();
    return false;
  }

  const int flags = ::fcntl(socket_, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
  }

  return true;
}

void DNSServer::processNextRequest() {
  if (socket_ < 0) {
    return;
  }

  std::array<uint8_t, 512> request = {};
  sockaddr_in source = {};
  socklen_t source_len = sizeof(source);
  const int received =
      ::recvfrom(socket_, reinterpret_cast<char*>(request.data()),
                 static_cast<int>(request.size()), 0,
                 reinterpret_cast<sockaddr*>(&source), &source_len);
  if (received <= static_cast<int>(sizeof(DnsHeader))) {
    return;
  }

  const size_t question_end =
      skip_question(request.data(), static_cast<size_t>(received));
  if (question_end == 0U) {
    return;
  }

  std::array<uint8_t, 512> response = {};
  std::memcpy(response.data(), request.data(), question_end);

  auto* header = reinterpret_cast<DnsHeader*>(response.data());
  header->flags = htons(kDnsResponseFlags);
  header->qdcount = htons(1U);
  header->ancount = htons(1U);
  header->nscount = 0U;
  header->arcount = 0U;

  size_t offset = question_end;
  response[offset++] = 0xC0U;
  response[offset++] = static_cast<uint8_t>(sizeof(DnsHeader));
  response[offset++] = 0x00U;
  response[offset++] = 0x01U;
  response[offset++] = 0x00U;
  response[offset++] = 0x01U;
  response[offset++] = 0x00U;
  response[offset++] = 0x00U;
  response[offset++] = 0x00U;
  response[offset++] = 0x3CU;
  response[offset++] = 0x00U;
  response[offset++] = 0x04U;
  response[offset++] = ip_[0];
  response[offset++] = ip_[1];
  response[offset++] = ip_[2];
  response[offset++] = ip_[3];

  ::sendto(socket_, reinterpret_cast<const char*>(response.data()),
           static_cast<int>(offset), 0, reinterpret_cast<sockaddr*>(&source),
           source_len);
}

void DNSServer::stop() {
  if (socket_ >= 0) {
    ::close(socket_);
    socket_ = -1;
  }
}
