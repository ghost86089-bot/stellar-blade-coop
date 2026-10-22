// The test bot's side of the shared fight (native/bot/loop_bot.cpp): a guest at the level of channel 2's lines
// (fight_sync.h), for one PC with one game. The bot has no world, so it takes every enemy the host announces (up) as
// paired, claims a hit on one of them every every_ms (rising claim numbers, the host's world generation, never more
// than the HP the host last said), and checks the host's hp lines: once the host acknowledges a claim, its values must
// show the drop within kConfirmLines lines. So the host's side of the shared fight is tested against real enemies in
// a real game; a real guest's engine (pairing its own copies, corrections, crowd control) is not. Free of Windows:
// tests/fight_bot_test.cpp checks it on canned host lines.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "fight_sync.h"

namespace coop::fight {

class FightBot {
public:
  static constexpr int kConfirmLines = 3; // hp lines after the ack in which a claim must show.

  struct Counts {
    uint64_t ups = 0, hp_lines = 0, hits = 0, confirmed = 0, unconfirmed = 0, deads = 0, bad = 0;
  };

  FightBot(int32_t hit_hp, uint64_t every_ms) : hit_hp_(std::max<int32_t>(1, hit_hp)), every_ms_(every_ms) {}

  // A line the host sent on channel 2: the lines to send back (reliable), if any.
  std::vector<std::string> receive(const std::string &text) {
    std::vector<std::string> out;
    const Message m = parse(text.data(), text.size());
    switch (m.type) {
    case Type::on: // The host's feature is on: ours too, which also makes it announce its whole list to us.
      if (!on_sent_) {
        on_sent_ = true;
        Message on;
        on.type = Type::on;
        out.push_back(format(on));
      }
      break;
    case Type::off: on_sent_ = false, gen_ = 0, enemies_.clear(); break;
    case Type::world: gen_ = m.gen, enemies_.clear(); break; // Every pair goes with the host's world.
    case Type::up:
      if (m.gen == gen_ && gen_) {
        Enemy &e = enemies_[m.guid];
        e = Enemy{};
        e.spawn = m.spawn, e.index = m.index, e.chr = m.chr, e.max_hp = m.max_hp;
        ++counts_.ups;
      }
      break;
    case Type::down:
      if (m.gen == gen_)
        enemies_.erase(m.guid);
      break;
    case Type::dead:
      if (m.gen == gen_)
        if (auto it = enemies_.find(m.guid); it != enemies_.end() && !it->second.dead) {
          // Its death settles a claim still waiting: 0 HP is at most anything we expected. The host lists a dead
          // enemy in no more hp lines, so a lethal claim is only ever confirmed here.
          if (it->second.pending)
            ++counts_.confirmed, it->second.pending = 0;
          it->second.dead = true, ++counts_.deads;
        }
      break;
    case Type::hp:
      if (m.gen == gen_ && gen_) {
        ++counts_.hp_lines;
        for (size_t i = 0; i < m.count && i < kHpPerMessage; ++i)
          if (auto it = enemies_.find(m.entries[i].guid); it != enemies_.end())
            it->second.hp = m.entries[i].value[kHp];
        for (auto &[guid, e] : enemies_)
          check(e, m.seq);
      }
      break;
    case Type::scale:
    case Type::place:
    case Type::aggro:
    case Type::cc:
    case Type::own:
    case Type::epoch:
    case Type::spawn:
    case Type::drop: break; // The host's settings and later stages: nothing for a guest without a world.
    default: ++counts_.bad; break;
    }
    return out;
  }

  // Each pass: the claim that is due, if any. One enemy at a time, the first the host said is alive with HP left.
  std::vector<std::string> tick(uint64_t now_ms) {
    std::vector<std::string> out;
    if (!on_sent_ || !gen_ || now_ms < next_hit_)
      return out;
    for (auto &[guid, e] : enemies_) {
      if (e.dead || e.hp <= 0 || e.pending)
        continue;
      Message h;
      h.type = Type::hit;
      h.gen = gen_;
      h.seq = ++seq_;
      h.guid = guid;
      h.delta[kHp] = std::min(hit_hp_, e.hp);
      h.pre_hp = e.hp;
      h.lethal = h.delta[kHp] >= e.hp;
      e.pending = h.seq, e.expect = e.hp - h.delta[kHp], e.lines_after_ack = 0;
      ++counts_.hits;
      out.push_back(format(h));
      next_hit_ = now_ms + every_ms_;
      break;
    }
    return out;
  }

  const Counts &counts() const { return counts_; }
  uint32_t gen() const { return gen_; }
  size_t enemies() const { return enemies_.size(); }

private:
  struct Enemy {
    uint32_t spawn = 0, chr = 0;
    uint8_t index = 0;
    int32_t max_hp = 0, hp = -1; // hp: the host's last value, -1 before its first hp line.
    bool dead = false;
    uint32_t pending = 0;        // The claim waiting for the host's values to show it.
    int32_t expect = 0;          // The HP the host must show at most once it took that claim.
    int lines_after_ack = 0;
  };
  // After the ack covers the claim: its drop must show (the host may have hit too: at most expect), or a kill may
  // hold at 0 (Host::apply); kConfirmLines lines without it count once as unconfirmed.
  void check(Enemy &e, uint32_t ack) {
    if (!e.pending || ack < e.pending)
      return;
    if (e.hp <= e.expect) {
      ++counts_.confirmed, e.pending = 0;
    } else if (++e.lines_after_ack >= kConfirmLines) {
      ++counts_.unconfirmed, e.pending = 0;
    }
  }

  int32_t hit_hp_;
  uint64_t every_ms_, next_hit_ = 0;
  bool on_sent_ = false;
  uint32_t gen_ = 0, seq_ = 0;
  std::map<uint32_t, Enemy> enemies_;
  Counts counts_;
};

} // namespace coop::fight
