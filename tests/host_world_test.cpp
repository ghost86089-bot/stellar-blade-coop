// The host's world for the guest (host_world.h): the request and chunk messages and their refusals, a transfer
// reassembled in any order with duplicates, chunks that disagree, a whole that fails its CRC or the game's own check,
// the sender's pacing; and place() on an in-memory account folder that enforces save_twins.h's name check: all five
// host files written (and again on a later join), her saves and co-op copies untouched, a write failing half way
// leaving no host file at all, and a save that fails the game's check refused before any write.
#include "host_world.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace coop;
using namespace coop::world;

namespace {
std::string save(const std::string &body) { // As the game writes one: EVAS, version 1, GVAS, the body, CRC-32 [8, -4).
  std::string b = std::string("EVAS\x01\0\0\0", 8) + "GVAS" + body;
  const uint32_t c = twins::crc32(b.data() + 8, b.size() - 8);
  return b + std::string(reinterpret_cast<const char *>(&c), 4);
}
std::string bulk(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::string s(n, '\0');
  for (char &c : s)
    c = char(rng());
  return s;
}
Chunk parsed(const std::string &m) {
  Chunk c;
  assert(parse_chunk(reinterpret_cast<const uint8_t *>(m.data()), m.size(), c));
  return c;
}

// An account folder in memory: files with a size and a write time that rises with each write, every name written
// checked against save_twins.h's writable(), and a backup folder.
struct MemFs {
  std::map<std::string, std::string> files, backup;
  std::map<std::string, uint64_t> times;
  uint64_t clock = 1000;
  std::vector<std::string> writes;
  int backups_made = 0;
  std::string fail_rename; // A rename onto this name fails, as a full or locked disk does.
  twins::Stat stat(const std::string &n) {
    auto it = files.find(n);
    return it == files.end() ? twins::Stat{} : twins::Stat{true, it->second.size(), times[n]};
  }
  bool read(const std::string &n, std::string &out, size_t max) {
    auto it = files.find(n);
    if (it == files.end() || it->second.size() > max)
      return false;
    out = it->second;
    return true;
  }
  bool write(const std::string &n, const std::string &b) {
    assert(twins::writable(n));
    writes.push_back(n);
    files[n] = b, times[n] = ++clock;
    return true;
  }
  bool rename(const std::string &from, const std::string &to) {
    assert(twins::writable(from) && twins::writable(to) && files.count(from));
    if (to == fail_rename)
      return false;
    files[to] = files[from], times[to] = times[from];
    files.erase(from);
    return true;
  }
  bool remove(const std::string &n) {
    assert(twins::writable(n));
    return files.erase(n) > 0;
  }
  std::vector<std::string> list() {
    std::vector<std::string> v;
    for (auto &[n, b] : files)
      v.push_back(n);
    return v;
  }
  std::string backup_dir() { return ++backups_made, "backups/1"; }
  bool backup_write(const std::string &n, const std::string &b) {
    assert(twins::backup_name(n));
    backup[n] = b;
    return true;
  }
  bool backup_read(const std::string &n, std::string &out, size_t) {
    auto it = backup.find(n);
    return it != backup.end() && (out = it->second, true);
  }
};
} // namespace

int main() {
  // ---- Messages ----
  {
    uint32_t id = 0;
    const std::string q = request(77);
    assert(q.size() == kRequestSize && parse_request(reinterpret_cast<const uint8_t *>(q.data()), q.size(), id) && id == 77);
    std::string bad = q;
    bad[3] ^= 1; // Another protocol.
    assert(!parse_request(reinterpret_cast<const uint8_t *>(bad.data()), bad.size(), id));

    const std::string host = save(bulk(150000, 1)); // Three chunks: 60000, 60000, the rest.
    const uint32_t crc = twins::crc32(host.data(), host.size());
    assert(chunks_for(host.size()) == 3 && chunk(5, host, crc, 3, 2).empty());
    const Chunk c = parsed(chunk(5, host, crc, 2, 2));
    assert(c.id == 5 && c.total == host.size() && c.crc == crc && c.index == 2 && c.count == 3 && c.slot == 2);
    assert(c.size == host.size() - 2 * kChunk);
    // Refusals, one defect each: a cut chunk, a wrong count, a slot past 4, an index past the count.
    std::string m = chunk(5, host, crc, 0, 2);
    Chunk out;
    assert(!parse_chunk(reinterpret_cast<const uint8_t *>(m.data()), m.size() - 1, out));
    std::string w = m;
    w[18] = 9;
    assert(!parse_chunk(reinterpret_cast<const uint8_t *>(w.data()), w.size(), out));
    w = m, w[20] = 5;
    assert(!parse_chunk(reinterpret_cast<const uint8_t *>(w.data()), w.size(), out));
    w = m, w[16] = 3;
    assert(!parse_chunk(reinterpret_cast<const uint8_t *>(w.data()), w.size(), out));
  }

  // ---- A transfer: any order, duplicates, other requests' chunks, disagreeing chunks, bad wholes ----
  {
    const std::string host = save(bulk(200000, 2));
    const uint32_t crc = twins::crc32(host.data(), host.size());
    Receiver r;
    r.start(9);
    for (int i : {3, 0, 3, 2}) // Chunk 3 twice: the second is ignored.
      assert(r.take(parsed(chunk(9, host, crc, uint16_t(i), 1))) != Receiver::Result::done);
    assert(r.take(parsed(chunk(8, host, crc, 1, 1))) == Receiver::Result::ignored); // An older request's.
    assert(r.take(parsed(chunk(9, host, crc, 1, 1))) == Receiver::Result::done);
    assert(r.bytes() == host && r.host_slot() == 1 && r.got() == 4 && !r.active());

    r.start(10); // One chunk with another total: the transfer ends.
    r.take(parsed(chunk(10, host, crc, 0, 1)));
    const std::string other = save(bulk(130000, 3));
    assert(r.take(parsed(chunk(10, other, crc, 1, 1))) == Receiver::Result::bad && !r.active());

    r.start(11); // A wrong CRC for the whole.
    Receiver::Result last = Receiver::Result::more;
    for (uint16_t i = 0; i < 4; ++i)
      last = r.take(parsed(chunk(11, host, crc ^ 1, i, 1)));
    assert(last == Receiver::Result::bad && r.why() == "its CRC-32 does not match" && r.bytes().empty());

    r.start(12); // Right CRC, but not a save the game would load.
    const std::string junk = bulk(70000, 4);
    for (uint16_t i = 0; i < 2; ++i)
      last = r.take(parsed(chunk(12, junk, twins::crc32(junk.data(), junk.size()), i, 0)));
    assert(last == Receiver::Result::bad && r.why() == "it fails the game's own load check");
  }

  // ---- The sender: every chunk once, kChunkEveryMs apart ----
  {
    const std::string host = save(bulk(130000, 5));
    Sender s;
    s.start(3, host, 4);
    Receiver r;
    r.start(3);
    std::vector<uint64_t> at;
    for (uint64_t t = 0; s.busy(); t += 25) {
      const std::string m = s.next(t);
      if (m.empty())
        continue;
      at.push_back(t);
      r.take(parsed(m));
    }
    assert(at.size() == 3 && at[1] - at[0] >= kChunkEveryMs && at[2] - at[1] >= kChunkEveryMs);
    assert(r.bytes() == host && r.host_slot() == 4 && s.next(10000).empty());
  }

  // ---- place(): the host's world into the guest's five host files ----
  const std::string world = save("the host's world"), mine = save("my own save"), progress = save("my co-op progress");
  {
    MemFs fs; // Her own save and her co-op progress beside: every host file holds the world, hers untouched.
    fs.files["StellarBladeSave00.sav"] = mine, fs.files["StellarBladeCoop00.sav"] = progress;
    assert(place(fs, world) == "");
    for (int n = 0; n < twins::kSlots; ++n)
      assert(fs.files[twins::host_name(n)] == world && !fs.files.count(twins::host_name(n) + twins::kTemp));
    assert(fs.files["StellarBladeSave00.sav"] == mine && fs.files["StellarBladeCoop00.sav"] == progress);
    // Again (a later join): every one replaced, still only host files written.
    const std::string newer = save("the host's world, later");
    assert(place(fs, newer) == "");
    for (int n = 0; n < twins::kSlots; ++n)
      assert(fs.files[twins::host_name(n)] == newer);
    for (const std::string &w : fs.writes)
      assert(w.rfind("StellarBladeHost0", 0) == 0); // Never one of her saves or copies.
    assert(fs.backups_made == 0);
  }
  {
    MemFs fs; // A write that fails half way (host file 03): no host file left, so no slot loads an older world.
    fs.files[twins::host_name(4)] = save("an older host's world");
    fs.files["StellarBladeCoop00.sav"] = progress;
    fs.fail_rename = twins::host_name(3);
    assert(place(fs, world) == "StellarBladeHost03.sav could not be written");
    for (int n = 0; n < twins::kSlots; ++n)
      assert(!fs.files.count(twins::host_name(n)) && !fs.files.count(twins::host_name(n) + twins::kTemp));
    assert(fs.files["StellarBladeCoop00.sav"] == progress);
    // A save that fails the game's check: refused before anything.
    std::string broken = world;
    broken[10] ^= 1;
    fs.writes.clear();
    assert(place(fs, broken) == "the host's save fails the game's own load check" && fs.writes.empty());
  }
  {
    MemFs fs; // The host's read of its own save: whole and checked, or why not.
    fs.files["StellarBladeCoop01.sav"] = world, fs.times["StellarBladeCoop01.sav"] = 5;
    std::string bytes;
    assert(read_save(fs, "StellarBladeCoop01.sav", bytes) == "" && bytes == world);
    assert(read_save(fs, "StellarBladeCoop02.sav", bytes) == "is missing");
    std::string broken = world;
    broken[12] ^= 1;
    fs.files["StellarBladeCoop01.sav"] = broken;
    assert(read_save(fs, "StellarBladeCoop01.sav", bytes) == "fails the game's own load check");
  }
  std::puts("host_world_test: ok");
}
