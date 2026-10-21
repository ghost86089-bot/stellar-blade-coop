// Loopback UDP for the test link (loop_steam.h), Windows only, shared by steam_link.cpp and native/bot. Winsock is
// loaded only when the test link is on (STELLARCOOP_LOOP=host:PORT or guest:PORT), so a player's DLL imports nothing
// more than before. Only 127.0.0.1: the test link never leaves this PC.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace coop::loop {

// STELLARCOOP_LOOP's value: role and port, or false when unset or malformed.
inline bool parse_switch(const std::string &text, bool &host, uint16_t &port) {
  const size_t colon = text.find(':');
  if (colon == std::string::npos)
    return false;
  const std::string role = text.substr(0, colon);
  const unsigned long n = std::strtoul(text.c_str() + colon + 1, nullptr, 10);
  if ((role != "host" && role != "guest") || n == 0 || n > 65535)
    return false;
  host = role == "host";
  port = static_cast<uint16_t>(n);
  return true;
}

class Udp {
public:
  ~Udp() {
    if (sock_ != INVALID_SOCKET && close_)
      close_(sock_);
  }
  // host: bound to 127.0.0.1:port, answering whoever sent last. guest: any port, sending to 127.0.0.1:port.
  bool open(bool host, uint16_t port, std::string &why) {
    HMODULE ws = LoadLibraryW(L"ws2_32.dll");
    if (!ws)
      return why = "ws2_32.dll not loaded", false;
    auto get = [ws](const char *name) { return GetProcAddress(ws, name); };
    auto startup = reinterpret_cast<int(WSAAPI *)(WORD, WSADATA *)>(get("WSAStartup"));
    auto make = reinterpret_cast<SOCKET(WSAAPI *)(int, int, int)>(get("socket"));
    auto bind_ = reinterpret_cast<int(WSAAPI *)(SOCKET, const sockaddr *, int)>(get("bind"));
    auto nonblock = reinterpret_cast<int(WSAAPI *)(SOCKET, long, u_long *)>(get("ioctlsocket"));
    send_ = reinterpret_cast<int(WSAAPI *)(SOCKET, const char *, int, int, const sockaddr *, int)>(get("sendto"));
    recv_ = reinterpret_cast<int(WSAAPI *)(SOCKET, char *, int, int, sockaddr *, int *)>(get("recvfrom"));
    close_ = reinterpret_cast<int(WSAAPI *)(SOCKET)>(get("closesocket"));
    if (!startup || !make || !bind_ || !nonblock || !send_ || !recv_ || !close_)
      return why = "a Winsock export is missing", false;
    WSADATA data;
    if (startup(MAKEWORD(2, 2), &data) != 0)
      return why = "WSAStartup failed", false;
    sock_ = make(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET)
      return why = "no UDP socket", false;
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl_(0x7F000001); // 127.0.0.1 only.
    me.sin_port = host ? htons_(port) : 0;
    if (bind_(sock_, reinterpret_cast<const sockaddr *>(&me), sizeof me) != 0)
      return why = "port " + std::to_string(port) + " is taken", false;
    u_long on = 1;
    nonblock(sock_, FIONBIO, &on);
    peer_.sin_family = AF_INET;
    peer_.sin_addr.s_addr = htonl_(0x7F000001);
    peer_.sin_port = htons_(port);
    have_peer_ = !host; // The host answers only after the guest spoke.
    return true;
  }
  void send(const std::string &d) {
    if (have_peer_)
      send_(sock_, d.data(), static_cast<int>(d.size()), 0, reinterpret_cast<const sockaddr *>(&peer_), sizeof peer_);
  }
  // One datagram from 127.0.0.1, or false when none is waiting.
  bool receive(std::string &d) {
    char buffer[65536];
    for (;;) {
      sockaddr_in from{};
      int size = sizeof from;
      const int n = recv_(sock_, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr *>(&from), &size);
      if (n < 0)
        return false; // WSAEWOULDBLOCK (nothing waiting), or the guest's port closed (WSAECONNRESET).
      if (from.sin_addr.s_addr != htonl_(0x7F000001))
        continue; // Never anything from outside this PC.
      peer_ = from;
      have_peer_ = true;
      d.assign(buffer, static_cast<size_t>(n));
      return true;
    }
  }

private:
  static uint16_t htons_(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }
  static uint32_t htonl_(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  }
  SOCKET sock_ = INVALID_SOCKET;
  sockaddr_in peer_{};
  bool have_peer_ = false;
  int(WSAAPI *send_)(SOCKET, const char *, int, int, const sockaddr *, int) = nullptr;
  int(WSAAPI *recv_)(SOCKET, char *, int, int, sockaddr *, int *) = nullptr;
  int(WSAAPI *close_)(SOCKET) = nullptr;
};

} // namespace coop::loop
