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
| Chat between the players | verified in game | the bot's "hello from the bot" shown in the game's chat box |
| Co-op save copies (Convert and play co-op) | verified in game on the test link | `StellarBladeCoop00.sav` made beside the save; Continue loads it (`saves=coop`) |
| Shared common enemies, host side: the guest's hits change the host's real enemies, a lethal claim kills | verified in game (host side) | 2026-10-22, first beach (Story Mode, scale 135/115): 58 bot claims of 150 HP, 58 confirmed exactly in the host's values, 0 wrong, 0 refused; enemy 494 from 5103 to 3 HP, killed by the lethal claim; host `claims=58 refused=0 kills=1` |
| Shared common enemies, guest side (her own copies paired and corrected) | built, not tested in game | needs two real games |
| Saves kept apart from the player's real saves during tests | verified | `userdir_probe.ps1`: private LOCALAPPDATA + `-userdir`; real Saved 0 differences in every run |
| See the partner move (ghost) | built here; verified upstream between two PCs | in game here: poses streamed (~20/s); the bot sends no poses yet, so no ghost was seen |
| Partner HUD, chat, synced photo mode | built (chat verified bot↔bot) | `tools/loop_e2e.py` chat both ways; HUD not seen in game |
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
| Rejoin after a drop | built (guest rejoins within 60 s); late join with world transfer missing | `link_core_test` |
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
4. **Host-world start (decision above).** Save transfer over the link in chunks, a guest co-op slot, load on join.
   Test: two games (game + bot, then two real clients) report the same save digest after join.
   Built 2026-10-22 (`host_world.h`, wired in `steam_link.cpp`): a guest asks once a link (and on `coop world`); the
   host sends the co-op save its game entered (`SBW` chunks of 60 KB, 8 a second, CRC-32 over the whole); the guest
   checks the CRC and the game's own load check and writes it into co-op slot 5 at the title, never over her own co-op
   progress (backed up first, its manifest line dropped, recorded in `StellarCoopHostWorld.txt`). Verified offline:
   `host_world_test` (with a twin), and `tools/loop_e2e.py`: the real DLL as guest writes a 650 KB save byte-identical
   in about 1.3 s. Not yet: the host side in game (its loaded co-op save sent), and loading the slot by itself (today
   the guest gets a notice to load co-op slot 5 with Load Game).
   Load on join, what is known (2026-10-22): the mod only reads the client's entered slot (+0x54), load slot (+0x868)
   and load mode (+0x86c, 2 = a load, set by the title's Continue and Load Game handlers); `coop saves probe` logs them.
   Nothing in it starts a load. Next, in game, one session: (a) does Continue pick the newest co-op file (then the
   guest's Continue already lands in the host's world, which is written last) or the slot played last by another
   record; (b) if not, find the title's Load Game function for a slot with UE4SS (its widget, WB_LoadGame, and what
   its slot button calls) and call it from Lua once the host's world is written. Test: the guest's entered slot
   reads 4 and both games' first save digests after the load match.
5. **Story and world sync, host authoritative.** Map with UE4SS dumps: quest/mission state, world flags, doors, lifts,
   fast travel, camps, cutscene and level-transition triggers. Send each as a host event; the guest applies it or is
   moved to the host. Test per event type, then a chapter played with the bot following the host.
6. **Death and respawn.** A downed player waits while the other fights; respawn at the host's last camp; both down
   resets the area. Test with scripted deaths.
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
