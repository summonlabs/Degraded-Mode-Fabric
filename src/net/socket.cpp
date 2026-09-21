// Degraded Mode Fabric - loopback transport.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstring>
#include <mutex>

#include "dmf/net.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dmf {

namespace {

#ifdef _WIN32
using native_socket = SOCKET;
constexpr native_socket kInvalidSocket = INVALID_SOCKET;
constexpr std::intptr_t kInvalidDescriptor = -1;

std::once_flag g_startup_once;
int g_startup_result = 0;

int native_close(native_socket socket) { return ::closesocket(socket); }
int native_shutdown(native_socket socket) { return ::shutdown(socket, SD_BOTH); }
int native_last_error() { return ::WSAGetLastError(); }
bool would_block(int code) { return code == WSAEWOULDBLOCK; }
bool interrupted(int code) { return code == WSAEINTR; }
#else
using native_socket = int;
constexpr native_socket kInvalidSocket = -1;
constexpr std::intptr_t kInvalidDescriptor = -1;

int native_close(native_socket socket) { return ::close(socket); }
int native_shutdown(native_socket socket) { return ::shutdown(socket, SHUT_RDWR); }
int native_last_error() { return errno; }
bool would_block(int code) { return code == EAGAIN || code == EWOULDBLOCK; }
bool interrupted(int code) { return code == EINTR; }
#endif

native_socket to_native(std::intptr_t descriptor) noexcept {
  return static_cast<native_socket>(descriptor);
}

Status socket_error(const char* what) {
  return Status(ErrorCode::IoError, std::string(what) + " failed with transport error");
}

}  // namespace

Status Socket::startup() {
#ifdef _WIN32
  std::call_once(g_startup_once, []() {
    WSADATA data{};
    g_startup_result = ::WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (g_startup_result != 0) {
    return Status(ErrorCode::IoError, "Winsock initialisation failed");
  }
#else
  static std::once_flag once;
  std::call_once(once, []() {
    // A peer that vanishes mid-write must surface as an error, not a signal.
    ::signal(SIGPIPE, SIG_IGN);
  });
#endif
  return Status{};
}

void Socket::teardown() {
#ifdef _WIN32
  ::WSACleanup();
#endif
}

Socket::~Socket() { close_quietly(); }

Socket::Socket(Socket&& other) noexcept {
  const std::lock_guard<std::mutex> guard(other.lifecycle_mutex_);
  descriptor_.store(other.descriptor_.exchange(kInvalidDescriptor));
  wakeup_.store(other.wakeup_.exchange(kInvalidDescriptor));
  std::memcpy(wakeup_address_, other.wakeup_address_, sizeof(wakeup_address_));
  wakeup_address_length_ = other.wakeup_address_length_;
  other.wakeup_address_length_ = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
    const std::intptr_t mine = descriptor_.exchange(kInvalidDescriptor);
    if (mine != kInvalidDescriptor) native_close(to_native(mine));
    const std::intptr_t my_wakeup = wakeup_.exchange(kInvalidDescriptor);
    if (my_wakeup != kInvalidDescriptor) native_close(to_native(my_wakeup));
    descriptor_.store(other.descriptor_.exchange(kInvalidDescriptor));
    wakeup_.store(other.wakeup_.exchange(kInvalidDescriptor));
    std::memcpy(wakeup_address_, other.wakeup_address_, sizeof(wakeup_address_));
    wakeup_address_length_ = other.wakeup_address_length_;
    other.wakeup_address_length_ = 0;
  }
  return *this;
}

Result<Socket> Socket::connect_to(const std::string& host, std::uint16_t port) {
  Status status = startup();
  if (!status.ok()) return status;
  if (host.empty() || host.size() > kMaxPathBytes) {
    return Status(ErrorCode::InvalidArgument, "connect host is empty or too long");
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0) {
    return Status(ErrorCode::IoError, "cannot resolve the connect host");
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(results, &::freeaddrinfo);
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    native_socket socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == kInvalidSocket) continue;
    if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      return Socket(static_cast<std::intptr_t>(socket));
    }
    native_close(socket);
  }
  return Status(ErrorCode::IoError, "no transport address accepted the connection");
}

Status Socket::write_all(const std::uint8_t* data, std::size_t size) {
  const std::intptr_t descriptor = descriptor_.load();
  if (descriptor == kInvalidDescriptor) return Status(ErrorCode::InvalidState, "socket is closed");
  std::size_t written = 0;
  while (written < size) {
    const std::size_t remaining = size - written;
#ifdef _WIN32
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1U << 20));
    const int result = ::send(to_native(descriptor), reinterpret_cast<const char*>(data + written),
                              chunk, 0);
#else
    const ssize_t result = ::send(to_native(descriptor), data + written, remaining, MSG_NOSIGNAL);
#endif
    if (result < 0) {
      const int code = native_last_error();
      if (interrupted(code)) continue;
      if (would_block(code)) continue;
      return socket_error("send");
    }
    if (result == 0) return Status(ErrorCode::IoError, "send returned zero");
    written += static_cast<std::size_t>(result);
  }
  return Status{};
}

Status Socket::read_some(std::uint8_t* data, std::size_t size, std::size_t& read_out) {
  read_out = 0;
  const std::intptr_t descriptor = descriptor_.load();
  if (descriptor == kInvalidDescriptor) return Status(ErrorCode::InvalidState, "socket is closed");
  if (wakeup_.load() != kInvalidDescriptor) {
    // Wait for readability through select() rather than blocking inside recv().
    // A blocked recv released by shutdown() leaves the operating system's
    // per-thread socket state in a way that delays thread termination by
    // minutes, so it is never used on a path that must shut down promptly.
    bool woken = false;
    Status waited = wait_readable(woken);
    if (!waited.ok()) return waited;
    if (woken) return Status(ErrorCode::Shutdown, "socket was released");
    if (descriptor_.load() == kInvalidDescriptor) {
      return Status(ErrorCode::Shutdown, "socket is closed");
    }
  }
  for (;;) {
#ifdef _WIN32
    const int chunk = static_cast<int>(std::min<std::size_t>(size, 1U << 20));
    const int result =
        ::recv(to_native(descriptor), reinterpret_cast<char*>(data), chunk, 0);
#else
    const ssize_t result = ::recv(to_native(descriptor), data, size, 0);
#endif
    if (result < 0) {
      const int code = native_last_error();
      if (interrupted(code)) continue;
      if (would_block(code)) continue;
      return socket_error("recv");
    }
    read_out = static_cast<std::size_t>(result);
    return Status{};
  }
}

Status Socket::release_blocked_io() noexcept {
  const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
  const std::intptr_t wakeup = wakeup_.load();
  if (wakeup != kInvalidDescriptor) {
    const char byte = 'w';
    (void)::sendto(to_native(wakeup), &byte, 1, 0,
                   reinterpret_cast<const sockaddr*>(wakeup_address_), wakeup_address_length_);
  }
  return Status{};
}

Status Socket::open_wakeup_channel() {
  native_socket wakeup = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (wakeup == kInvalidSocket) return socket_error("wakeup socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(wakeup, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    native_close(wakeup);
    return socket_error("wakeup bind");
  }
  sockaddr_in bound{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(bound));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
  if (::getsockname(wakeup, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    native_close(wakeup);
    return socket_error("wakeup getsockname");
  }
  static_assert(sizeof(bound) <= sizeof(wakeup_address_), "the wakeup address must fit");
  std::memcpy(wakeup_address_, &bound, sizeof(bound));
  wakeup_address_length_ = static_cast<int>(sizeof(bound));
  wakeup_.store(static_cast<std::intptr_t>(wakeup));
  return Status{};
}

Status Socket::wait_readable(bool& woken) {
  woken = false;
  const std::intptr_t descriptor = descriptor_.load();
  if (descriptor == kInvalidDescriptor) return Status(ErrorCode::Shutdown, "socket is closed");
  const std::intptr_t wakeup = wakeup_.load();
  if (wakeup == kInvalidDescriptor) return Status(ErrorCode::InvalidState, "no wakeup channel");
  fd_set reads;
  FD_ZERO(&reads);
  FD_SET(to_native(descriptor), &reads);
  FD_SET(to_native(wakeup), &reads);
  // No timeout: this waits indefinitely and is released by the wakeup channel.
  const int ready = ::select(0, &reads, nullptr, nullptr, nullptr);
  if (ready == SOCKET_ERROR) {
    const int code = native_last_error();
    if (interrupted(code)) return Status{};
    return socket_error("select");
  }
  if (FD_ISSET(to_native(wakeup), &reads)) {
    char drained[8];
    (void)::recvfrom(to_native(wakeup), drained, sizeof(drained), 0, nullptr, nullptr);
    woken = true;
  }
  return Status{};
}

Status Socket::close() {
  const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
  const std::intptr_t wakeup = wakeup_.exchange(kInvalidDescriptor);
  if (wakeup != kInvalidDescriptor) native_close(to_native(wakeup));
  const std::intptr_t descriptor = descriptor_.exchange(kInvalidDescriptor);
  if (descriptor == kInvalidDescriptor) return Status{};
  native_shutdown(to_native(descriptor));
  if (native_close(to_native(descriptor)) != 0) return socket_error("close");
  return Status{};
}

void Socket::close_quietly() noexcept {
  const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
  const std::intptr_t wakeup = wakeup_.exchange(kInvalidDescriptor);
  if (wakeup != kInvalidDescriptor) native_close(to_native(wakeup));
  const std::intptr_t descriptor = descriptor_.exchange(kInvalidDescriptor);
  if (descriptor != kInvalidDescriptor) {
    native_shutdown(to_native(descriptor));
    native_close(to_native(descriptor));
  }
}

bool Socket::closed_by_owner() const noexcept {
  return descriptor_.load() == kInvalidDescriptor;
}

Listener::~Listener() {
  Status status = close();
  (void)status;
}

Listener::Listener(Listener&& other) noexcept {
  descriptor_.store(other.descriptor_.exchange(kInvalidDescriptor));
  wakeup_.store(other.wakeup_.exchange(kInvalidDescriptor));
  std::memcpy(wakeup_address_, other.wakeup_address_, sizeof(wakeup_address_));
  wakeup_address_length_ = other.wakeup_address_length_;
  port_ = other.port_;
  other.wakeup_address_length_ = 0;
  other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    Status status = close();
    (void)status;
    descriptor_.store(other.descriptor_.exchange(kInvalidDescriptor));
    wakeup_.store(other.wakeup_.exchange(kInvalidDescriptor));
    std::memcpy(wakeup_address_, other.wakeup_address_, sizeof(wakeup_address_));
    wakeup_address_length_ = other.wakeup_address_length_;
    port_ = other.port_;
    other.wakeup_address_length_ = 0;
    other.port_ = 0;
  }
  return *this;
}

Result<Listener> Listener::bind_loopback(std::uint16_t port, int backlog) {
  Status status = Socket::startup();
  if (!status.ok()) return status;
  native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) return socket_error("socket");
  int reuse = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               static_cast<int>(sizeof(reuse)));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    native_close(socket);
    return socket_error("bind");
  }
  if (::listen(socket, backlog > 0 ? backlog : 1) != 0) {
    native_close(socket);
    return socket_error("listen");
  }
  // A bound loopback datagram socket is the wakeup channel: it is the only
  // reliable way to interrupt the select() that guards accept().
  native_socket wakeup = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (wakeup == kInvalidSocket) {
    native_close(socket);
    return socket_error("wakeup socket");
  }
  sockaddr_in wakeup_address{};
  wakeup_address.sin_family = AF_INET;
  wakeup_address.sin_port = 0;
  wakeup_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(wakeup, reinterpret_cast<sockaddr*>(&wakeup_address),
             static_cast<int>(sizeof(wakeup_address))) != 0) {
    native_close(socket);
    native_close(wakeup);
    return socket_error("wakeup bind");
  }
  sockaddr_in wakeup_bound{};
#ifdef _WIN32
  int wakeup_length = static_cast<int>(sizeof(wakeup_bound));
#else
  socklen_t wakeup_length = static_cast<socklen_t>(sizeof(wakeup_bound));
#endif
  if (::getsockname(wakeup, reinterpret_cast<sockaddr*>(&wakeup_bound), &wakeup_length) != 0) {
    native_close(socket);
    native_close(wakeup);
    return socket_error("wakeup getsockname");
  }
  sockaddr_in bound{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(bound));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    native_close(socket);
    return socket_error("getsockname");
  }
  Listener listener;
  listener.descriptor_.store(static_cast<std::intptr_t>(socket));
  listener.wakeup_.store(static_cast<std::intptr_t>(wakeup));
  static_assert(sizeof(wakeup_bound) <= sizeof(listener.wakeup_address_),
                "the wakeup address must fit in its buffer");
  std::memcpy(listener.wakeup_address_, &wakeup_bound, sizeof(wakeup_bound));
  listener.wakeup_address_length_ = static_cast<int>(sizeof(wakeup_bound));
  listener.port_ = ntohs(bound.sin_port);
  return listener;
}

Status Listener::wait_readable(bool& woken) {
  woken = false;
  const std::intptr_t descriptor = descriptor_.load();
  if (descriptor == kInvalidDescriptor) return Status(ErrorCode::Shutdown, "listener is closed");
  const std::intptr_t wakeup = wakeup_.load();
  if (wakeup == kInvalidDescriptor) return Status(ErrorCode::Shutdown, "listener is closed");
  fd_set reads;
  FD_ZERO(&reads);
  FD_SET(to_native(descriptor), &reads);
  FD_SET(to_native(wakeup), &reads);
  // No timeout: this waits indefinitely and is released by the wakeup channel,
  // never by expiring.
  const int ready = ::select(0, &reads, nullptr, nullptr, nullptr);
  if (ready == SOCKET_ERROR) {
    const int code = native_last_error();
    if (interrupted(code)) return Status(ErrorCode::Ok, "interrupted");
    return socket_error("select");
  }
  if (FD_ISSET(to_native(wakeup), &reads)) {
    char drained[8];
    (void)::recvfrom(to_native(wakeup), drained, sizeof(drained), 0, nullptr, nullptr);
    woken = true;
  }
  if (descriptor_.load() == kInvalidDescriptor) {
    return Status(ErrorCode::Shutdown, "listener is closed");
  }
  return Status{};
}

Status Listener::accept(Socket& out) {
  for (;;) {
    bool woken = false;
    Status waited = wait_readable(woken);
    if (!waited.ok()) return waited;
    if (woken) return Status(ErrorCode::Shutdown, "listener was released");
    const std::intptr_t descriptor = descriptor_.load();
    if (descriptor == kInvalidDescriptor) {
      return Status(ErrorCode::Shutdown, "listener is closed");
    }
    native_socket accepted = ::accept(to_native(descriptor), nullptr, nullptr);
    if (accepted == kInvalidSocket) {
      const int code = native_last_error();
      if (interrupted(code) || would_block(code)) continue;
      if (descriptor_.load() == kInvalidDescriptor) {
        return Status(ErrorCode::Shutdown, "listener was shut down");
      }
      return socket_error("accept");
    }
    out = Socket(static_cast<std::intptr_t>(accepted));
    return Status{};
  }
}

Status Listener::release_blocked_io() noexcept {
  const std::intptr_t wakeup = wakeup_.load();
  if (wakeup != kInvalidDescriptor) {
    const char byte = 'w';
    (void)::sendto(to_native(wakeup), &byte, 1, 0,
                   reinterpret_cast<const sockaddr*>(wakeup_address_), wakeup_address_length_);
  }
  const std::intptr_t descriptor = descriptor_.load();
  if (descriptor == kInvalidDescriptor) return Status{};
  native_shutdown(to_native(descriptor));
  return Status{};
}

Status Listener::close() {
  const std::intptr_t wakeup = wakeup_.exchange(kInvalidDescriptor);
  if (wakeup != kInvalidDescriptor) native_close(to_native(wakeup));
  const std::intptr_t descriptor = descriptor_.exchange(kInvalidDescriptor);
  if (descriptor == kInvalidDescriptor) return Status{};
  native_shutdown(to_native(descriptor));
  if (native_close(to_native(descriptor)) != 0) return socket_error("close");
  return Status{};
}

}  // namespace dmf