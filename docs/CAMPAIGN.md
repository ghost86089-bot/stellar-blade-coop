# Full co-op campaign: baseline, gaps and plan

The goal of this fork (branch `my-coop`): two players finish Stellar Blade's whole campaign together, building on
StellarCoop instead of rewriting it. This page says what works today and how we know, what is missing, and the order
in which it gets built. Nothing is listed as working without a run that shows it; everything else says how far it got.

Status words: **verified** (a run we did shows it: in game, or the automated tests named), **built** (the code and its
unit tests exist and pass, but it never ran in game), **missing** (nothing exists yet).

As of 2026-10-21, fork at `375a392` (StellarCoop v0.3.0-beta.1 + upstream's unreleased stage2, stage3-pilot and
stage4-spawn + this fork's Windows port and test link), on Steam build 24463856, UE4SS v3.0.1 Beta (git 08a0733).

## How StellarCoop is built (what we extend)

- **Two worlds.** Each player runs her own single-player game on her own save (or a co-op copy of it). Nothing makes
  the two worlds the same; the mod links them and mirrors parts of them.
- **Link:** Steam lobbies and Steam networking messages (relays, no server), host = lobby owner. Channel 0 control
  lines (hello, ping, chat, photo, vitals), channel 1 binary (component tables and poses), channel 2 the shared
  fight's lines. Protocol 11. `link_core.h` is platform free and runs against a fake Steam in tests.
- **Partner:** a display-only copy of her EVE (the "ghost"): her poses at up to 60 a second, her outfit, a HUD card
  with HP, shield and Beta. It does not collide, fight or take damage.
- **Shared enemies (`fight_engine.h`, `fight_sync.h`, `pilot.h`, `spawn_sync.h`):** the host's game decides HP,
  balance and death of each enemy that exists in both worlds (paired by spawn point, character id and max HP); scaling
  for two (135 % HP, 115 % balance by default), kill credit, takedowns, "same place" (the host drives the guest's copy
  of an enemy), "same spawns" (the same spawn/despawn decisions in both worlds) and "extra enemies".
- **Native part** (`StellarCoop.dll`) talks to Lua only through files in the mod folder; it reads game memory with
  fault-contained reads and is pinned to one build.

## Baseline against the target

| Area (target) | Today | How we know |
|---|---|---|
| Install and load into the game (UE4SS + mod) | verified | in-game runs 2026-10-21: UE4SS 08a0733 loads, `engine on: pinned build 24463856` |
| Join and leave a session | verified on the test link; Steam path verified upstream only (two PCs, 2026-10-17) | in-game run: lobby, hello both ways, `peer linked` with the bot; `loop_steam_test`, `link_core_test` |
| Test with one PC (bot partner) | verified | `STELLARCOOP_LOOP`, `loop_bot`, `tools/loop_e2e.py`, in-game link in 15–18 s |
| Bot shows as a moving EVE (echo of the host's own, 150 cm along X) | verified in game | 2026-10-22: `Ghost ... built: 6 parts (4 leaders)`, `drawn: 4 of 4 leaders`; screenshot of both EVEs walking on the first beach; partner card and "2 m" marker on screen |
| Chat between the players | verified in game both ways | 2026-10-23: the bot's "hello from the bot" in the game's chat box, and a line typed there ("test from buffy") reached the bot (`bot heard: in test from buffy`); the same box showed the session notices (lobby, the shared-enemies/co-op-save notice, zone changes) and Esc closed it and gave the game its keys back |
| Co-op save copies (Convert and play co-op) | verified in game on the test link | `StellarBladeCoop00.sav` made beside the save; Continue loads it (`saves=coop`) |
| Shared common enemies, host side: the guest's hits change the host's real enemies, a lethal claim kills | verified in game (host side) | 2026-10-22, first beach (Story Mode, scale 135/115): 58 bot claims of 150 HP, 58 confirmed exactly in the host's values, 0 wrong, 0 refused; enemy 494 from 5103 to 3 HP, killed by the lethal claim; host `claims=58 refused=0 kills=1` |
| Shared common enemies, guest side (her own copies paired and corrected) | built, not tested in game | needs two real games |
| Saves kept apart from the player's real saves during tests | verified | `userdir_probe.ps1`: private LOCALAPPDATA + `-userdir`; real Saved 0 differences in every run |
| See the partner move (ghost) | built here; verified upstream between two PCs | in game here: poses streamed (~20/s); the bot sends no poses yet, so no ghost was seen |
| Partner HUD, chat, synced photo mode | built; the HUD and the chat seen in game 2026-10-23 | the partner HUD (the friend's name, HP/SH/BE bars) drawn at the top left in the test copy; the co-op menu opened on F5, its keys moved the selection and Enter chose a row; photo mode still untested in game |
| Common enemies: one HP bar, one death | built, not tested in game (upstream: "not tested in game yet") | `fight_engine_test`, `fight_sync_test` pass (ASan, UBSan) |
| Scaling for two, kill credit | built | `fight_engine_test` (scaled 135/115) |
| Takedowns on shared enemies | built | `pilot_test`, `fight_engine_test` |
| Same enemy positions and attacks ("same place") | built, off by default | `pilot_test` (copy within 27 cm of the host moving) |
| Same spawns in both worlds, extra enemies | built, off by default | `spawn_sync_test`, `fight_engine_test` |
| Bosses and Alpha Naytibas shared | missing (left out on purpose upstream; boss finisher guard not built) | docs/usage.md |
| Both EVEs really in one fight (the guest's hits and damage reach the host's enemies) | built for common enemies via the shared fight; her EVE is still a ghost in the host's game | design above |
| Story, quests, cutscenes, area transitions shared | missing | no code; both games run their own story |
| Doors, lifts, puzzles, fast travel, camps shared | missing | no code |
| Death and respawn in co-op (survivor fights on, revive) | missing | each game handles its own death; host death only resets the guest's enemy HP |
| Rejoin after a drop | built (guest rejoins within 60 s) | `link_core_test` |
| The guest gets the host's world and Continue loads it | verified in game on the test link (game as guest of the bot, and as host) | 2026-10-22: game as host sent `StellarBladeCoop00.sav` to the bot, 629,372 bytes byte-identical. 2026-10-23: game as guest wrote the bot host's world to `StellarBladeHost00-04.sav` (byte-identical), switched to `saves=host` at the title, and its Continue entered slot 0 of it: the game's save after the load went to `StellarBladeHost00.sav`, her `StellarBladeSave00.sav` and `StellarBladeCoop00.sav` unchanged (same SHA-256) |
| Hold EVE's death (for revive) | mapped in game, not wired | 2026-10-22: MinimumHP (stat 107) = 1 on her FSBActor holds her at 1 HP, alive, under enemy hits; set back to 0 she dies on the next hit |
| Loot, inventory, upgrades | per player by design (each keeps her own) | README |
| Level change, long sessions | not tested | upstream README |
| Two real PCs on this fork | not tested | needs the second player |

## The decision that shapes the rest: whose world

Shared story needs both players to be in the same world state. Two ways, both possible on StellarCoop:

1. **Mirror two worlds.** Keep two campaigns and copy the host's story events into the guest's game (quest stages,
   world flags, opened doors, cutscene starts). Every event must be found, mapped and replayed; one missed flag and
   the worlds drift apart for good.
2. **Start from the host's world (recommended).** At join, the guest's game loads a copy of the host's save (sent over
   the link, kept as a separate co-op slot; the guest's own saves are never written). Both worlds then start
   identical, the existing shared-enemy machinery keeps fights the same, and only what changes during the session
   (story steps, doors, pickups, area changes) needs syncing, host-authoritative, as in the closed-source
   "Seamless Coop For Stellar Blade" (Nexus 3984, which describes this model).

Plan below assumes 2; it needs the owner's OK before step 4.

## Plan, in order (each step ends with a test that can fail)

1. **A save to play on, in the test copy.** Done 2026-10-22: New Game (Story Mode) in the guarded test copy, the
   game's own autosave at the first beach (`StellarBladeSave00.sav`, test folder only), converted to a co-op copy.
2. **The bot as a real guest.** The bot replays recorded poses (the host draws its ghost) and runs the shared fight's
   guest side against a fake world mirroring the host's enemies. Test: in game, a shared common enemy pairs, its HP
   changes from the bot's claims, it dies once in both, and the logs agree.
3. **Shared common enemies in game.** Turn on `same_spawns` and `same_place`, run their own probes, fix what breaks.
   Test: the bot scenario above with logs and one in-game screenshot per step.
4. **Host-world start (decision above).** Save transfer over the link in chunks, a guest save set, load on join.
   Test: two games (game + bot, then two real clients) report the same save digest after join.
   Built 2026-10-22 (`host_world.h`, wired in `steam_link.cpp`): a guest asks every 10 s until a world arrives (and on
   `coop world`); a host in her world sends the save her game entered (`SBW` chunks of 60 KB, 8 a second, CRC-32 over
   the whole); the guest checks the CRC and the game's own load check. Where it goes changed on 2026-10-23 (below).
   Verified offline: `host_world_test`, and `tools/loop_e2e.py`: the real DLL as guest writes a 650 KB save
   byte-identical in about 1.3 s.
   In game 2026-10-22 (test link, bot partner): the game as host sent its co-op save, byte-identical (629,372 bytes);
   the game as guest wrote the bot host's world into co-op slot 5, byte-identical, and wrote it again over its own
   earlier copy on the next run. At its title a host's entered slot is the last session's (it read 0 before any
   load), so a host now answers only once her world is loaded, and a guest asks every 10 s until a world arrives
   (both in this fork since this run; the second guest run used them).
   Load on join, what is known (2026-10-22): the mod only reads the client's entered slot (+0x54), load slot (+0x868)
   and load mode (+0x86c, 2 = a load, set by the title's Continue and Load Game handlers); `coop saves probe` logs them.
   Nothing in it starts a load. Settled in game 2026-10-22: (a) Continue does not pick slot 5: with the host's world
   in `StellarBladeCoop04.sav` (the newest file), Continue entered slot 0 and autosaved it. (b) Load Game lists one
   entry only and it too entered slot 0; slot 5 is not offered anywhere at the title. (That host's world was an older
   copy of slot 0, so "newest by the save's own time" is not ruled out, but neither menu can be relied on.)
   Built 2026-10-23: a third save set. The switch rewrites the game's 16-character save name in place
   (`StellarBladeSave`, `StellarBladeCoop`, and now `StellarBladeHost`, save_slot.h). The title's slot list is built
   from the files only at startup and on each return to the title, while loads use the name of the moment: so the
   guest writes the host's world into all five `StellarBladeHost0N.sav` (read back; a failure removes all five) with
   no save of hers loaded, then switches to the host set at the title (save_twins.h `to_host`, refused only while a
   conversion runs), and whatever slot her Continue or Load Game picks is the host's world. Her saves and co-op copies
   are never written; every co-op saves request is refused while the host set is on, until the game restarts; the
   slot line reads `slot=coop ... saves=host`, so co-op features run on it. Verified: `save_slot_test`,
   `save_twins_test`, `host_world_test`, `tools/loop_e2e.py` (the DLL as guest writes all five byte-identical), and
   four twins that fail them (a failed write leaving files, the Host word one character off, a switch during a
   conversion, the slot line not co-op on the host set). In game 2026-10-23 (game as guest of the bot, test link):
   received, written, switched at the title, notice "The host's world is ready: press Continue to join it"; no co-op
   saves question (it shows only on `saves=main`); Continue entered slot 0 (`slot=coop index=0 saves=host`), the shared
   fight came on as guest, and the game's save after the load went to `StellarBladeHost00.sav` while her
   `StellarBladeSave00.sav` and `StellarBladeCoop00.sav` kept their SHA-256. The run before this change is the twin:
   without the host set, Continue entered her `StellarBladeCoop00.sav`. Not yet: two real clients comparing digests,
   and a guest with no save of her own (the title then offers no Continue; untested).
5. **Story and world sync, host authoritative.** Map with UE4SS dumps: quest/mission state, world flags, doors, lifts,
   fast travel, camps, cutscene and level-transition triggers. Send each as a host event; the guest applies it or is
   moved to the host. Test per event type, then a chapter played with the bot following the host.
   Mapped offline 2026-10-23 (`docs/story.md`, `tools/gvas.py` and `tools/story.py`): the story and world state is in
   the save, in three structures — quests (`DataMap_SBQuest[Quest]`: states, tasks and their counters, tracking,
   complete and failed), mission records (`GamePlayProgressRecordData`: a success flag per achievement, trigger and env
   state, keyed by quest task group) and zones (`DataMap_SBEnv[Env]`: per zone, its env objects — doors, lifts,
   consoles — with activate/active state/interacted/count, NPCs, triggers, event items and actors, and world objects by
   their own `UniqueKey`). `tools/story.py` turns those into one sorted `key = value` line per fact (1,502 in the
   test save; aliases, never array indices), with a SHA-256 digest and a diff, so two saves can be compared in one
   value and a session's story changes read in one screen. Verified: `story_test.py` (a save written as the game writes
   it, read back; volatile fields, a reset zone and reordered zones change nothing; a quest state, a task value, a
   mission flag, an env object and a world object each show as one line), and the test save before and after a walk on
   the first beach: 236 changed lines over the whole save, 3 of them story (two achievement flags and one effect
   volume switched on). Read live 2026-10-23: `coop story [dump|find]` (`Scripts/story.lua`, read only) prints the same
   lines from the save object the running game holds, to `StellarCoopStory.txt` in the mod folder, asked by command or
   by `coop-story.txt` at load; enum fields are named through the game's own `UEnum`s. In the test copy, asked at the
   title and run as the world came up, the dump wrote 1,532 lines whose digest is the file's (`tools/story.py compare`:
   "the same"); two minutes later in the same session the game held a partial object (946 lines, 130 of 199 zones), so
   the full mirror is only there at load. Played on 2026-10-23: two dumps 25 s apart with nothing done between them are
   identical, so in-session changes can be diffed; 32 s after an autosave (a 1,976,427-byte save) the dump found three
   save objects and wrote 10,800 lines in 15.3 s, the quests and mission records matching the new save and the
   world-actor stage not (maps as pointers, positions not filtered, 1-based indices), the zone event items off by one;
   73 s later the objects were gone again. `docs/story.md` has the numbers. Not mapped yet, and listed in
   `docs/story.md`: live changes during play, which env alias is a door or a lift, cutscenes and level transitions, the
   enums' other values, and `bZoneReset` (excluded: it flips for every zone that is not the loaded one).
6. **Death and respawn.** A downed player waits while the other fights; respawn at the host's last camp; both down
   resets the area. Test with scripted deaths.
   Logic built 2026-10-22 (`revive_core.h`, the owner's Sekiro revive rules): down instead of dead, revived by the
   partner within 1.5 m for 3 s at half HP, the next lethal hit spectates, a camp rest brings a spectator back and
   resets revives, down or spectating with no alive partner is the game's own death, and a `dead` state until the
   respawn so that both down means both respawn; SBV lines on channel 1. `revive_core_test` with two twins.
   Mapped in game 2026-10-22 on the first beach (direct writes from outside, test copy and test save only): her
   FSBActor is the actor manager's local index as `vitals()` finds it; HP is stat 1 (+0x11C), MinimumHP stat 107
   (+0x2C4, the stat the shared fight floors enemies with), life +0x2600 (1 alive, 3 dead). With MinimumHP 1 a
   Naytiba's hits (24 to 41 each) took her from 500 to 16 and then to 1, and she stayed at 1 and alive under further
   hits; MinimumHP back to 0, the next hit gave HP 0, life 3 and the game's YOU DIED screen, which waits for R
   ("Revive"); after R: life 1, full HP, the same FSBActor. So: the hold is MinimumHP +1 (in the DLL through ApplyStat
   type 107 on the logic thread, as the enemy floor), "down" is HP at the floor right after a hit, and "respawned" is
   life 3 to 1. Still to map: a camp-rest signal, hiding a spectator, and whether a held EVE should stop acting (the
   enemy kept hitting her at 1 HP).
7. **Bosses.** Share BossMonster HP, phases and finishers (the guard upstream lacks). Test on the first boss.
8. **Reconnect and late join.** Rejoin restores the host's world state (step 4's transfer again). Test: kill the bot
   mid-fight, restart it, compare state.
9. **Two real PCs, whole campaign.** With the owner's friend; long-session and high-enemy-count runs with the logs.

## Tests and how to run them

- Everything without the game: `python tools/install.py --dry-run --game <folder with ue4ss\Mods>` (Lua, Python and
  C++ tests with ASan and UBSan, then the DLL build) and `python tools/loop_e2e.py`.
- In game, one PC: the guarded probe in the coop-anything workspace (`local\stellarblade\userdir_probe.ps1
  -LoopPort N`), which launches the clean test copy with the test link, joins the bot, and checks the player's real
  saves before and after.
