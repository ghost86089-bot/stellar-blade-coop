// The bot's echo (pose_echo.h) against the real codec: a keyframe moved 150 cm decodes 150 cm away and otherwise the
// same; a delta moved alike rebuilds, on the moved keyframe, a pose 150 cm away; a table goes as it is; SBE lines and
// junk are not echoed; a move past kMaxWorld is refused.
#include "pose_echo.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace coop::pose;

namespace {
Table table() {
  Table t;
  t.generation = 42;
  Part leader{1, 1, 1, 5, true}, follower{1, 1, 1, 5, false}, small{1, 1, 1, 3, true};
  t.eve = {leader, follower, small};
  return t;
}
constexpr size_t kLeaderBones = 8; // 5 + 3: the follower carries none.

std::vector<uint8_t> bones(float wiggle) {
  std::vector<float> f;
  for (size_t b = 0; b < kLeaderBones; ++b) {
    const float one[12] = {0, 0, std::sin(wiggle * b), std::cos(wiggle * b), float(b), 2, 3, 0, 1, 1, 1, 0};
    f.insert(f.end(), one, one + 12);
  }
  std::vector<uint8_t> out(f.size() * 4);
  std::memcpy(out.data(), f.data(), out.size());
  return out;
}

std::string pose(const Table &t, float x, float wiggle, uint32_t seq, uint64_t pub) {
  const float root[10] = {0, 0, 0, 1, x, 200, 300, 1, 1, 1};
  const std::vector<uint8_t> b = bones(wiggle);
  Effect fx[kEffectSlots] = {};
  PoseHeader h;
  h.generation = t.generation, h.sequence = seq, h.publication = pub, h.map = 7;
  std::string out(pose_size(t), '\0');
  uint32_t clamped = 0;
  const size_t n = encode_pose(t, h, root, b.data(), fx, reinterpret_cast<uint8_t *>(&out[0]), clamped);
  assert(n == out.size() && clamped == 0);
  return out;
}

void decode(const Table &t, const std::string &m, float *root, std::vector<uint8_t> &b) {
  PoseHeader h;
  Effect fx[kEffectSlots];
  b.assign(kLeaderBones * kTransformBytes, 0);
  const char *why = decode_pose(t, reinterpret_cast<const uint8_t *>(m.data()), m.size(), h, root, b.data(), fx);
  if (why)
    std::printf("decode refused: %s\n", why);
  assert(!why);
}
} // namespace

int main() {
  const Table t = table();

  // A keyframe: 150 cm further along X once echoed, everything else the same.
  const std::string key = pose(t, 100, 0.1f, 1, 1'000'000);
  std::string moved = key;
  assert(world_x_at(moved) == kWorldAt && echo_shift(moved, 150));
  float a[10], b[10];
  std::vector<uint8_t> ba, bb;
  decode(t, key, a, ba);
  decode(t, moved, b, bb);
  assert(std::fabs(b[4] - (a[4] + 150)) < 0.05f);
  for (int k = 0; k < 10; ++k)
    if (k != 4)
      assert(a[k] == b[k]);
  assert(ba == bb);

  // A delta against that keyframe, moved alike, rebuilds on the moved keyframe a pose 150 cm from the original's.
  const std::string full = pose(t, 120, 0.4f, 2, 1'016'000);
  std::string delta(pose_size(t), '\0');
  const size_t dn = encode_delta(t, reinterpret_cast<const uint8_t *>(key.data()),
                                 reinterpret_cast<const uint8_t *>(full.data()), 16'000,
                                 reinterpret_cast<uint8_t *>(&delta[0]), delta.size());
  assert(dn > 0);
  delta.resize(dn);
  assert(world_x_at(delta) == kPoseHeader + 4 && echo_shift(delta, 150));
  std::string rebuilt(pose_size(t), '\0');
  const char *why = apply_delta(t, reinterpret_cast<const uint8_t *>(moved.data()), 1'000'000,
                                reinterpret_cast<const uint8_t *>(delta.data()), delta.size(),
                                reinterpret_cast<uint8_t *>(&rebuilt[0]));
  if (why)
    std::printf("apply refused: %s\n", why);
  assert(!why);
  float f[10], r[10];
  std::vector<uint8_t> bf, br;
  decode(t, full, f, bf);
  decode(t, rebuilt, r, br);
  assert(std::fabs(r[4] - (f[4] + 150)) < 0.05f && r[5] == f[5] && r[6] == f[6] && bf == br);

  // A table goes back unchanged; an SBE line, junk and a move past kMaxWorld do not go.
  std::string tab = "SBT 11\ngeneration 42\n";
  assert(echo_shift(tab, 150) && tab == "SBT 11\ngeneration 42\n");
  std::string sbe = "SBE....", junk = "xx", far = key;
  assert(!echo_shift(sbe, 150) && !echo_shift(junk, 150));
  assert(!echo_shift(far, 2 * kMaxWorld) && far == key);
  std::puts("pose_echo_test: ok");
}
