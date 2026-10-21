// The test link: a stand-in for the Steam API the link uses, for two players on one PC (the game and a bot, or two
// bots), over datagrams the caller carries (loopback UDP in steam_link.cpp and native/bot). Test-only: steam_link.cpp
// uses it only when STELLARCOOP_LOOP is set, and players never set it. It never talks to Steam.
//
// Roles are fixed: the host owns the only lobby, kLoopLobby, and keeps its members, owner and data; the guest mirrors
// them. Datagrams: 'J' join (guest to host), 'L' leave, 'S' lobby state (host to guest, on each change and every
// second), 'M' a message (channel, fragment index and count, then its bytes). Messages are delivered whether or not
// the session was accepted, like tests/link_core_test.cpp's fake; the first one from a peer also raises a session
// request, which the caller passes to Link::tick as steam_link.cpp passes Steam's callback.
// shortcut: no resend, so a datagram lost on loopback (a full socket buffer) loses a reliable message; add acks if a
// test ever sees it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "link_core.h"

namespace coop::loop {

constexpr uint64_t kLoopLobby = 0x0186000000000777ull;
constexpr uint64_t kHostId = 0x0110000100000001ull, kGuestId = 0x0110000100000002ull; // Never real accounts.
constexpr uint32_t kLoopBuild = 24463856;    // The one build StellarCoop pins.
constexpr size_t kFragment = 60000;          // Under loopback UDP's 65507-byte limit.
constexpr uint64_t kJoinTimeoutMs = 5000, kStateEveryMs = 1000;

class Steam {
public:
  // out: one datagram to the other side.
  Steam(bool host, std::function<void(const std::string &)> out) : host_(host), out_(std::move(out)) {}

  bool host() const { return host_; }
  uint64_t me() const { return host_ ? kHostId : kGuestId; }
  uint64_t other() const { return host_ ? kGuestId : kHostId; }

  // One datagram from the other side.
  void on_datagram(const std::string &d) {
    if (d.empty())
      return;
    heard_ = true;
    switch (d[0]) {
    case 'J':
      if (host_ && created_ && members_.size() < 2 && !member(other()))
        members_.push_back(other()), send_state();
      else if (host_ && member(other()))
        send_state(); // A repeated join: she missed the state.
      break;
    case 'L':
      if (host_)
        drop(other()), send_state();
      break;
    case 'S':
      if (!host_)
        read_state(d.substr(1));
      break;
    case 'M':
      on_fragment(d);
      break;
    }
  }

  // Each worker pass, before Link::tick: completes calls, repeats joins, sends the host's state each second.
  void pump(uint64_t now_ms) {
    now_ = now_ms;
    for (auto &[id, c] : calls_) {
      if (c.done)
        continue;
      if (c.id == kCreatedId) {
        created_ = true;
        members_ = {me()};
        owner_ = me();
        store(c, LobbyCreated{kResultOk, kLoopLobby});
        c.done = true;
        send_state();
      } else if (c.id == kEnterId) {
        if (member(me()) && owner_ == kHostId) {
          store(c, LobbyEnter{kLoopLobby, 0, false, kEnterSuccess});
          c.done = true;
        } else if (now_ms - c.started >= kJoinTimeoutMs) {
          store(c, LobbyEnter{kLoopLobby, 0, false, 2}); // DoesntExist: no host answered.
          c.done = true;
        } else if (now_ms >= c.next_try) {
          out_("J");
          c.next_try = now_ms + 250;
        }
      }
    }
    if (host_ && created_ && now_ms >= next_state_)
      send_state();
  }

  // The session request a new peer's first message raised, or 0.
  uint64_t take_session() { return std::exchange(session_, 0); }

  // ---- What the Api table calls ----
  uint64_t create_lobby() {
    if (!host_)
      return 0; // The guest never hosts here.
    return new_call(kCreatedId);
  }
  uint64_t join_lobby(uint64_t lobby) {
    if (host_ || lobby != kLoopLobby)
      return 0;
    return new_call(kEnterId);
  }
  void leave_lobby() {
    if (host_) {
      created_ = false;
      members_.clear();
      data_.clear();
      send_state();
    } else {
      members_.clear();
      out_("L");
    }
  }
  bool call_done(uint64_t call) const {
    auto it = calls_.find(call);
    return it != calls_.end() && it->second.done;
  }
  bool call_result(uint64_t call, void *out, int size, int id) const {
    auto it = calls_.find(call);
    if (it == calls_.end() || !it->second.done || it->second.id != id ||
        it->second.result.size() != static_cast<size_t>(size))
      return false;
    std::memcpy(out, it->second.result.data(), it->second.result.size());
    return true;
  }
  bool set_data(const char *k, const char *v) {
    if (!host_ || !created_)
      return false; // Steam lets only the owner write lobby data.
    data_[k] = v;
    send_state();
    return true;
  }
  const char *data(const char *k) const {
    auto it = data_.find(k);
    return it == data_.end() ? "" : it->second.c_str();
  }
  uint64_t owner() const { return members_.empty() ? 0 : owner_; }
  int member_count() const { return static_cast<int>(members_.size()); }
  uint64_t member_at(int i) const {
    return i >= 0 && static_cast<size_t>(i) < members_.size() ? members_[static_cast<size_t>(i)] : 0;
  }
  int send(uint64_t to, const void *data, uint32_t size, int channel) {
    if (to != other() || channel < 0 || channel > 255)
      return 3; // k_EResultNoConnection
    const auto *bytes = static_cast<const char *>(data);
    const uint32_t count = size ? static_cast<uint32_t>((size + kFragment - 1) / kFragment) : 1;
    const uint32_t id = ++message_id_;
    for (uint32_t i = 0; i < count; ++i) {
      std::string d = "M";
      d += static_cast<char>(channel);
      put(d, id), put(d, i), put(d, count);
      const size_t at = static_cast<size_t>(i) * kFragment;
      d.append(bytes + at, std::min<size_t>(kFragment, size - at));
      out_(d);
    }
    return kResultOk;
  }
  // The next whole message on a channel: its bytes, or false.
  bool receive(int channel, std::string &bytes) {
    auto &box = inbox_[channel];
    if (box.empty())
      return false;
    bytes = std::move(box.front());
    box.erase(box.begin());
    return true;
  }
  bool heard() const { return heard_; }

private:
  struct Call {
    int id = 0;
    bool done = false;
    uint64_t started = 0, next_try = 0;
    std::vector<unsigned char> result;
  };
  template <class T> static void store(Call &c, const T &v) {
    c.result.resize(sizeof v);
    std::memcpy(c.result.data(), &v, sizeof v);
  }
  static void put(std::string &d, uint32_t v) { d.append(reinterpret_cast<const char *>(&v), 4); }
  static uint32_t get(const std::string &d, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  uint64_t new_call(int id) {
    const uint64_t call = next_call_++;
    calls_[call] = Call{id, false, now_, now_, {}};
    return call;
  }
  bool member(uint64_t id) const {
    for (auto m : members_)
      if (m == id)
        return true;
    return false;
  }
  void drop(uint64_t id) {
    std::vector<uint64_t> kept;
    for (auto m : members_)
      if (m != id)
        kept.push_back(m);
    members_ = kept;
  }
  void send_state() {
    next_state_ = now_ + kStateEveryMs;
    std::string s = "S" + std::to_string(created_ ? owner_ : 0) + "\n";
    for (auto m : members_)
      s += "m" + std::to_string(m) + "\n";
    for (auto &[k, v] : data_)
      if (k.find('=') == std::string::npos && k.find('\n') == std::string::npos && v.find('\n') == std::string::npos)
        s += "d" + k + "=" + v + "\n";
    out_(s);
  }
  void read_state(const std::string &s) {
    std::vector<uint64_t> members;
    std::map<std::string, std::string> data;
    uint64_t owner = 0;
    size_t at = 0;
    for (bool first = true; at < s.size(); first = false) {
      size_t end = s.find('\n', at);
      if (end == std::string::npos)
        end = s.size();
      const std::string line = s.substr(at, end - at);
      at = end + 1;
      if (first)
        owner = std::strtoull(line.c_str(), nullptr, 10);
      else if (!line.empty() && line[0] == 'm')
        members.push_back(std::strtoull(line.c_str() + 1, nullptr, 10));
      else if (!line.empty() && line[0] == 'd' && line.find('=') != std::string::npos)
        data[line.substr(1, line.find('=') - 1)] = line.substr(line.find('=') + 1);
    }
    owner_ = owner;
    data_ = data;
    // The host's members, but only while we asked to be in (a stale state after our leave adds nothing back).
    bool asked = false;
    for (auto &[id, c] : calls_)
      asked = asked || c.id == kEnterId;
    members_ = asked && owner ? members : std::vector<uint64_t>{};
    if (!member(me()))
      members_.clear();
  }
  void on_fragment(const std::string &d) {
    if (d.size() < 14)
      return;
    const int channel = static_cast<unsigned char>(d[1]);
    const uint32_t id = get(d, 2), index = get(d, 6), count = get(d, 10);
    if (!count || index >= count || count > 64)
      return;
    auto &p = partial_[id];
    if (p.parts.empty())
      p.parts.resize(count), p.channel = channel;
    if (p.parts.size() != count || p.channel != channel)
      return;
    if (p.parts[index].empty() && !p.got[index]) {
      p.parts[index] = d.substr(14);
      p.got[index] = true;
      ++p.have;
    }
    if (p.have < count)
      return;
    std::string whole;
    for (auto &part : p.parts)
      whole += part;
    partial_.erase(id);
    inbox_[channel].push_back(std::move(whole));
    if (!accepted_once_) {
      accepted_once_ = true;
      session_ = other();
    }
  }

  struct Partial {
    std::vector<std::string> parts;
    std::map<uint32_t, bool> got;
    uint32_t have = 0;
    int channel = 0;
  };
  bool host_;
  std::function<void(const std::string &)> out_;
  uint64_t now_ = 0, next_state_ = 0, next_call_ = 1, owner_ = 0, session_ = 0;
  uint32_t message_id_ = 0;
  bool created_ = false, heard_ = false, accepted_once_ = false;
  std::vector<uint64_t> members_;
  std::map<std::string, std::string> data_;
  std::map<uint64_t, Call> calls_;
  std::map<int, std::vector<std::string>> inbox_;
  std::map<uint32_t, Partial> partial_;
};

// The Api table on Steam instance N (captureless lambdas: the table holds plain function pointers).
inline Steam *instance[2] = {nullptr, nullptr};
inline int dummy_interface;

template <int N> Api api() {
  Api a{};
  a.user_handle = [] { return instance[N] ? 1 : 0; };
  a.release_memory = [] {};
  a.register_callback = [](void *, int) {};
  a.user = a.friends = a.utils = a.matchmaking = a.messages = a.net_utils = a.apps = [] {
    return static_cast<void *>(&dummy_interface);
  };
  a.steam_id = [](void *) { return instance[N]->me(); };
  a.app_id = [](void *) { return 3489700u; };
  a.overlay = [](void *) { return true; };
  a.build_id = [](void *) { return static_cast<int>(kLoopBuild); };
  a.persona = [](void *) -> const char * { return instance[N]->host() ? "Loop host" : "Loop guest"; };
  a.friend_persona = [](void *, uint64_t id) -> const char * {
    return id == kHostId ? "Loop host" : id == kGuestId ? "Loop guest" : "";
  };
  a.friend_count = [](void *, int) { return 0; };
  a.friend_by_index = [](void *, int, int) { return uint64_t{0}; };
  a.friend_game = [](void *, uint64_t, FriendGame *) { return false; };
  a.has_friend = [](void *, uint64_t id, int) { return id == instance[N]->other(); };
  a.invite_dialog = [](void *, uint64_t) {};
  a.call_done = [](void *, uint64_t call, bool *failed) {
    *failed = false;
    return instance[N]->call_done(call);
  };
  a.call_result = [](void *, uint64_t call, void *out, int size, int id, bool *failed) {
    const bool ok = instance[N]->call_result(call, out, size, id);
    *failed = !ok;
    return ok;
  };
  a.create_lobby = [](void *, int, int) { return instance[N]->create_lobby(); };
  a.join_lobby = [](void *, uint64_t lobby) { return instance[N]->join_lobby(lobby); };
  a.leave_lobby = [](void *, uint64_t) { instance[N]->leave_lobby(); };
  a.set_lobby_data = [](void *, uint64_t, const char *k, const char *v) { return instance[N]->set_data(k, v); };
  a.lobby_data = [](void *, uint64_t, const char *k) { return instance[N]->data(k); };
  a.lobby_owner = [](void *, uint64_t) { return instance[N]->owner(); };
  a.member_count = [](void *, uint64_t) { return instance[N]->member_count(); };
  a.member = [](void *, uint64_t, int i) { return instance[N]->member_at(i); };
  a.send = [](void *, const Identity *who, const void *data, uint32_t size, int, int channel) {
    return instance[N]->send(steam_id_of(*who), data, size, channel);
  };
  a.receive = [](void *, int channel, NetMessage **out, int max) {
    int n = 0;
    std::string bytes;
    while (n < max && instance[N]->receive(channel, bytes)) {
      auto *copy = new std::string(std::move(bytes));
      auto *m = new NetMessage{};
      m->data = copy->data();
      m->size = static_cast<int32_t>(copy->size());
      m->peer.type = kIdentitySteamId;
      const uint64_t from = instance[N]->other();
      std::memcpy(m->peer.data, &from, 8);
      std::memcpy(m->peer.data + 8, &copy, sizeof copy); // Kept alive until release_message.
      out[n++] = m;
    }
    return n;
  };
  a.accept_session = [](void *, const Identity *) { return true; };
  a.close_session = [](void *, const Identity *) { return true; };
  a.session_info = [](void *, const Identity *, void *, void *quick) {
    std::memset(quick, 0, 64);
    const int32_t state = 3, ping = 1; // Connected, 1 ms.
    std::memcpy(quick, &state, 4);
    std::memcpy(static_cast<char *>(quick) + 4, &ping, 4);
    return 3;
  };
  a.release_message = [](NetMessage *m) {
    std::string *copy = nullptr;
    std::memcpy(&copy, m->peer.data + 8, sizeof copy);
    delete copy;
    delete m;
  };
  a.init_relay = [](void *) {};
  a.relay_status = [](void *, void *) { return 100; }; // k_ESteamNetworkingAvailability_Current
  a.set_steam_id = [](Identity *who, uint64_t id) {
    who->type = kIdentitySteamId;
    who->size = 8;
    std::memcpy(who->data, &id, 8);
  };
  a.get_steam_id = [](const Identity *who) { return steam_id_of(*who); };
  return a;
}

} // namespace coop::loop
