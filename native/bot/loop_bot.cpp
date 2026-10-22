// The bot partner for the test link: one StellarCoop link (link_core.h) on the stand-in Steam (loop_steam.h) over
// loopback UDP, so one game, or no game at all, can be tested against a second player on this PC.
//
//   loop_bot guest PORT [SECONDS] [ECHO_CM]  join the host on 127.0.0.1:PORT (the game with STELLARCOOP_LOOP=host:PORT)
//   loop_bot host PORT [SECONDS]             host, for a second bot or a game with STELLARCOOP_LOOP=guest:PORT
//   loop_bot dll FOLDER PORT [SECONDS]       load FOLDER\StellarCoop.dll here as the host (STELLARCOOP_LOOP=host:PORT)
//                                            and ask it to host through coop-request.txt, as Lua does: the DLL's whole
//                                            link path without the game
//
// Once linked, the bot says one chat line. The guest echoes (pose_echo.h): every table, keyframe and delta the host
// sends comes back as the bot's, its EVE ECHO_CM (default 150, 0 off) along X, so the host's game draws the bot's EVE
// beside its own. The host bot is a pose source for that: one table, then a keyframe every 50 ms, and it checks that
// each echoed keyframe comes back exactly ECHO_CM away. It prints the link's events and ends with
// "BOT RESULT: linked=0|1 ..." (exit code 0 only when it was linked).
#include "loop_steam.h"
#include "loop_udp.h"
#include "pose_echo.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
uint64_t binary_in = 0;
std::vector<std::string> binary_queue; // Channel-1 messages received this pass, echoed or checked after tick().

namespace pose = coop::pose;
// The host bot's pose source: a small EVE (two leaders, one follower) whose keyframes move along X.
pose::Table source_table() {
  pose::Table t;
  t.generation = 777;
  t.map = "/game/loop/bot";
  pose::Part leader{1, 1, 1, 5, true}, follower{1, 1, 1, 5, false}, small{1, 1, 1, 3, true};
  t.eve = {leader, follower, small};
  // What format_table writes for each part (it reads look[i] for every part): a name, leader and parent, the
  // leaders' bone names, and an identity placement under part 0.
  t.look.resize(t.eve.size());
  for (size_t i = 0; i < t.eve.size(); ++i) {
    pose::Look &l = t.look[i];
    l.name = "bot_part_" + std::to_string(i);
    l.parent = i == 0 ? -1 : 0;
    l.lead = t.eve[i].leader ? -1 : 0;
    const float identity[10] = {0, 0, 0, 1, 0, 0, 0, 1, 1, 1};
    std::copy(identity, identity + 10, l.place);
    if (t.eve[i].leader)
      for (uint32_t b = 0; b < t.eve[i].bones; ++b)
        l.names.push_back("bone_" + std::to_string(i) + "_" + std::to_string(b));
  }
  return t;
}
std::string source_pose(const pose::Table &t, uint32_t seq, float x) {
  const float root[10] = {0, 0, 0, 1, x, 200, 300, 1, 1, 1};
  std::vector<float> bones;
  for (int b = 0; b < 8; ++b) {
    const float one[12] = {0, 0, 0, 1, float(b), 0, 0, 0, 1, 1, 1, 0};
    bones.insert(bones.end(), one, one + 12);
  }
  pose::Effect fx[pose::kEffectSlots] = {};
  pose::PoseHeader h;
  h.generation = t.generation, h.sequence = seq, h.publication = uint64_t(GetTickCount64()) * 1000, h.map = 0;
  std::string out(pose::pose_size(t), '\0');
  uint32_t clamped = 0;
  out.resize(pose::encode_pose(t, h, root, reinterpret_cast<const uint8_t *>(bones.data()), fx,
                               reinterpret_cast<uint8_t *>(&out[0]), clamped));
  return out;
}
uint32_t sequence_of(const std::string &m) { // SBP/SBD: magic and protocol, generation (8), then the sequence.
  uint32_t s = 0;
  if (m.size() >= 16)
    std::memcpy(&s, m.data() + 12, 4);
  return s;
}

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

int run_link(bool host, uint16_t port, uint64_t seconds, float echo_cm) {
  coop::loop::Udp udp;
  std::string why;
  if (!udp.open(host, port, why))
    return std::printf("BOT RESULT: linked=0 (%s)\n", why.c_str()), 2;
  coop::loop::Steam steam(host, [&udp](const std::string &d) { udp.send(d); });
  coop::loop::instance[0] = &steam;
  const coop::Api api = coop::loop::api<0>();
  coop::Link link(api, "loop-bot", [](const std::string &e) { std::printf("bot: %s\n", e.c_str()); },
                  [](const uint8_t *d, size_t n) {
                    ++binary_in;
                    binary_queue.emplace_back(reinterpret_cast<const char *>(d), n);
                  });
  link.world(true);
  bool was_linked = false, said = false, table_sent = false;
  int heard = 0, echoed = 0, echo_ok = 0, echo_bad = 0;
  const pose::Table table = source_table();
  std::map<uint32_t, float> sent_x; // The host bot's keyframes: sequence to world X.
  uint32_t next_seq = 1;
  uint64_t next_pose = 0;
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
      said = true;
    }
    for (auto &line : link.take_chat())
      std::printf("bot heard: %s\n", line.c_str()), ++heard;
    if (host && link.linked() && echo_cm != 0) { // The pose source: the table once, then a keyframe every 50 ms.
      if (!std::exchange(table_sent, true)) {
        const std::string t = pose::format_table(table);
        link.send_binary(reinterpret_cast<const uint8_t *>(t.data()), t.size(), true);
      }
      if (now >= next_pose && sent_x.size() < 400) {
        const float x = 100.0f + 2.0f * next_seq;
        const std::string p = source_pose(table, next_seq, x);
        if (!p.empty())
          sent_x[next_seq] = x, link.send_binary(reinterpret_cast<const uint8_t *>(p.data()), p.size(), false);
        ++next_seq, next_pose = now + 50;
      }
    }
    for (std::string &m : std::exchange(binary_queue, {})) {
      if (host) { // An echoed keyframe must come back exactly echo_cm along X from the one we sent.
        const size_t at = pose::world_x_at(m);
        auto it = sent_x.find(sequence_of(m));
        if (m.size() > 3 && m[2] == 'P' && at && it != sent_x.end()) {
          float x;
          std::memcpy(&x, m.data() + at, 4);
          ++(std::fabs(x - (it->second + echo_cm)) < 0.01f ? echo_ok : echo_bad);
        }
      } else if (echo_cm != 0 && pose::echo_shift(m, echo_cm)) {
        link.send_binary(reinterpret_cast<const uint8_t *>(m.data()), m.size(), m[2] == 'T');
        if (std::exchange(echoed, echoed + 1) == 0)
          std::printf("bot: echoing the host's EVE %.0f cm along X\n", echo_cm);
      }
    }
    Sleep(20);
  }
  std::printf("BOT RESULT: linked=%d said=%d heard=%d binary_in=%llu echoed=%d echo_ok=%d echo_bad=%d\n",
              was_linked ? 1 : 0, said ? 1 : 0, heard, static_cast<unsigned long long>(binary_in), echoed, echo_ok,
              echo_bad);
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
                    argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 30, argc > 4 ? float(std::atof(argv[4])) : 150.0f);
  if (mode == "dll" && argc >= 4)
    return run_dll(argv[2], static_cast<uint16_t>(std::atoi(argv[3])),
                   argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 30);
  std::fputs("usage: loop_bot guest|host PORT [SECONDS] | loop_bot dll FOLDER PORT [SECONDS]\n", stderr);
  return 2;
}
