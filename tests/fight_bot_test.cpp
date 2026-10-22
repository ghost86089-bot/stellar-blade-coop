// The test bot's shared-fight guest (fight_bot.h) on canned host lines: it answers the host's on once, takes the
// host's world and ups, claims hits that fight_sync.h parses as the host expects (the host's generation, rising claim
// numbers, never more than the HP the host said, lethal on the last), confirms a claim the host's values show, counts
// one they do not, stops at a dead enemy, forgets every enemy at a new world, and counts a line it cannot read.
#include "fight_bot.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace coop::fight;

namespace {
const std::string T = coop::fight_tag() + " ";
Message hit_of(const std::vector<std::string> &lines) {
  assert(lines.size() == 1);
  const Message m = parse(lines[0].data(), lines[0].size());
  assert(m.type == Type::hit);
  return m;
}

// The host's world for the real Host (fight_sync.h), cut down from tests/fight_sync_test.cpp's World: its enemies,
// ApplyStat in whole units with a drop stopping at 0 (a death, after which writes are refused), the effect call, and
// the lines Host sends.
struct World {
  std::vector<Seen> actors;
  std::vector<Message> sent;
  std::vector<Event> events;
  Seen *actor(uint32_t guid) {
    for (Seen &s : actors)
      if (s.guid == guid)
        return &s;
    return nullptr;
  }
  void add(uint32_t guid, uint32_t spawn, uint8_t index, float max_hp) {
    Seen s;
    s.guid = guid, s.spawn = spawn, s.index = index, s.chr = 900;
    s.max[kHp] = max_hp, s.max[kShield] = 100, s.max[kStamina] = 200;
    s.value[kHp] = whole(max_hp), s.value[kShield] = 100, s.value[kStamina] = 200;
    actors.push_back(s);
  }
  bool apply(const Write &w, int32_t &after) {
    Seen *s = actor(w.guid);
    if (!s || s->dying)
      return false;
    int32_t &v = s->value[w.stat];
    v = w.delta < 0 ? std::max(0, v + w.delta) : std::min(v + w.delta, whole(s->max[w.stat]));
    s->dying = w.stat == kHp && v == 0;
    after = v;
    return true;
  }
  bool effect(uint32_t guid, int32_t) { return actor(guid) && !actor(guid)->dying; }
  void send(const Message &m) { sent.push_back(m); }
  void event(const Event &e) { events.push_back(e); }
};
// One turn of the host, its pass as the engine builds it: the GUIDs it follows first, then every other living actor.
void step(Host &host, World &w, uint32_t turn, uint64_t now) {
  uint32_t guids[kMaxTracked];
  const size_t n = host.tracked(guids);
  std::vector<Seen> seen;
  for (size_t i = 0; i < n; ++i)
    if (Seen *s = w.actor(guids[i]))
      seen.push_back(*s);
  for (const Seen &s : w.actors)
    if (!dying(s) && std::find(guids, guids + n, s.guid) == guids + n)
      seen.push_back(s);
  host.step(seen.data(), seen.size(), true, turn, now, w);
}
} // namespace

int main() {
  FightBot bot(500, 1000);
  assert(bot.tick(0).empty()); // Nothing before the host's on and world.

  // The host's on: the bot answers on, once.
  std::vector<std::string> back = bot.receive(T + "on");
  assert(back.size() == 1 && parse(back[0].data(), back[0].size()).type == Type::on);
  assert(bot.receive(T + "on").empty());

  // Its world, one enemy, its first values.
  assert(bot.receive(T + "world 3").empty() && bot.gen() == 3);
  bot.receive(T + "scale 3 135 115");
  bot.receive(T + "up 3 101 7 0 900 1600");
  bot.receive(T + "up 2 999 7 1 900 1600"); // Another world's: not taken.
  assert(bot.enemies() == 1 && bot.counts().ups == 1);
  assert(bot.tick(1000).empty()); // No HP known yet: no claim.
  bot.receive(T + "hp 3 1 0 101:2160:100:200");

  // A claim: the host's generation, claim 1, 500 HP from 2160, not lethal.
  Message h = hit_of(bot.tick(1000));
  assert(h.gen == 3 && h.seq == 1 && h.guid == 101 && h.delta[kHp] == 500 && h.pre_hp == 2160 && !h.lethal);
  assert(bot.tick(1500).empty()); // Not before every_ms, and one claim waiting per enemy.

  // The host's values before its ack, then with it: confirmed.
  bot.receive(T + "hp 3 2 0 101:2160:100:200");
  assert(bot.counts().confirmed == 0);
  bot.receive(T + "hp 3 3 1 101:1660:100:200");
  assert(bot.counts().confirmed == 1 && bot.counts().unconfirmed == 0);

  // Claim 2 is acknowledged but never shows in three lines: unconfirmed once.
  h = hit_of(bot.tick(2000));
  assert(h.seq == 2 && h.pre_hp == 1660);
  for (int i = 0; i < 3; ++i)
    bot.receive(T + "hp 3 " + std::to_string(4 + i) + " 2 101:1660:100:200");
  assert(bot.counts().unconfirmed == 1 && bot.counts().confirmed == 1);

  // The last 300 HP: a lethal claim of exactly what is left; the host holds the kill at 0, then says dead.
  bot.receive(T + "hp 3 7 2 101:300:100:200");
  h = hit_of(bot.tick(3000));
  assert(h.seq == 3 && h.delta[kHp] == 300 && h.pre_hp == 300 && h.lethal);
  bot.receive(T + "hp 3 8 3 101:0:100:200");
  assert(bot.counts().confirmed == 2);
  bot.receive(T + "dead 3 101");
  assert(bot.counts().deads == 1 && bot.tick(9000).empty()); // Dead: no more claims.

  // A new world drops every enemy; a line the bot cannot read is counted.
  bot.receive(T + "up 3 102 8 0 900 1600");
  bot.receive(T + "world 4");
  assert(bot.enemies() == 0 && bot.tick(20000).empty());
  bot.receive("garbage");
  assert(bot.counts().bad == 1 && bot.counts().hits == 3);

  // Against the real Host, through channel 2's text both ways: the bot claims 400 HP every 300 ms on the host's one
  // enemy (1600 HP). The host takes every claim (no bad line, none stale, none over the rates), its enemy loses
  // exactly what the bot claimed, the bot confirms each one, and its last, lethal claim kills it once.
  {
    World H;
    H.add(100, 7000, 0, 1600);
    Host host;
    FightBot b(400, 300);
    std::vector<std::string> to_host;
    host.receive(parse((T + "on").data(), (T + "on").size()), 0, H); // The host's own on reaches it from its worker.
    for (const std::string &l : b.receive(T + "on"))
      to_host.push_back(l);
    for (uint64_t t = 0, turn = 1; t <= 4000; t += 16, ++turn) {
      for (const std::string &l : std::exchange(to_host, {}))
        host.receive(parse(l.data(), l.size()), t, H);
      step(host, H, uint32_t(turn), t);
      for (const Message &m : std::exchange(H.sent, {}))
        for (const std::string &l : b.receive(format(m)))
          to_host.push_back(l);
      for (const std::string &l : b.tick(t))
        to_host.push_back(l);
    }
    const FightBot::Counts &c = b.counts();
    std::printf("fight_bot_test: against the real Host: %llu claims, %llu confirmed, host took %llu, enemy hp %d, %llu "
                "dead\n",
                static_cast<unsigned long long>(c.hits), static_cast<unsigned long long>(c.confirmed),
                static_cast<unsigned long long>(host.counts.claims), H.actor(100)->value[kHp],
                static_cast<unsigned long long>(c.deads));
    std::printf("  host: bad %llu stale %llu rate %llu refused %llu kills %llu; bot: unconfirmed %llu bad %llu ups %llu\n",
                static_cast<unsigned long long>(host.counts.bad), static_cast<unsigned long long>(host.counts.stale),
                static_cast<unsigned long long>(host.counts.rate), static_cast<unsigned long long>(host.counts.refused),
                static_cast<unsigned long long>(host.counts.kills), static_cast<unsigned long long>(c.unconfirmed),
                static_cast<unsigned long long>(c.bad), static_cast<unsigned long long>(c.ups));
    std::fflush(stdout);
    assert(host.counts.bad == 0 && host.counts.stale == 0 && host.counts.rate == 0 && host.counts.refused == 0);
    assert(c.hits == 4 && host.counts.claims == 4 && c.confirmed == 4 && c.unconfirmed == 0 && c.bad == 0);
    assert(H.actor(100)->dying && H.actor(100)->value[kHp] == 0 && c.deads == 1 && host.counts.kills == 1);
  }
  std::puts("fight_bot_test: ok");
}
