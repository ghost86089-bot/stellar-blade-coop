// The host's world for the guest (host_world.h): the request and chunk messages and their refusals, a transfer
// reassembled in any order with duplicates, chunks that disagree, a whole that fails its CRC or the game's own check,
// the sender's pacing; and place() on an in-memory account folder that enforces save_twins.h's name check: an empty
// slot, an untouched co-op copy (backed up, replaced, its manifest line gone, the other lines kept), the guest's own
// co-op progress (refused, nothing changed), the host's world written before (replaced), and a save that fails the
// game's check (refused). Her own saves are never written.
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

  // ---- place(): where the host's world may go ----
  const std::string world = save("the host's world"), mine = save("my own save"), progress = save("my co-op progress");
  {
    MemFs fs; // An empty slot 2, her own save in 0: written, recorded; her save untouched.
    fs.files["StellarBladeSave00.sav"] = mine, fs.times["StellarBladeSave00.sav"] = 1;
    assert(place(fs, 2, world, 7656) == "");
    assert(fs.files["StellarBladeCoop02.sav"] == world && fs.files["StellarBladeSave00.sav"] == mine);
    const Marker m = parse_marker(fs.files[twins::kHostWorld]);
    assert(m.present && m.slot == 2 && m.host == 7656 && twins::same(m.copy, fs.stat("StellarBladeCoop02.sav")));
    assert(fs.backups_made == 0 && !fs.files.count("StellarBladeCoop02.sav.coop-tmp"));

    // Again, over the host's world it wrote: replaced (backed up first).
    const std::string newer = save("the host's world, later");
    assert(place(fs, 2, newer, 7656) == "" && fs.files["StellarBladeCoop02.sav"] == newer);
    assert(fs.backup["StellarBladeCoop02.sav"] == world);
    for (const std::string &w : fs.writes)
      assert(w.rfind("StellarBladeSave", 0) != 0); // Never one of her own saves.
  }
  {
    MemFs fs; // An untouched co-op copy in slot 1 (its line matches), and a line for slot 0 that must stay.
    fs.files["StellarBladeCoop01.sav"] = mine, fs.times["StellarBladeCoop01.sav"] = 50;
    fs.files[twins::kManifest] = "0 10 20 30 40\n1 " + std::to_string(mine.size()) + " 50 9 9\n";
    assert(place(fs, 1, world, 1) == "" && fs.files["StellarBladeCoop01.sav"] == world);
    assert(fs.backup["StellarBladeCoop01.sav"] == mine);
    twins::Lines lines;
    assert(twins::parse(fs.files[twins::kManifest], lines) && lines[0].present && !lines[1].present);
  }
  {
    MemFs fs; // Her co-op progress in slot 3 (no line matches it): refused, nothing written.
    fs.files["StellarBladeCoop03.sav"] = progress, fs.times["StellarBladeCoop03.sav"] = 60;
    fs.files[twins::kManifest] = "3 1 2 3 4\n";
    const std::string why = place(fs, 3, world, 1);
    assert(why == "co-op slot 3 holds your own co-op progress: not replaced");
    assert(fs.files["StellarBladeCoop03.sav"] == progress && fs.writes.empty() && fs.backups_made == 0);
    // A record naming another slot does not make slot 3 the host's either.
    fs.files[twins::kHostWorld] = "2 " + std::to_string(progress.size()) + " 60 1\n";
    assert(place(fs, 3, world, 1) == why && fs.writes.empty());
    // A save that fails the game's check, or a slot past 4: refused before anything.
    std::string broken = world;
    broken[10] ^= 1;
    assert(place(fs, 0, broken, 1) == "the host's save fails the game's own load check" && fs.writes.empty());
    assert(place(fs, 5, world, 1) == "no such co-op slot");
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
