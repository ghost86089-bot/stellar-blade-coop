// The host's world for the guest (docs/CAMPAIGN.md, step 4): at join, the guest's game gets a copy of the save the
// host plays, so both worlds start the same. Free of Windows: tests/host_world_test.cpp checks it on the build machine.
//
// On channel 1 (link_core.h's binary messages, next to the pose path's, told apart by their first three bytes):
//   "SBQ" protocol(u8) id(u32)                                the guest asks for the host's world; id: her request
//   "SBW" protocol(u8) id(u32) total(u32) crc(u32) index(u16) count(u16) slot(u8), then the bytes
//                                                             one chunk of the host's save: of her request id, total
//                                                             bytes, the CRC-32 of all of them, chunk index of count
//                                                             (each kChunk bytes but the last), the host's slot 0-4
// The host reads its loaded co-op save whole (save_twins.h's checks) and sends it kChunkEveryMs apart, under the link's
// rate limit for one member (kBinaryPerSecond, kBinaryBytesPerSecond) with room for the poses. The guest keeps one
// transfer, the latest id she asked for, checks the CRC and then the game's own load check, and only then writes.
//
// Where it goes (place): the guest's host set, StellarBladeHost00-04.sav (save_twins.h's host_name), every one of the
// five holding it, each written beside and renamed over as the co-op copies are and read back; a failure removes all
// five, so no slot ever holds an older world next to the new one. These files are the mod's own: her saves and her
// co-op copies are never written. The guest's game then switches to that set at the title (save_twins.h's to_host):
// its slot list is her own saves', so Continue and Load Game load whichever slot they pick, of the host's world.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "link_protocol.h"
#include "save_twins.h"

namespace coop::world {

constexpr size_t kChunk = 60000, kHeader = 21, kRequestSize = 8;
constexpr uint64_t kChunkEveryMs = 125; // 8 a second, 480 KB/s: under the link's 160 messages and 1 MB a second.
constexpr size_t kMaxChunks = (twins::kMaxSave + kChunk - 1) / kChunk;

inline void put(std::string &s, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i)
    s += char(v >> (8 * i) & 0xff);
}
inline uint64_t get(const uint8_t *p, int bytes) {
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i)
    v = v << 8 | p[i];
  return v;
}

inline std::string request(uint32_t id) {
  std::string s = "SBQ";
  put(s, kProtocol, 1), put(s, id, 4);
  return s;
}
inline bool parse_request(const uint8_t *d, size_t n, uint32_t &id) {
  if (n != kRequestSize || std::memcmp(d, "SBQ", 3) || d[3] != kProtocol)
    return false;
  id = uint32_t(get(d + 4, 4));
  return true;
}

struct Chunk {
  uint32_t id = 0, total = 0, crc = 0;
  uint16_t index = 0, count = 0;
  uint8_t slot = 0;
  const uint8_t *data = nullptr;
  size_t size = 0;
};
inline uint16_t chunks_for(size_t total) { return uint16_t((total + kChunk - 1) / kChunk); }
// Chunk index of a save's bytes, or empty when index is past its last.
inline std::string chunk(uint32_t id, const std::string &bytes, uint32_t crc, uint16_t index, uint8_t slot) {
  const uint16_t count = chunks_for(bytes.size());
  if (index >= count)
    return {};
  std::string s = "SBW";
  put(s, kProtocol, 1), put(s, id, 4), put(s, bytes.size(), 4), put(s, crc, 4), put(s, index, 2), put(s, count, 2);
  put(s, slot, 1);
  const size_t at = size_t(index) * kChunk;
  s.append(bytes, at, (std::min)(kChunk, bytes.size() - at)); // (std::min): windows.h's min macro may be defined.
  return s;
}
// A chunk whose header agrees with itself: a save size the game could write, its chunk count, an index within it, a
// slot 0-4, and exactly the bytes that index holds.
inline bool parse_chunk(const uint8_t *d, size_t n, Chunk &c) {
  if (n < kHeader || std::memcmp(d, "SBW", 3) || d[3] != kProtocol)
    return false;
  c.id = uint32_t(get(d + 4, 4)), c.total = uint32_t(get(d + 8, 4)), c.crc = uint32_t(get(d + 12, 4));
  c.index = uint16_t(get(d + 16, 2)), c.count = uint16_t(get(d + 18, 2)), c.slot = d[20];
  c.data = d + kHeader, c.size = n - kHeader;
  if (c.total == 0 || c.total > twins::kMaxSave || c.count != chunks_for(c.total) || c.index >= c.count ||
      c.slot >= twins::kSlots)
    return false;
  const size_t want = c.index + 1 < c.count ? kChunk : c.total - size_t(c.index) * kChunk;
  return c.size == want;
}

// The host's side of one transfer: its chunks, one each kChunkEveryMs.
class Sender {
public:
  void start(uint32_t id, std::string bytes, uint8_t slot) {
    id_ = id, bytes_ = std::move(bytes), slot_ = slot, crc_ = twins::crc32(bytes_.data(), bytes_.size());
    next_ = 0, count_ = chunks_for(bytes_.size()), due_ = 0;
  }
  void stop() { next_ = count_; }
  bool busy() const { return next_ < count_; }
  // The next chunk once it is due, else empty.
  std::string next(uint64_t now_ms) {
    if (!busy() || now_ms < due_)
      return {};
    due_ = now_ms + kChunkEveryMs;
    return chunk(id_, bytes_, crc_, next_++, slot_);
  }
  uint16_t sent() const { return next_; }
  uint16_t count() const { return count_; }

private:
  std::string bytes_;
  uint32_t id_ = 0, crc_ = 0;
  uint16_t next_ = 0, count_ = 0;
  uint8_t slot_ = 0;
  uint64_t due_ = 0;
};

// The guest's side: the chunks of the transfer she asked for last, in any order, each once.
class Receiver {
public:
  enum class Result { ignored, more, done, bad };
  void start(uint32_t id) { id_ = id, active_ = true, got_ = 0, bytes_.clear(), have_.clear(), why_.clear(); }
  bool active() const { return active_; }
  // A chunk: ignored (not of her request, or one she has), more, done (whole, the CRC and the game's own check pass),
  // or bad (a chunk that disagrees with the first, or a whole that fails a check: the transfer ends).
  Result take(const Chunk &c) {
    if (!active_ || c.id != id_)
      return Result::ignored;
    if (have_.empty()) {
      total_ = c.total, crc_ = c.crc, count_ = c.count, slot_ = c.slot;
      bytes_.assign(total_, '\0');
      have_.assign(count_, false);
    } else if (c.total != total_ || c.crc != crc_ || c.count != count_ || c.slot != slot_) {
      return end("a chunk disagrees with the first one");
    }
    if (have_[c.index])
      return Result::ignored;
    std::memcpy(&bytes_[size_t(c.index) * kChunk], c.data, c.size);
    have_[c.index] = true;
    if (++got_ < count_)
      return Result::more;
    active_ = false;
    if (twins::crc32(bytes_.data(), bytes_.size()) != crc_)
      return end("its CRC-32 does not match");
    if (!twins::game_save(bytes_))
      return end("it fails the game's own load check");
    return Result::done;
  }
  const std::string &bytes() const { return bytes_; }
  uint8_t host_slot() const { return slot_; }
  uint16_t got() const { return got_; }
  uint16_t count() const { return count_; }
  const std::string &why() const { return why_; }

private:
  Result end(const char *why) {
    active_ = false, why_ = why, bytes_.clear();
    return Result::bad;
  }
  bool active_ = false;
  uint32_t id_ = 0, total_ = 0, crc_ = 0;
  uint16_t got_ = 0, count_ = 0;
  uint8_t slot_ = 0;
  std::string bytes_, why_;
  std::vector<bool> have_;
};

// A save of the account read whole between two equal stats (the game may be writing it), then the game's own load check
// (save_twins.h's reading, once more on a torn read): empty, or why not.
template <class Fs> std::string read_save(Fs &fs, const std::string &name, std::string &bytes) {
  std::string why;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const twins::Stat s = fs.stat(name);
    why = !s.exists                                                ? "is missing"
          : s.size > twins::kMaxSave                               ? "is over 64 MB"
          : !fs.read(name, bytes, twins::kMaxSave)                 ? "is unreadable"
          : bytes.size() != s.size || !twins::same(fs.stat(name), s) ? "changed while it was read"
          : !twins::game_save(bytes)                               ? "fails the game's own load check"
                                                                   : "";
    if (why.empty())
      break;
  }
  return why;
}

// The host's world into the guest's five host files: empty on success, else why not (no host file left then). Fs is
// save_twins.h's.
template <class Fs> std::string place(Fs &fs, const std::string &bytes) {
  using namespace twins;
  if (bytes.size() > kMaxSave || !game_save(bytes))
    return "the host's save fails the game's own load check";
  for (int n = 0; n < kSlots; ++n) {
    const std::string name = host_name(n), temp = name + kTemp;
    std::string back;
    if (!writable(temp) || !writable(name) || !fs.write(temp, bytes) || !fs.rename(temp, name) ||
        !fs.read(name, back, kMaxSave) || back != bytes) {
      for (int k = 0; k < kSlots; ++k)
        fs.remove(host_name(k) + kTemp), fs.remove(host_name(k));
      return host_name(n) + " could not be written";
    }
  }
  return {};
}

} // namespace coop::world
