
## 2026-10-09: landscape + fixed stick (Claude Opus 5.5)
- Landscape 280x240 via `cv->setRotation(DOOM_ROT)` (1 = logical top on the panel's right
  edge; flip to 3 if the watch should be turned the other way). Raycaster writes go through
  `pix(x, y)`; `doomLeave()` (from onExit) restores rotation 0 for the OS.
- Controls: single-touch CST816S, so no twin pads. Left half = fixed stick (bottom-left),
  right half = fire on touch-down, side button = fire, 2 s hold = exit (bar from 0.5 s).
  HUD = HP + AMMO bars only.
- Measured on V2.0: 240 MHz, frame 66-90 ms (11-15 fps); flush alone is ~49 ms (Arduino_GFX
  SPI push of 134 KB from PSRAM): that, not the raycaster, is the frame-rate ceiling.
  `I/DOOM: fps=...` logs every 5 s.
- Fixed: Doom ran at 80 MHz. The render loop picks the clock from the PREVIOUS view before
  calling render(), which Doom never returns from, AND `powerPerfDemand()` trusted its
  cached flag (80->80 measured). onEnter now forces 240; the governor checks the real clock.
- Test harness: `goto` is ignored while Doom runs; Doom polls input per frame, so hold sim
  touches (a short `tap` can be missed); console replies can stall ~15-30 s (known USB
  console stall), and a stalled `btn up` once left the sim button held through exit into a
  3 s OS power-off: use `btn hold N` (presses and releases inside one command).
- Open: one panic (backtrace through prvIdleTask) seen once while starting a game; not
  reproduced in ~10 later starts.

## 2026-10-09 (later): tilt to look, stick strafes
- Look = tilt left/right (lateral gravity along landscape x: -ay/4096 for DOOM_ROT 1, from the
  particles.cpp convention downhill = (-ax, -ay)). Level = filtered angle over the first
  0.3 s of each level; dead zone 0.07 g, full turn at 0.42 g, soft curve; TILT_SIGN flips it.
  Look indicator top-centre. Stick: up/down walk, left/right strafe (0.85x speed).
- Verified on V2.0 with ewsim tilt: right-side-down turns right (dot full right, view turns);
  stick-right strafes without turning. Real-wrist feel NOT yet checked.
- ewt `tilt` takes g, not raw counts (`tilt 0 2800 3000` clamps to 0,0,-8 g -> looked like
  "always turns right"). Resting watch on the desk read (-1828, 954, -3704).
- The player dies within ~10 s if idle (demons aggro at 11 cells): tests must act quickly
  or expect DEAD states.

## 2026-10-09 (evening): fps + feel pass
- FPS 13 -> ~30 (capped by the 33 ms frame budget). Measured on V2.0 (`I/DOOM fps=` every 30 s):
  canvas flush 49 ms (per-pixel) -> CPU writePixels strips 40-53 ms -> GDMA strips 28 ms
  (doom_lcd.cpp = copy of modules/hopper/hopper_lcd.cpp, agreed with the Hopper session; no
  symbols shared across modules). Double-buffered: frame N is pushed by a core-0 task
  ("doomflush", prio 1) while core 1 renders N+1; wait_ms ~0 now, so render is the limit.
  ewsimTapRows() feeds the ewsim shadow, since DMA bypasses the bus tap.
- Feel: tilt drift-to-level near level (5 s), double-press = re-level, full-tilt turn boost
  (orange dot); round player collider (slides corners); accel/decel momentum; sprint at the
  stick rim; weapon sway; fire shake + muzzle light; knockback + sink/redden death; red edge
  toward attacker; auto-fire while the right half is held (only with a demon in the crosshair).
- Pacing: demons wake after 0.7 s of line of sight; 2.5 s grace; melee 7 dmg / 1.4 s; spawns
  moved >= 9 cells from the start (the old ones were 3-4 cells: instant pair attacks);
  medkits (+25) + ammo crates (+15); level 2 (8 demons); "YOU WIN" after it.
- Verified on the watch by event log: kills, ammo pickup (40 -> 55), hurt direction,
  re-level, level 2 load, exit; 30 fps in play. NOT verified: medkit pickup, level 1 -> 2
  via a real clear, death animation visuals.
- Test harness: during Doom, ewt replies / snaps often time out or arrive corrupted (worse
  with the core-0 push task). Console priority 2 did NOT fix it (reverted), so it's the known
  USB console stall. Use `-v run` scripts and the DOOM event log lines; snaps are hit or miss.

## 2026-10-09 (night): endless levels, enemy types, guns
- "Crash on exit" (Ewan): next boot was reset=POWERON with no panic in the RTC mirror. Cause
  (inferred, not reproduced): the power manager's idle clock never saw Doom's input (Doom
  owns touch + button), so the screen blanked right after exit and looked dead; a long press
  then powered the watch off. Fix: powerOnActivity() on input (<= 2/s) and on exit.
- Levels: generated from the level number (xorshift seeded per level; 10 retries until >= 60%
  of the arena is open), 16x16 + 2/level up to 32x32, wall runs with a doorway, pillars,
  unreachable pockets filled. Enemies 2+n (boss level: one Baron + n), >= 35% of the map
  from the start. Death -> back to level 1, best level kept (RAM only).
- Enemies (kEnemies): imp, runt (fast/small), spitter (ranged, keeps 3.5-6 cells), bomber
  (rushes + explodes, chain reacts), brute (armour 0.55), shade (translucent), Baron boss
  (3-fireball spread + melee) every 5th level. hp/speed/damage scale per level.
- Guns (kGuns): pistol + SMG (9mm), shotgun (7 pellets, falloff by 7 cells), sniper (AP,
  pierces 3), RPG (rocket, 2.6-cell blast, 40% self-damage). Typed ammo only for the gun in
  hand (other boxes drawn dim). Fire on a gun = swap; the dropped gun re-arms once you step
  off. 9mm guns trickle 3 rounds when empty; an empty special gun gets a pistol by the start.
- Sprites in one PSRAM block (19 x 2 KB). DMA strips are now freed on exit (15 KB internal).
- Verified on the watch so far: builds and runs; level 1 generates (map 16, 3 enemies, 3
  items, pistol). Ewan was play-testing; enemy types / guns / swap / boss not yet checked by me.
- GFX gotcha: Arduino_GFX.h #defines OLIVE (and other colour names): a local `OLIVE`
  constant fails with "expected unqualified-id before numeric constant" at Arduino_GFX.h:41.

## 2026-10-09 (late): full arsenal, inventory, damage types, coins + shop
- Weapons (kGuns, 11): pistol, dual pistols, SMG (9mm); shotgun (shells); sniper (heavy, AP,
  pierce 3); RPG (rockets); chainsaw (melee, no ammo); flamer (fuel, cone + burn DOT); plasma
  (cells, projectile); railgun (5 cells, 0.45 s charge, pierces 16); grenade launcher
  (bouncing, 1.4 s fuse). Each has a damage type: ballistic / explosive / fire / energy / melee.
- Enemies (kEnemies, 13): imp, runt, spitter, brute, bomber, shade, Baron (boss every 5th),
  mancubus (5-ball spread), leaper (crouch -> pounce), shield (blocks bal/ener/melee from
  the front, turns slowly: flank it; front/back sprites), necro (raises one corpse every
  6 s), turret (fixed, 3-bolt bursts, spawns by walls), bat (swarm of 3, flying, erratic).
  mult[DT] per enemy = the effectiveness table; AP ballistic ignores resistance (<1 -> 1).
  Hit marker: green >= 1.3x, grey <= 0.65x, blue = shield block.
- Inventory: every gun you walk over is kept (or tops up its ammo); SWITCH button top centre
  (touch zone x 120-204, y < 34) cycles owned guns. Ammo still only picked up for the gun in
  hand (Ewan's rule); boxes for other guns are drawn dim.
- Coins: every kill drops its EnemyDef.coins, plus piles on the floor; reset on death. WIN ->
  shop: health +25 / full, ammo packs for guns you own, guns unlocked by the next level;
  2 x 4 tiles, paged, NEXT (or the side button) starts the next level. Bought guns stay on
  their tile as OWNED (rebuilding the list shifted tiles under the finger).
- Verified on the watch (temporary debug build: start L11 with $500, auto-clear): L11/L12
  generate 13/15 enemies of 7-8 types on 32x32 maps at 29 fps; shop buys across both pages
  with correct prices; switch cycles; every weapon fires with no crash; exit -> AppList,
  screen still on 16 s later, heap 40 -> 56 KB (DMA strips freed). NOT verified by me:
  per-enemy behaviours in a real fight (leap, necro raise, shield block, turret bursts),
  damage numbers vs types, boss level.
