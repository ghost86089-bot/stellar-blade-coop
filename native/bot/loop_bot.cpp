// The bot partner for the test link: one StellarCoop link (link_core.h) on the stand-in Steam (loop_steam.h) over
// loopback UDP, so one game, or no game at all, can be tested against a second player on this PC.
//
//   loop_bot guest PORT [SECONDS]     join the host on 127.0.0.1:PORT (the game with STELLARCOOP_LOOP=host:PORT)
//   loop_bot host PORT [SECONDS]      host, for a second bot or a game with STELLARCOOP_LOOP=guest:PORT
//   loop_bot dll FOLDER PORT [SECONDS] load FOLDER\StellarCoop.dll here as the host (STELLARCOOP_LOOP=host:PORT) and
//                                     ask it to host through coop-request.txt, as Lua does: the DLL's whole link path
//                                     without the game
//
// Once linked, the bot says one chat line and sends one pose-sized message on channel 1; it prints the link's events
// and ends with "BOT RESULT: linked=0|1 ..." (exit code 0 only when it was linked).
#include "loop_steam.h"
#include "loop_udp.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {
uint64_t binary_in = 0;

std::string read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
void write_file(const std::string &path, const std::string &text) {
  std::ofstream(path + ".tmp", std::ios::binary) << text;
  MoveFileExA((path + ".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

int run_link(bool host, uint16_t port, uint64_t seconds) {
  coop::loop::Udp udp;
  std::string why;
  if (!udp.open(host, port, why))
    return std::printf("BOT RESULT: linked=0 (%s)\n", why.c_str()), 2;
  coop::loop::Steam steam(host, [&udp](const std::string &d) { udp.send(d); });
  coop::loop::instance[0] = &steam;
  const coop::Api api = coop::loop::api<0>();
  coop::Link link(api, "loop-bot", [](const std::string &e) { std::printf("bot: %s\n", e.c_str()); },
                  [](const uint8_t *, size_t) { ++binary_in; });
  link.world(true);
  bool was_linked = false, said = false;
  int heard = 0;
  const uint64_t start = GetTickCount64(), end = start + seconds * 1000;
  std::string request = host ? "host" : "";
  uint64_t join_at = start + 500; // The guest asks once the host had a moment to create the lobby.
  link.ready(L"loop_bot.exe");
  while (GetTickCount64() < end) {
    std::string d;
    while (udp.receive(d))
      steam.on_datagram(d);
    const uint64_t now = GetTickCount64();
    steam.pump(now);
    if (!host && join_at && now >= join_at)
      request = "join " + std::to_string(coop::loop::kLoopLobby), join_at = 0;
    link.tick(now, std::exchange(request, ""), 0, steam.take_session());
    if (link.linked() && !std::exchange(was_linked, true))
      std::printf("bot: LINKED after %llu ms\n", static_cast<unsigned long long>(now - start));
    if (was_linked && !said && link.chatting()) {
      link.say("hello from the bot");
      link.send_binary(reinterpret_cast<const uint8_t *>("bot-pose"), 8, false);
      said = true;
    }
    for (auto &line : link.take_chat())
      std::printf("bot heard: %s\n", line.c_str()), ++heard;
    Sleep(20);
  }
  std::printf("BOT RESULT: linked=%d said=%d heard=%d binary_in=%llu\n", was_linked ? 1 : 0, said ? 1 : 0, heard,
              static_cast<unsigned long long>(binary_in));
  return was_linked ? 0 : 1;
}

int run_dll(const std::string &folder, uint16_t port, uint64_t seconds) {
  SetEnvironmentVariableA("STELLARCOOP_LOOP", ("host:" + std::to_string(port)).c_str());
  DeleteFileA((folder + "\\coop-status.txt").c_str());
  HMODULE dll = LoadLibraryA((folder + "\\StellarCoop.dll").c_str());
  if (!dll)
    return std::printf("BOT RESULT: linked=0 (StellarCoop.dll not loaded: %lu)\n", GetLastError()), 2;
  bool linked = false, asked = false;
  std::string status;
  for (const uint64_t end = GetTickCount64() + seconds * 1000; GetTickCount64() < end;) {
    Sleep(200);
    status = read_file(folder + "\\coop-status.txt");
    // Its worker deletes a request left from before it started: ask once it wrote its first status, as Lua would.
    if (!asked && status.find("steam=ready") != std::string::npos)
      write_file(folder + "\\coop-request.txt", "host"), asked = true;
    // A peer that passed the handshake both ways (link_core.h status()).
    for (size_t at = 0; (at = status.find("peer=", at)) != std::string::npos; ++at) {
      const std::string line = status.substr(at, status.find('\n', at) - at);
      linked = linked || (line.find(" accepted=1") != std::string::npos &&
                          line.find(" compatible=1") != std::string::npos);
    }
    if (linked)
      break;
  }
  std::printf("--- coop-status.txt\n%s--- coop-link.log\n%s", status.c_str(),
              read_file(folder + "\\coop-link.log").c_str());
  std::printf("BOT RESULT: dll linked=%d\n", linked ? 1 : 0);
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), linked ? 0 : 1); // The DLL's worker never stops on its own.
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0); // Every line reaches a redirected file even if a test stops the bot.
  const std::string mode = argc > 1 ? argv[1] : "";
  if ((mode == "guest" || mode == "host") && argc >= 3)
    return run_link(mode == "host", static_cast<uint16_t>(std::atoi(argv[2])),
                    argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 30);
  if (mode == "dll" && argc >= 4)
    return run_dll(argv[2], static_cast<uint16_t>(std::atoi(argv[3])),
                   argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 30);
  std::fputs("usage: loop_bot guest|host PORT [SECONDS] | loop_bot dll FOLDER PORT [SECONDS]\n", stderr);
  return 2;
}
