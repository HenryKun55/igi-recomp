# Gameplay update rate (game_hz)

IGI runs its game logic in fixed steps. The scheduler 0x402260 runs one update
(or one render) per call and keeps `steps * 1000 / rate` in line with the clock.
The rate is set by SetRate 0x402820: 60 Hz in the menus and 30 Hz in gameplay.
Almost all game code assumes a step of 1/30 s.

`igi.cfg game_hz=N` (Vita) or `IGI_GAME_HZ=N` (desktop), with N in 31..240,
replaces the gameplay 30 with N. Rendering follows the update rate (one render
per update), so 60 gives 60 fps.

This document lists every place found where behaviour depends on the step being
1/30 s, and how each one is kept at its 30 Hz real-time speed. At 30 Hz (no
override) none of the fixes is active and the original code runs unchanged.

## Mechanisms

The fixes are applied by the recompiler (`tools/recomp/tick_rate.py`, used by
`gen.py`) and the runtime (`runtime/src/tick_rate.c`).

- **Gate.** A per-step update handler of a logic object keeps running 30 times a
  second of game time. At a higher rate the call is skipped on the steps between
  two 30 Hz ticks. The step that runs is the first step of each tick (the step
  where `floor(steps * 30 / rate)` changes).
  - With `GATE_CHILDREN`, a skipped call still updates the object's children
    through 0x488700, the generic child loop that the handler inlines.
  - Container updates (Game, Level, Dynamic, ConditionalContainer, Car, Train,
    EditRigidObj, SectorObject, ...) are never gated. They set up context that
    their children rely on: gating Level or Dynamic made the player body read
    a null pointer.
- **Constant slot (CONSTS, IMMS).** The absolute memory operand or immediate of
  one instruction is redirected to a slot in the guest page at 0x3FF000.
  - A slot holds `value * (30/rate)^exp`, or `value^(30/rate)` for per-step
    factors.
  - Scope G applies whenever gameplay runs at another rate. Scope P applies only
    inside the player scope (code shared with gated soldiers).
- **Tick-only instruction.** The increment of a global per-step counter runs on
  30 Hz ticks only, so the counter keeps counting 30 Hz ticks.
- **Hook.** A host replacement for a guest function:
  - GetStepCount, which returns 30 Hz ticks;
  - camera shake;
  - the cutscene clock.
- **Player scope.** The player's HumanPlayer update (0x454EE0 on the HumanPlayer
  object: body, view, input, weapons subtree) runs every step. During it:
  - the scope-P slots hold scaled values;
  - the animation step `[0xA54658]` (animation units per step, 4800/30) is set
    to 4800/rate.

  The player's animations (walk root motion, view and weapon animations, blend
  times) therefore advance at their real-time speed in smaller steps. Mouse look
  is naturally rate-independent: the mouse delta is consumed per step.
- **Mode tracking.** `tick_set_rate` follows the SetRate callers.
  - A 30 passed by the game starts gameplay mode.
  - The menu pair 0x418CCE/0x417AAA saves and restores the mode around the
    in-game menu. The menus keep their own 60 Hz untouched.

Below 30 Hz is not supported: gates only skip steps, they never repeat them.

## Inventory

Status keys:
- **verified A/B**: checked with the A/B harness (see Verification).
- **static**: from the code, not exercised in mission 1's opening.
- **neutral**: no dependency.
- **known gap**: not fixed (listed again at the end).

### Clock and global counters

| Guest address | What | Dependency | Fix | Status |
|---|---|---|---|---|
| 0x402260 scheduler, 0x402820 SetRate | Fixed-step loop; `[clk+0x34]` steps, `[clk+0x38]` renders, `[clk+0x3C]` rate | Defines the step | Rate override in `tick_set_rate` | verified A/B |
| 0x4028B0 GetStepCount (48 callers) | Step-count timers (`now + N`, `seconds * 30`), once-per-step guards (Train +0x888, Car +0x924, Elevator +0x644, StationaryGun +0x218), AI path memo 0x4530A0, AI event stamps 0x489500, AI round-robin 0x489750 (`steps & 3`), patrol DELAY 0x48E920 (data in ticks), cooldowns 0x4606F0, 0x4637C0, 0x476CC0, 0x47B750, 0x48A920, 0x48BE30, AI action deadlines 0x48E0A0, 0x48E150, 0x48EB60 | Steps | Hook `rt_tick_step_count`: returns `floor(steps * 30 / rate)`. Every deadline, guard and stagger keeps its 30 Hz meaning. Guarded ungated objects (Train, Car, Elevator, StationaryGun) effectively update once per tick | verified A/B (trains, AI) |
| 0x4028C0 GetRenderCount: 0x44FB3B HumanAI, 0x45F2BF Human draw, 0x460C80 debug key | "Drawn in the last frame" (render count − stamp ≤ 1) | Renders | none needed: holds while renders ≥ updates | neutral |
| 0x402870 GetRate: 0x41BEB6 ScrollListBox, 0x41D86A InputBox (menus, rate-aware) | — | Uses the rate | none | neutral |
| 0x402870 GetRate: 0x48947F mission clock `[LF+0x68] / (rate * 3600)` | LevelFlow tick is kept in 30 Hz ticks (below) | Mixes the rate with ticks | CONST 0x489492 (3600.0) exp +1 | static |
| 0x489290 LevelFlow, `inc [LF+0x68]` at 0x48936B ("%s.nTick") | Mission tick. Readers: Missile 0x42859B (`& 0x1F`), use windows (Terminal, Cabinet, Switch, GenericTBA: +2 ticks), StatusMessage repeat (+210), countdown 0x4894B0 (−30), "Time Left" HUD 0x486F30 (magic /30) | Steps | TICK_ONLY 0x48936B (fixes every reader at the source) | verified A/B (idle) |
| 0x4F0580 Level, 0x4F05F2 / 0x4F05FC | Engine level clock `[0xA758A4]`: +0x24 += 1000/30 ms, +0x28 += 1; read by AlarmControl 0x440915, Dirlight 0x527439 | Steps | TICK_ONLY both | static |
| 0x51BE10 LevelTimer, 0x51BED7 | nTick per step (script-visible) | Steps | TICK_ONLY | static |
| 0x4E6030 / 0x495CC0, sound system (SoundDefSound 0x4E8840, SoundDefTriggerOnce 0x4E9AA0, 0x4E8BD0, 0x4E9E20, 0x4EAC90, streaming 0x496AB0 / 0x4972B0 via 0xC28C5C and 0x54391C) | Sound timers | Reads the rate | none, already rate-aware | neutral |

### Cutscenes

| Guest address | What | Dependency | Fix | Status |
|---|---|---|---|---|
| CutScene 0x4F51D0, `inc [+0x64]` at 0x4F52F3 and `add` at 0x4F52E7 | nTick, script-visible (e.g. a trigger compared nTick with a 30 Hz tick count, so Josef's AI started 9 s early at 60 Hz) | Steps | TICK_ONLY both | verified A/B |
| 0x4F5400 cutscene positioning: time = nTick / `[0xA76C54]` (set to 30 by 0x4F65D0(30) at 0x415EF8); camera splines, keys, end (0x4F54D5), start 0x4F63F6 | Camera and timing | Steps through nTick | Hook `rt_tick_cutscene_time`: the call sees `nTick*rate + substeps*30` and ticks/s `30*rate`, i.e. time = nTick/30 plus the steps since the tick, so the camera moves every step; a written nTick (end) is converted back | verified A/B (full 90 s intro, screenshots identical at 10/19/20/60/89 s) |
| AnimCam 0x52A180, CameraControlTask 0x4E7C90, FreeCamera 0x531130 | Camera follow / look-at | none | — | neutral |
| AnimTask 0x4F3B40 | One recorded frame per step, sent as input (method 106) to vehicles and actors | Steps | GATE | verified A/B |
| AnimScaleTask 0x52AF10 | Frame accumulator | Steps | GATE | static |
| Cutscene camera shake 0x4F5DA1… | Random jitter per step from its own seed 0xA76C70 | Jitter frequency doubles | none (cosmetic) | known gap |

### Player (scope P, runs every step)

| Guest address | What | Dependency | Fix | Status |
|---|---|---|---|---|
| 0x45DCF0 HumanPlayerInput → method 106 0x410960 → body 0x4109A0 | Input, movement, view, weapon | Per step | Runs every step in the player scope (gate flags `GATE_NOT_PLAYER \| GATE_PLAYER_SCOPE` on 0x454EE0 / 0x45EEA0) | verified A/B |
| `[0xA54658]` animation units per step (0x4D2D30(30) from 0x415E41) | Copied to channel speed on start (0x4D503F, 0x4D6580); seconds → steps for blends (0x4D6130, 0x4D61D0, 0x4D62B0); advance 0x4D4B60 / 0x4D63B0 | Steps | 4800/rate inside the player scope | verified A/B (walk speed equal within one step) |
| Root motion 0x4D2FB0, ×`[0x56E1E8]` (1.75) at 0x410B90 / 0x410B9A / 0x410BAC | Walk velocity = animation delta × 1.75 | Follows the animation step | none (a scale, not a damping: an early POW here made walking 25 % slow) | verified A/B |
| Physics config loader 0x410070 | 0x410099 / 0x4100B3 jump speeds km/h → per step (exp +1, G); 0x4100CD air acceleration (exp +2, G); 0x410106 lean time ×30 (exp −1, G). Read only by player code | Per step | CONST | static (jump verified A/B) |
| Gravity / slides / pushes | 0x408477 airborne gravity (exp +2); 0x40858F landing speed ×30 (exp −1); 0x40DC00 / 12 / 2A slide 0.99 (POW); 0x40DC24 slide gravity (exp +2); 0x40DF04 push acceleration (exp +2); 0x40DF10 push damping (POW) | Per step | CONST, scope P | verified A/B (jump: same airtime and landing; apex 4 % lower, see gaps) |
| Slope slide 0x4614B0 | 0x461613 / 36, 0x46166E / 91 speeds (exp +1); 0x4616C9 / D3, 0x46171F / 29 accelerations (exp +2); 0.9 at 0x461780, 0x46179C, 0x4617B2, 0x4617F7, 0x461809, 0x46181F (POW) | Per step | CONST, scope P | static |
| Turn soft clamp 0x4610C0 (yaw), 0x464800 (pitch) | x / (2 − 16.67\|x\|) for \|x\| < 0.06 per step | Per-step input | 0x461345 / 0x464899 (0.06) exp +1; 0x461361 / 0x4648BD (16.67) exp −1 | verified A/B (mouse turn within 0.001) |
| Zoom 0x464934 / 46 / 6B / 7D | ×0.96 / ×1.0417 per step | Per step | POW, scope P | static |
| Scope approach 0x464A8D / AC1 / AF5 / B25 | ±3° per step | Per step | exp +1, scope P | static |
| View 0x464BD0 | Landing bob phase 0x464E21; eye height approach 0x46512E / 67 / 99, 0x4651D3 | Per step | exp +1, scope P | verified A/B (idle view) |
| Recoil 0x462E50 | 0x463049 / 0x46305F ×0.9 | Per step | POW, scope P | static |
| Fades and timers | 0x4087CD / FB ±1/300; 0x41124B −1/150; 0x411320 −1/45; 0x411380 −1/90; 0x411388 −1/60 | Per step | exp +1, scope P | static |
| Action durations (+0xD60 countdown) | 0x40C0BD, 0x40C4A2, 0x40C55A, 0x40C7F5, 0x40D100, 0x40D508, 0x40DCD0, 0x40F547; medipack 0x40FD0C | seconds ×30 | exp −1, scope P | static |
| Cooldown 0x412FBF `mov [edi+0xDE0], 45` | Per-step countdown | Steps | IMM exp −1, scope P | static |
| Grenade cook time `inc [+0x4E0]` 0x40F5ED, 0x40F670 | Throw power clamp [3, 60] (0x462F14), fuse 0x477124 | Steps | TICK_ONLY both | static |
| Camera shake 0x465970(human, x, y, steps) | +0x1C0 = 1/steps, faded per step by the view | Steps | Hook `rt_tick_camera_shake`: duration × rate/30 inside the player scope | static |
| InputPort 0x4ED010 keyboard / joystick axis ramps (`[i+0xBC20D0..E4]`, set by 0x4ED5E0) | Per-step ramp; axis applied per step | Per step | not fixed | known gap (turn keys unbound by default; the mouse is unaffected) |
| State-machine combo windows 0x4EC275 | Byte tables decremented per step | Steps | not fixed | known gap |
| Demo record / playback 0x45DD5C (0x2328 steps) | One input per step | Steps | not fixed | known gap (only valid at the recording rate) |

### Humans, AI and weapons (gated)

| Guest address | What | Dependency | Fix | Status |
|---|---|---|---|---|
| HumanAI 0x44F410 | Brain; calls the soldier body (method 106) at 0x44FA89; config ×30 (0x406FF6, 0x407010, 0x407036, 0x44EB01, 0x44EB2B, 0x44FC2A, 0x450EB6, 0x450F85, 0x450FBB, 0x452937, 0x457163, 0x45A0C3, 0x45B414, 0x45D9BF), 300-step timer 0x44F25A, countdown +0x3E10, `steps & 3` | Steps | GATE (soldier body and animation advance at 30 Hz, exactly as originally) | verified A/B (soldier positions match) |
| Human 0x45EEA0, 0x454EE0, HumanSoldier 0x45CF90 | Per-step event flags; child loop | Steps | GATE_CHILDREN (not for the player) | verified A/B |
| Gun 0x478640 (all guns) | Fire interval 0x479666 / 0x4796D6 / 0x479704, 0x478718 / 5E / C2 / 0x478818, +1/45 at 0x479738 | Steps | GATE_CHILDREN | verified A/B (same ammo after firing) |
| GunShot 0x47B690, GunCasing 0x4782D0, StationaryGun 0x474190, AIStationaryGun 0x46F390 / Holder 0x46E5B0, Gunx2 0x46DE70, M203 0x476340, Mine 0x475CC0, Grenade 0x476AC0, HumanCamera 0x482700 | Flight, physics, timers, perception | Steps | GATE (children where present) | static |

### Animation, physics and vehicles (gated or neutral)

| Guest address | What | Dependency | Fix | Status |
|---|---|---|---|---|
| Rigid-body integrator 0x4FC0E0 (gravity 44.6 pushed by callers, damping args, rest threshold) | No dt | Per step | Gate the callers: GenericPhysicsObj 0x43EC30, GenericPhysicsMagicObj 0x43E600, Car / Heli dynamics via AnimTask / CarAI input, Missile, Grenade | static |
| Heli 0x4316D0, Rotor 0x42D440, Wheel 0x4301C0, Afterburner 0x42BFA0 | Crash fall, spin / lift, suspension, smoothing | Per step | GATE_CHILDREN | verified A/B (Heli idle) |
| Gear 0x435930, Hatch 0x435E50, CockpitShield 0x4352A0, HeliDoor / CarDoor 0x4273B0, RocketPod 0x427A70, CarAI 0x425DE0, SplinePathGuideQTask 0x51AEB0, Fragment 0x4805B0, SimpointObj 0x503BB0, SimpointObjGenerator 0x43BCA0 | Smoothing, angle steps, countdowns, hidden ×30 | Per step | GATE | static |
| Missile 0x428390, SmoothQTask 0x51F340, NoiseQTask 0x520400, SimpointObjContainer 0x5035B0 | Same | Per step | GATE_CHILDREN | static |
| Train 0x42B700, Car 0x42E320, Elevator 0x43D280, Plane 0x433690 | Containers; motion comes from AnimTask / CarAI input; step guard | — | Not gated; the guard sees ticks | verified A/B (9 trains) |
| MagicObj 0x4D9970, BoneMagicObj 0x531620, EditRigidObj 0x4F2AC0, SectorObject 0x4DE4E0, MoveRigidObj 0x51FAA0, GraphNode 0x5188C0, PhysicsObj / Vehicle 0x4FB2B0 / 0x4F4180 | Containers or no time | — | none | neutral |

### World objects and effects

| Guest address | What | Dependency | Fix | Status |
|---|---|---|---|---|
| Door 0x446D10 | Swing / slide step = distance / (time × 30) (0x446F05, 0x446F68, 0x44701A, 0x44706C); nDoorOpenTicks `inc` 0x446E0F | Per step | CONST exp −1 (smooth); TICK_ONLY | static |
| RotatingObject 0x439090 | 0x43910F … 0x4392FC: accel and angle × 1/30 | Per step | exp +1 | static |
| RadarDish 0x448E30 | 0x448ECD / F16 / F47 | Per step | exp +1 | static |
| AlarmLightReflector 0x439B70 | 0x439BA1: 6° per step | Per step | exp +1 | static |
| Drawer 0x444260 | 0x44427E / 0x4442B5 open / close speed | Per step | exp +1 | static |
| SCameraLens 0x441770 | Pan / tilt steps, dwell, blink counters | Per step | GATE_CHILDREN | verified A/B |
| FlatSkyLayer / FlatWaterLayer 0x51DA90 | UV scroll per step | Per step | GATE (clouds step at 30 Hz) | verified A/B |
| AmbientArea 0x44C370, EnvironmentSound 0x436D10, Generator 0x43B500, Radio 0x43AF90, ExplodeObject 0x481970, ComputerMap 0x46A0D0, Smoke 0x437DB0 | Fades, countdowns, emitters | Per step | GATE_CHILDREN | verified A/B (AmbientArea, Smoke idle) |
| MineField 0x43ABC0, ComputerStartup 0x466C90, DeathZone 0x465DE0 (damage per step), Water 0x518C90, Particle2DDynCubeObjTask 0x4FD660, ExplosionSmoke 0x480420, FireBall 0x4806E0, Explosion0 0x480F40, FlashBang 0x47E820, GlassShatter 0x449E50, RainEffect 0x436990, RainDynCubeObj 0x4F7170, GunPickup 0x46CEA0 | Particles, lifetimes, damage, physics | Per step | GATE (exact; particles and effects animate at 30 Hz) | verified A/B (particles idle); others static |
| StatusScreen HUD 0x486CD0 | Blink phase += 1/15 per step (0x486CDA) | Per step | CONST exp +1 | verified A/B |
| StatusMessageScreen 0x485B40, StatusMessage 0x4855F0, FlashTask 0x530AD0, BandFilter 0x4A38A0, EditVariable 0x4889B0 | Typewriter, display hold, fades, script variables | Per step | GATE | verified A/B (idle) |
| FireBall draw 0x480790: spin += speed per drawn frame (0x480C3B) | Render-side | Per frame | not fixed (spins 2× at 60 fps) | known gap |
| Binocular overlay flicker 0x4719D0 | Per call, probably render-time | Per frame | not fixed | known gap |
| Switch, Cabinet, Terminal, SCamera, SCameraControl, AlarmControl, AlarmLight, Siren, ElevatorFloorIndicator, Computer, ComputerMenu, DefineComputerObjective, Glass, Fence, Wire, ConditionalSound, pickups, AreaActivate, BulletHole (wall clock), HandiCamFilter (wall clock) | Event or state driven | — | none | neutral |

### Constants not related to time

Grouped by constant:
- 1/24 (0x4C90C2, 0x4CA0E4, 0x4CD3BF, 0x4870A3): LOD and layout.
- 1/4800 (0x4D74E7, 0x40C554): animation unit.
- 1/180 (0x51EBE2): degrees.
- 60.0 at 0x46165D: an angle; 60.0 at 0x533490: seconds per minute.
- 16.67 (0x461361, 0x4648BD): handled above as the soft clamp.
- 1/300 at 0x419904 and 0x419927 (menu BackgroundFX): menus run at 60 anyway.
- 0x410CD8, 0x455274, 0x45D1F3: ×30 for debug text.

### Randomness

There is no CRT rand. The engine RNG is 0x4B4740 / 0x4B4770 / 0x4B47C0, used with
seeds such as `[game+0x2C]` (0x416D20). Every per-step probability roll that was
checked is event-driven, not polled per step. In the intro run with the gates,
the game seed was consumed by the same callers at both rates. The only difference
in call counts (AI delays, 0x450EA8) came from the AI divergence that the
cutscene nTick fix removed.

## Verification

**Harness.** `IGI_TICK_TEST` (shim/src/tick_test.c), `tools/tick_ab.sh` and
`tools/tick_compare.py` run the same scenario at several rates.
- The clock is deterministic (`IGI_FAKE_CLOCK`) and the benchmark driver goes
  into mission 1.
- Input is scripted in game time (t = steps / rate since gameplay started).
- The harness writes per-step traces of watched values, memory snapshots and
  screenshots at equal game time.
- `tick_compare.py objects` compares every game object (the task tree) of two
  snapshots, field by field and class by class.

**Results at 60 Hz against 30 Hz** (mission 1, Mac):

- **Idle gameplay.**
  - Before the fixes, about 30 classes diverged: trains, heli, AI, soldiers,
    security cameras, smoke, particles, clouds, sound areas, cutscene, HUD.
  - After them, every object matches except:
    - clock bookkeeping (Flow, Game);
    - render stamps (Human +0x7C4) and the animation dirty flag;
    - float noise in soldiers' gun attachments (1e-5);
    - the HUD blink phase (a half-step offset);
    - the end value of the landing-bob phase;
    - sound channel state, which differs between two runs at the same rate too.
- **Walking, mouse turn, strafe, back.** Player position within about 200 units of
  23,000 units/s walking (a constant half-step lead). Orientation within 0.001.
  - Without the animation-step fix, walking was 2.2× too fast (no fixes) or 0.75×
    (with an early wrong damping scale).
- **Jump.** Same airtime and landing time. The apex is 4 % lower at 60 Hz: Euler
  integration with half steps, inherent to the smaller step.
- **Firing.** The player's weapon state (ammo) is identical after 2 s of fire.
- **Intro cutscene, 90 s.**
  - Before: twice as fast.
  - After: the cutscene chain finishes at the same time (nTick 559 = 18.63 s at
    both rates), and the screenshots at 10, 19.2, 20.4, 60 and 89 s show the same
    shot, subtitle and actors.
- **30 Hz unchanged.** A 30 Hz run with the fixes compiled in has every game
  object identical to a run without them, apart from sound channel state, which
  differs run to run anyway.

## Known gaps and possible next steps

- **Soldiers move at 30 Hz steps.** Their brain (HumanAI) drives their body, so
  gating the brain also gates the body. They keep their exact speed, but their
  motion is not smoothed at 60 fps. Smoothing them needs HumanAI ungated with the
  AI constants above scaled, or the body split from the brain.
- **Other 30 Hz-stepped visuals.** Vehicles (AnimTask input), particles, smoke,
  explosions, clouds and security cameras are exact but update at 30 Hz.
- **Integration differences.** Jump apex 4 % lower; player slope and push physics
  use exact per-step scaling, but the discrete integration differs slightly.
- **Not fixed:**
  - InputPort keyboard / joystick turn ramps (unbound by default);
  - state-machine combo windows 0x4EC275;
  - demo record / playback;
  - FireBall sprite spin and binocular flicker (render-side, per frame);
  - cutscene camera shake frequency.
- **Savegames.** Counters saved in steps (e.g. nDoorOpenTicks, nPlayTick) are in
  30 Hz ticks where the counter itself was fixed; others are not checked.
- **Not exercised by the A/B runs:** doors, elevators, drawers, rotating objects,
  vehicles driven by the player, explosions, mines and later missions. Their
  fixes come from the static inventory only.
