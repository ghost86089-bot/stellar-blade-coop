// The bot partner for the test link: one StellarCoop link (link_core.h) on the stand-in Steam (loop_steam.h) over
// loopback UDP, so one game, or no game at all, can be tested against a second player on this PC.
//
//   loop_bot guest PORT [SECONDS] [ECHO_CM] [HIT_HP]
//                                            join the host on 127.0.0.1:PORT (the game with STELLARCOOP_LOOP=host:PORT);
//                                            HIT_HP > 0: also the shared fight's guest (fight_bot.h), a claim of that
//                                            many HP once a second on an enemy the host announced
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
#include "fight_bot.h"
#include "host_world.h"
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

int run_link(bool host, uint16_t port, uint64_t seconds, float echo_cm, int32_t hit_hp, const std::string &world_file) {
  const std::string world = !host || world_file.empty() ? std::string() : read_file(world_file);
  if (host && !world_file.empty() && !coop::twins::game_save(world))
    return std::printf("BOT RESULT: linked=0 (%s is not a save the game would load)\n", world_file.c_str()), 2;
  coop::world::Sender world_out;
  coop::world::Receiver world_in; // The guest bot's: with a WORLD_SAVE path, it asks once linked and writes it there.
  int world_requests = 0, world_chunks = 0, world_received = 0;
  uint32_t world_id_next = 1;
  uint64_t world_ask_at = 0; // The guest bot asks every 10 s until a world arrives (a host at its title has none yet).
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
  coop::fight::FightBot fight(hit_hp, 1000);
  const bool fighting = !host && hit_hp > 0;
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
    if (!host && !world_file.empty() && link.linked() && !world_received && now >= world_ask_at &&
        !(world_in.active() && world_in.got() > 0)) { // Never while chunks are arriving.
      const uint32_t id = world_id_next++;
      world_in.start(id);
      const std::string q = coop::world::request(id);
      link.send_binary(reinterpret_cast<const uint8_t *>(q.data()), q.size(), true);
      world_ask_at = now + 10000;
      std::printf("bot: asked for the host's world (request %u)\n", id);
    }
    for (std::string &m : std::exchange(binary_queue, {})) {
      uint32_t world_id = 0;
      coop::world::Chunk chunk;
      if (!host && coop::world::parse_chunk(reinterpret_cast<const uint8_t *>(m.data()), m.size(), chunk)) {
        const auto r = world_in.take(chunk);
        if (r == coop::world::Receiver::Result::done) {
          write_file(world_file, world_in.bytes());
          world_received = int(world_in.bytes().size());
          std::printf("bot: the host's world received: %d bytes from its slot %u, CRC and the game's check pass; "
                      "written to %s\n",
                      world_received, world_in.host_slot(), world_file.c_str());
        } else if (r == coop::world::Receiver::Result::bad) {
          std::printf("bot: the host's world refused: %s\n", world_in.why().c_str());
        }
        continue;
      }
      if (host && coop::world::parse_request(reinterpret_cast<const uint8_t *>(m.data()), m.size(), world_id)) {
        ++world_requests;
        if (world.empty())
          std::printf("bot: the guest asked for the host's world (request %u); none given\n", world_id);
        else
          world_out.start(world_id, world, 0),
              std::printf("bot: sending the host's world (request %u): %zu bytes in %u chunks\n", world_id,
                          world.size(), world_out.count());
      } else if (host) { // An echoed keyframe must come back exactly echo_cm along X from the one we sent.
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
    if (host && link.linked())
      if (const std::string c = world_out.next(now); !c.empty())
        link.send_binary(reinterpret_cast<const uint8_t *>(c.data()), c.size(), true), ++world_chunks;
    if (fighting) { // Channel 2: the host's lines in, our answers and claims out, all reliable.
      for (const std::string &line : link.take_fight()) {
        std::printf("bot fight in: %s\n", line.c_str());
        for (const std::string &reply : fight.receive(line))
          std::printf("bot fight out: %s\n", reply.c_str()), link.send_fight(reply, true);
      }
      for (const std::string &claim : fight.tick(now))
        std::printf("bot fight out: %s\n", claim.c_str()), link.send_fight(claim, true);
    } else {
      link.take_fight(); // Not fighting: nothing keeps them.
    }
    Sleep(20);
  }
  const coop::fight::FightBot::Counts &fc = fight.counts();
  std::printf("BOT RESULT: linked=%d said=%d heard=%d binary_in=%llu echoed=%d echo_ok=%d echo_bad=%d fight_ups=%llu "
              "fight_hp_lines=%llu fight_hits=%llu fight_confirmed=%llu fight_unconfirmed=%llu fight_deads=%llu "
              "fight_bad=%llu world_requests=%d world_chunks=%d world_received=%d\n",
              was_linked ? 1 : 0, said ? 1 : 0, heard, static_cast<unsigned long long>(binary_in), echoed, echo_ok,
              echo_bad, static_cast<unsigned long long>(fc.ups), static_cast<unsigned long long>(fc.hp_lines),
              static_cast<unsigned long long>(fc.hits), static_cast<unsigned long long>(fc.confirmed),
              static_cast<unsigned long long>(fc.unconfirmed), static_cast<unsigned long long>(fc.deads),
              static_cast<unsigned long long>(fc.bad), world_requests, world_chunks, world_received);
  return was_linked ? 0 : 1;
}

// The DLL in this process. As host it is asked to host and the run ends once a peer linked. As guest (LOCALAPPDATA
// pointed into FOLDER\local, so its saves folder is a fresh one there) it is asked to join, and the run ends once it
// wrote the host's world: then that co-op slot must hold exactly expect_file's bytes.
int run_dll(bool as_host, const std::string &folder, uint16_t port, uint64_t seconds, const std::string &expect_file) {
  SetEnvironmentVariableA("STELLARCOOP_LOOP", ((as_host ? "host:" : "guest:") + std::to_string(port)).c_str());
  const std::string saves = folder + "\\local\\SB\\Saved\\SaveGames";
  if (!as_host) {
    SetEnvironmentVariableA("LOCALAPPDATA", (folder + "\\local").c_str());
    for (const std::string &d : {folder + "\\local", folder + "\\local\\SB", folder + "\\local\\SB\\Saved", saves})
      CreateDirectoryA(d.c_str(), nullptr);
  }
  DeleteFileA((folder + "\\coop-status.txt").c_str());
  HMODULE dll = LoadLibraryA((folder + "\\StellarCoop.dll").c_str());
  if (!dll)
    return std::printf("BOT RESULT: linked=0 (StellarCoop.dll not loaded: %lu)\n", GetLastError()), 2;
  bool linked = false, asked = false, world = false;
  std::string status;
  for (const uint64_t end = GetTickCount64() + seconds * 1000; GetTickCount64() < end;) {
    Sleep(200);
    status = read_file(folder + "\\coop-status.txt");
    // Its worker deletes a request left from before it started: ask once it wrote its first status, as Lua would.
    if (!asked && status.find("steam=ready") != std::string::npos)
      write_file(folder + "\\coop-request.txt",
                 as_host ? std::string("host") : "join " + std::to_string(coop::loop::kLoopLobby)),
          asked = true;
    // A peer that passed the handshake both ways (link_core.h status()).
    for (size_t at = 0; (at = status.find("peer=", at)) != std::string::npos; ++at) {
      const std::string line = status.substr(at, status.find('\n', at) - at);
      linked = linked || (line.find(" accepted=1") != std::string::npos &&
                          line.find(" compatible=1") != std::string::npos);
    }
    // place() writes the host files 00 to 04 in order: the last one there means the set is whole.
    world = !as_host &&
            GetFileAttributesA((saves + "\\" + coop::twins::host_name(4)).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (as_host ? linked : world)
      break;
  }
  std::printf("--- coop-status.txt\n%s--- coop-link.log\n%s", status.c_str(),
              read_file(folder + "\\coop-link.log").c_str());
  if (as_host) {
    std::printf("BOT RESULT: dll linked=%d\n", linked ? 1 : 0);
  } else {
    const std::string expect = read_file(expect_file), slot = read_file(saves + "\\" + coop::twins::host_name(0));
    bool same = world && !expect.empty();
    for (int n = 0; n < coop::twins::kSlots; ++n)
      same = same && read_file(saves + "\\" + coop::twins::host_name(n)) == expect;
    std::printf("BOT RESULT: dll guest linked=%d world_written=%d same_bytes=%d bytes=%zu\n", linked ? 1 : 0,
                world ? 1 : 0, same ? 1 : 0, slot.size());
    linked = same;
  }
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
                    argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 30, argc > 4 ? float(std::atof(argv[4])) : 150.0f,
                    argc > 5 ? std::atoi(argv[5]) : 0, argc > 6 ? argv[6] : "");
  if ((mode == "dll" || mode == "dllguest") && argc >= 4)
    return run_dll(mode == "dll", argv[2], static_cast<uint16_t>(std::atoi(argv[3])),
                   argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 30, argc > 5 ? argv[5] : "");
  std::fputs("usage: loop_bot guest|host PORT [SECONDS] [ECHO_CM] [HIT_HP] [WORLD_SAVE] | "
             "loop_bot dll FOLDER PORT [SECONDS] | loop_bot dllguest FOLDER PORT [SECONDS] EXPECTED_SAVE\n",
             stderr);
  return 2;
}
