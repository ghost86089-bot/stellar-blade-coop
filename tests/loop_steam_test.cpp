// The test link (loop_steam.h): two Links, a host and a guest, each on its own stand-in Steam, joined by in-memory
// datagram pipes: hosting, a join by the loop lobby's number, the handshake both ways, channel 1 with a table split
// into fragments, the guest leaving and the host leaving; and the join's failure when no host state arrives.
#include "loop_steam.h"

#include <cassert>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace coop;

namespace {
std::deque<std::string> to_host, to_guest; // Datagrams in flight each way.
bool drop_state = false;                     // The twin: every lobby state the host sends is lost.
std::vector<std::string> events[2], received[2];

void dump() {
  for (int side = 0; side < 2; ++side)
    for (auto &e : events[side])
      std::printf("%s: %s\n", side ? "guest" : "host", e.c_str());
}

struct Pair {
  loop::Steam host{true, [](const std::string &d) {
                     if (!(drop_state && d[0] == 'S'))
                       to_guest.push_back(d);
                   }};
  loop::Steam guest{false, [](const std::string &d) { to_host.push_back(d); }};
  Api api[2];
  Link a, b;
  uint64_t now = 1000;
  Pair()
      : api{(loop::instance[0] = &host, loop::api<0>()), (loop::instance[1] = &guest, loop::api<1>())},
        a(api[0], "v-loop", [](const std::string &e) { events[0].push_back(e); },
          [](const uint8_t *d, size_t n) { received[0].emplace_back(reinterpret_cast<const char *>(d), n); }),
        b(api[1], "v-loop", [](const std::string &e) { events[1].push_back(e); },
          [](const uint8_t *d, size_t n) { received[1].emplace_back(reinterpret_cast<const char *>(d), n); }) {
    a.world(true);
    b.world(true);
    assert(a.ready(L"SB.exe") == Link::Ready::now && b.ready(L"SB.exe") == Link::Ready::now);
  }
  // One pass on each side: deliver what is in flight, pump, tick.
  void round(const std::string &host_request = "", const std::string &guest_request = "") {
    now += 50;
    while (!to_host.empty())
      host.on_datagram(to_host.front()), to_host.pop_front();
    host.pump(now);
    a.tick(now, host_request, 0, host.take_session());
    while (!to_guest.empty())
      guest.on_datagram(to_guest.front()), to_guest.pop_front();
    guest.pump(now);
    b.tick(now, guest_request, 0, guest.take_session());
  }
  bool run_until(bool (*done)(Pair &), int rounds) {
    for (int i = 0; i < rounds && !done(*this); ++i)
      round();
    return done(*this);
  }
};

void reset() {
  to_host.clear(), to_guest.clear();
  drop_state = false;
  for (int i = 0; i < 2; ++i)
    events[i].clear(), received[i].clear();
}
} // namespace

int main() {
  // 1. Host, join by number, both linked within 10 s of passes.
  {
    reset();
    Pair p;
    p.round("host");
    p.round("", "join " + std::to_string(loop::kLoopLobby));
    const bool linked = p.run_until([](Pair &q) { return q.a.linked() && q.b.linked(); }, 200);
    if (!linked)
      dump();
    assert(linked && p.a.host() && !p.b.host());
    assert(p.host.member_count() == 2 && p.guest.member_count() == 2 && p.guest.owner() == loop::kHostId);

    // 2. Channel 1: a whole table (65536 bytes, two fragments) arrives once and intact; a small pose too.
    std::string table(kMaxBinary, '\0');
    for (size_t i = 0; i < table.size(); ++i)
      table[i] = static_cast<char>(i * 31 + 7);
    p.a.send_binary(reinterpret_cast<const uint8_t *>(table.data()), table.size(), true);
    p.b.send_binary(reinterpret_cast<const uint8_t *>("pose"), 4, false);
    p.round(), p.round();
    assert(received[1].size() == 1 && received[1][0] == table);
    assert(received[0].size() == 1 && received[0][0] == "pose");

    // 3. The guest leaves: the host's lobby has only her, and she is no longer linked.
    p.round("", "leave");
    assert(p.run_until([](Pair &q) { return !q.a.linked() && q.host.member_count() == 1; }, 40));
    assert(!p.b.linked() && p.guest.member_count() == 0);

    // 4. She joins again, then the host leaves: the guest's mirror empties and she is not linked.
    p.round("", "join " + std::to_string(loop::kLoopLobby));
    assert(p.run_until([](Pair &q) { return q.a.linked() && q.b.linked(); }, 200));
    p.round("leave");
    assert(p.run_until([](Pair &q) { return !q.b.linked() && !q.a.linked(); }, 400));
  }
  // 5. Twin: with every lobby state lost, the join times out (DoesntExist) and nobody is linked.
  {
    reset();
    drop_state = true;
    Pair p;
    p.round("host");
    p.round("", "join " + std::to_string(loop::kLoopLobby));
    p.run_until([](Pair &) { return false; }, 140); // 7 s of passes, past the 5 s join timeout.
    assert(!p.a.linked() && !p.b.linked() && p.guest.member_count() == 0);
  }
  std::puts("loop_steam_test: ok");
}
