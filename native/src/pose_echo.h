// The bot's echo (native/bot/loop_bot.cpp): a game's own component table, keyframes and deltas sent back to it as the
// bot's, with the EVE moved shift_cm along the world X axis, so that game draws a second EVE next to its player
// without a recording. Free of Windows: tests/pose_echo_test.cpp checks it against the real codec.
#pragma once

#include <cmath>
#include <cstring>
#include <string>

#include "pose_codec.h"

namespace coop::pose {

// Where a message keeps its main mesh's world X (the first float of the world transform), or 0 when it has none:
// a keyframe (SBP) right after component 0's status byte when that byte says kWorld; a delta (SBD) after its age.
inline size_t world_x_at(const std::string &m) {
  if (m.size() < 4 || m[0] != 'S' || m[1] != 'B')
    return 0;
  if (m[2] == 'P' && m.size() >= kWorldAt + 4 && (static_cast<uint8_t>(m[kPoseHeader]) & kWorld))
    return kWorldAt;
  if (m[2] == 'D' && m.size() >= kPoseHeader + 4 + 4)
    return kPoseHeader + 4;
  return 0;
}

// m, a channel-1 message the peer sent, made into the echo's: a table (SBT) as it is, a keyframe or delta with its
// world X moved by shift_cm. False for anything the echo does not send back (SBE and show lines, junk), and for a
// position the move would take past kMaxWorld.
inline bool echo_shift(std::string &m, float shift_cm) {
  if (m.size() < 4 || m[0] != 'S' || m[1] != 'B')
    return false;
  if (m[2] == 'T')
    return true;
  if (m[2] != 'P' && m[2] != 'D')
    return false;
  const size_t at = world_x_at(m);
  if (!at)
    return m[2] == 'P'; // A keyframe without a world transform goes as it is.
  float x;
  std::memcpy(&x, m.data() + at, 4);
  x += shift_cm;
  if (!(std::fabs(x) <= kMaxWorld))
    return false;
  std::memcpy(&m[at], &x, 4);
  return true;
}

} // namespace coop::pose
