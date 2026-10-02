"""Update-rate fixes for the recompiler (see docs/tick_rate.md, runtime/src/tick_rate.c).

The game updates in fixed steps of 1/30 s during gameplay and much of its code
counts in those steps. With a different gameplay rate (IGI_GAME_HZ / igi.cfg
game_hz) the runtime keeps every such dependency at its 30 Hz real-time speed.
All of it is inert at 30 Hz: the original code runs unchanged.

GATES     guest functions (per-step update handlers of logic objects) that keep
          running 30 times a second of game time: at a higher rate the call is
          skipped on the steps in between. Flags:
            GATE_CHILDREN    the handler also updates the object's children (the
                             inlined child loop of 0x488700); a skipped call still
                             updates them through 0x488700.
            GATE_NOT_PLAYER  never gated for the player's HumanPlayer object.
            GATE_PLAYER_SCOPE  the call for the player's HumanPlayer object runs
                             in the player scope (player-scope constants scaled,
                             animation step set for the rate).
          Container updates (Game, Level, Dynamic, ConditionalContainer, ...)
          are never gated: they set up context their children rely on.
CONSTS    instructions whose absolute memory operand is a constant that encodes
          the step length; the operand is redirected to a slot. Value: (mode, scope)
            mode   an integer exp: value * (30 / hz) ** exp (-1 "seconds * 30 =
                   steps", +1 "per second / 30 = per step", +2 per step squared);
                   POW: value ** (30 / hz) (a factor applied once per step).
            scope  G: whenever gameplay runs at another rate; P: only inside the
                   player scope (code shared with gated soldiers).
IMMS      the same for an immediate operand (integer, rounded).
TICK_ONLY instructions (increments of per-step counters) executed on 30 Hz ticks only.
HOOKS     guest functions with a host replacement that may handle the call.
"""
import struct

from capstone import x86 as X

# First guest address of the slots (top page of the host scratch area, see
# GUEST_TICK_CONSTS in runtime/include/runtime.h), 8 bytes each.
SLOT_BASE = 0x003FF000
SLOT_END = 0x00400000

GATE_CHILDREN = 1
GATE_NOT_PLAYER = 2
GATE_PLAYER_SCOPE = 4
C = GATE_CHILDREN

G, P = 0, 1          # scopes
POW = 'pow'

GATES = {
    # Flow, timers, HUD (docs/tick_rate.md: flow)
    0x004889B0: (0,),   # EditVariable: script variable += expression per step
    0x00485B40: (0,),   # StatusMessageScreen: typewriter / open animation, display hold
    0x004855F0: (0,),   # StatusMessage: ticks since displayed
    0x00530AD0: (0,),   # FlashTask: keyframed fade, per-step increments
    0x004A38A0: (0,),   # BandFilter: per-step decay, random step intervals
    # Humans, AI, weapons
    0x0044F410: (0,),   # HumanAI: brain; also drives the soldier body (method 106)
    0x0045EEA0: (C | GATE_NOT_PLAYER,),   # Human: per-step event flags reset
    0x00454EE0: (C | GATE_NOT_PLAYER | GATE_PLAYER_SCOPE,),   # HumanPlayer/HumanSoldier* update
    0x0045CF90: (C,),   # HumanSoldier update
    0x00478640: (C,),   # Gun (all guns): fire interval, reload, recoil timers
    0x0047B690: (0,),   # GunShot: bullet flight
    0x004782D0: (0,),   # GunCasing: ejected casing physics
    0x00474190: (C,),   # StationaryGun
    0x0046F390: (C,),   # AIStationaryGun
    0x0046E5B0: (C,),   # AIStationaryGunHolder (perception)
    0x0046DE70: (C,),   # Gunx2
    0x00476340: (0,),   # M203 round physics
    0x00475CC0: (C,),   # Mine
    0x00482700: (0,),   # HumanCamera (death / external camera)
    # Animation, physics, vehicles
    0x004F3B40: (0,),   # AnimTask: one recorded frame per step (drives vehicles)
    0x0052AF10: (0,),   # AnimScaleTask
    0x0043EC30: (0,),   # GenericPhysicsObj physics step (rigid body integrator)
    0x0043E600: (0,),   # GenericPhysicsMagicObj physics step
    0x00425DE0: (0,),   # CarAI: path commands, step counter
    0x004316D0: (C,),   # Heli: crash fall, countdowns
    0x0042D440: (C,),   # Rotor: spin, lift
    0x004301C0: (C,),   # Wheel: suspension
    0x0042BFA0: (C,),   # Afterburner: smoothing, flicker
    0x00435930: (0,),   # Gear: per-step smoothing
    0x00435E50: (0,),   # Hatch: per-step smoothing
    0x004352A0: (0,),   # CockpitShield: per-step smoothing
    0x004273B0: (0,),   # HeliDoor / CarDoor: angle += speed per step
    0x00427A70: (0,),   # RocketPod: fire interval countdown
    0x00428390: (C,),   # Missile: step counter, physics
    0x00476AC0: (C,),   # Grenade: fuse countdown, physics
    0x0051AEB0: (0,),   # SplinePathGuideQTask: path speed per step
    0x0051F340: (C,),   # SmoothQTask: value ramps per step
    0x00520400: (C,),   # NoiseQTask: ramps, random step countdown
    0x004805B0: (0,),   # Fragment
    0x00503BB0: (0,),   # SimpointObj: lifetime countdown
    0x0043BCA0: (0,),   # SimpointObjGenerator: spawn interval (hidden x30)
    0x005035B0: (C,),   # SimpointObjContainer: debris integrator
    # World objects, effects
    0x00441770: (C,),   # SCameraLens: pan/tilt steps, dwell, blink counters
    0x0051DA90: (0,),   # FlatSkyLayer / FlatWaterLayer: UV scroll per step
    0x0046CEA0: (0,),   # GunPickup: dropped weapon fall
    0x0044C370: (C,),   # AmbientArea: volume fades, retrigger interval
    0x00436D10: (C,),   # EnvironmentSound: volume fades
    0x0043B500: (C,),   # Generator: start delay
    0x0043AF90: (C,),   # Radio: chatter countdown
    0x0043ABC0: (0,),   # MineField: re-arm delay
    0x00481970: (C,),   # ExplodeObject: fuse countdown
    0x00466C90: (0,),   # ComputerStartup: stage timer
    0x0046A0D0: (C,),   # ComputerMap: flicker, refresh countdown, key repeat
    0x00465DE0: (0,),   # DeathZone: damage per step
    0x00518C90: (0,),   # Water: per-step counter
    0x004FD660: (0,),   # Particle2DDynCubeObjTask: particle integration, lifetimes
    0x00437DB0: (C,),   # Smoke: emission accumulator, particle parameters
    0x00480420: (0,),   # ExplosionSmoke: life countdown, emissions
    0x004806E0: (0,),   # FireBall: delay, life, sprite frame
    0x00480F40: (0,),   # Explosion0: burst delay / interval / count
    0x0047E820: (0,),   # FlashBang
    0x00449E50: (0,),   # GlassShatter: shard physics, lifetime
    0x00436990: (0,),   # RainEffect (engine rain 0x499cf0)
    0x004F7170: (0,),   # RainDynCubeObj
}

CONSTS = {
    # Cutscene clock and mission clock
    0x00489492: (1, G),     # LevelFlow clock: tick / (GetRate * 3600) -> ticks stay 30 Hz
    0x00486CDA: (1, G),     # StatusScreen (HUD) blink phase += 1/15 per step
    # Moving world objects (updated every step: smooth motion)
    0x00446F05: (-1, G),    # Door: swing step = angle / (time * 30)
    0x00446F68: (-1, G),
    0x0044701A: (-1, G),    # Door: slide step = distance / (time * 30)
    0x0044706C: (-1, G),
    0x0043910F: (1, G),     # RotatingObject: acceleration and angle per second / 30
    0x00439155: (1, G),
    0x004391B0: (1, G),
    0x004391F6: (1, G),
    0x00439251: (1, G),
    0x00439297: (1, G),
    0x004392C8: (1, G),
    0x004392E4: (1, G),
    0x004392FC: (1, G),
    0x00448ECD: (1, G),     # RadarDish: same
    0x00448F16: (1, G),
    0x00448F47: (1, G),
    0x00439BA1: (1, G),     # AlarmLightReflector: 6 degrees per step
    0x0044427E: (1, G),     # Drawer: open speed per step
    0x004442B5: (1, G),     # Drawer: close speed per step
    # Player physics config (loaded once; read only by the player's body)
    0x00410099: (1, G),     # jump forward speed km/h -> units per step
    0x004100B3: (1, G),     # jump up speed km/h -> units per step
    0x004100CD: (2, G),     # air acceleration -> units per step^2
    0x00410106: (-1, G),    # lean time seconds -> steps
    # Player body (player scope)
    0x00408477: (2, P),     # airborne gravity per step^2
    0x0040858F: (-1, P),    # landing speed per step -> per second
    0x0040DC00: (POW, P),   # slide damping 0.99 per step
    0x0040DC12: (POW, P),
    0x0040DC2A: (POW, P),
    0x0040DC24: (2, P),     # slide gravity per step^2
    0x0040DF04: (2, P),     # push acceleration per step^2
    0x0040DF10: (POW, P),   # push damping 0.99 per step
    0x00461613: (1, P),     # slope slide speed per step
    0x00461636: (1, P),
    0x0046166E: (1, P),
    0x00461691: (1, P),
    0x004616C9: (2, P),     # slope slide acceleration per step^2
    0x004616D3: (2, P),
    0x0046171F: (2, P),
    0x00461729: (2, P),
    0x00461780: (POW, P),   # slope slide damping 0.9 per step
    0x0046179C: (POW, P),
    0x004617B2: (POW, P),
    0x004617F7: (POW, P),
    0x00461809: (POW, P),
    0x0046181F: (POW, P),
    0x00461345: (1, P),     # yaw soft clamp threshold (per-step turn)
    0x00461361: (-1, P),
    0x00464899: (1, P),     # pitch soft clamp threshold (per-step turn)
    0x004648BD: (-1, P),
    0x00464934: (POW, P),   # zoom in / out factor per step
    0x00464946: (POW, P),
    0x0046496B: (POW, P),
    0x0046497D: (POW, P),
    0x00464A8D: (1, P),     # scope approach angle per step
    0x00464AC1: (1, P),
    0x00464AF5: (1, P),
    0x00464B25: (1, P),
    0x00464E21: (1, P),     # landing bob phase per step
    0x0046512E: (1, P),     # eye height approach per step
    0x00465167: (1, P),
    0x00465199: (1, P),
    0x004651D3: (1, P),
    0x00463049: (POW, P),   # recoil decay 0.9 per step
    0x0046305F: (POW, P),
    0x004087CD: (1, P),     # +0xdc0 ramp +-1/300 per step
    0x004087FB: (1, P),
    0x0041124B: (1, P),     # +0xdc8 fade -1/150 per step
    0x00411320: (1, P),     # fades -1/45 per step
    0x00411380: (1, P),     # fade -1/90 per step
    0x00411388: (1, P),     # fade -1/60 per step
    0x0040C0BD: (-1, P),    # action durations seconds -> steps (+0xd60 countdown)
    0x0040C4A2: (-1, P),
    0x0040C55A: (-1, P),
    0x0040C7F5: (-1, P),
    0x0040D100: (-1, P),
    0x0040D508: (-1, P),
    0x0040DCD0: (-1, P),
    0x0040F547: (-1, P),
    0x0040FD0C: (-1, P),    # medipack use interval
}

IMMS = {
    0x00412FBF: (-1, P),    # player cooldown of 45 steps
}

# Per-step counters that must advance once per 30 Hz tick.
TICK_ONLY = {
    0x0048936B,   # LevelFlow mission tick [LevelFlow+0x68] ("%s.nTick")
    0x004F05F2,   # Level clock: ms += 1000/30
    0x004F05FC,   # Level clock: steps += 1
    0x0051BED7,   # LevelTimer nTick
    0x004F52F3,   # CutScene nTick ("%s.nTick", read by scripts), see rt_tick_cutscene_time
    0x004F52E7,   # CutScene nTick += speed expression * 30
    0x00446E0F,   # Door nDoorOpenTicks
    0x0040F5ED,   # player grenade cook time (throw power, fuse)
    0x0040F670,
}

HOOKS = {
    0x004028B0: 'rt_tick_step_count',   # GetStepCount: 30 Hz ticks
    0x00465970: 'rt_tick_camera_shake',  # camera shake: duration in steps
    0x004F5400: 'rt_tick_cutscene_time',  # cutscene time = nTick / 30, plus the steps since the tick
}

KIND_INT, KIND_FLOAT = 0, 1
MODE_EXP, MODE_POW = 0, 1


def build(an):
    """Slot assignment: ({insn: slot} for memory operands, {insn: slot} for
    immediates, [(slot, orig_addr, orig_bits, size, kind, mode, exp, scope)])."""
    slots, by_key, mem, imm = [], {}, {}, {}

    def slot_for(key, *fields):
        if key not in by_key:
            a = SLOT_BASE + 8 * len(slots)
            assert a + 8 <= SLOT_END, 'too many update-rate slots'
            by_key[key] = a
            slots.append((a,) + fields)
        return by_key[key]

    for a, (how, scope) in sorted(CONSTS.items()):
        ins = an.decode(a)
        mop = [o for o in ins.ops if o.type == X.X86_OP_MEM]
        assert len(mop) == 1 and not mop[0].mem.base and not mop[0].mem.index, '%08X: %s' % (a, ins.text)
        orig = mop[0].mem.disp & 0xFFFFFFFF
        size = mop[0].size
        kind = KIND_INT if not ins.mnem.startswith('f') or ins.mnem.startswith('fi') else KIND_FLOAT
        mode, exp = (MODE_POW, 0) if how == POW else (MODE_EXP, how)
        bits = int.from_bytes(an.pe.read(orig, size), 'little')
        mem[a] = slot_for(('m', orig, size, kind, mode, exp, scope), orig, bits, size, kind, mode, exp, scope)
    for a, (exp, scope) in sorted(IMMS.items()):
        ins = an.decode(a)
        iop = [o for o in ins.ops if o.type == X.X86_OP_IMM]
        assert len(iop) == 1, '%08X: %s' % (a, ins.text)
        v = iop[0].imm & 0xFFFFFFFF
        imm[a] = slot_for(('i', v, exp, scope), 0, v, 4, KIND_INT, MODE_EXP, exp, scope)
    for a in TICK_ONLY:
        ins = an.decode(a)
        assert ins.mnem in ('inc', 'add'), '%08X: %s' % (a, ins.text)
    return mem, imm, slots


def write_table(path, slots):
    with open(path, 'w') as f:
        f.write('/* Generated by tools/recomp/gen.py - do not edit. */\n#include "recomp_gen.h"\n\n')
        f.write('/* Update-rate constant slots (tools/recomp/tick_rate.py): address, original\n'
                ' * address (0: immediate), original bits, size, kind (0 int, 1 float), mode\n'
                ' * (0 exponent, 1 power), exponent, scope (0 gameplay, 1 player). */\n')
        f.write('const TickSlot g_tick_slots[] = {\n')
        for a, orig, bits, size, kind, mode, exp, scope in slots:
            f.write('    { 0x%08Xu, 0x%08Xu, 0x%016Xull, %d, %d, %d, %d, %d },\n'
                    % (a, orig, bits, size, kind, mode, exp, scope))
        if not slots:
            f.write('    { 0u, 0u, 0ull, 0, 0, 0, 0, 0 },\n')
        f.write('};\nconst uint32_t g_tick_slot_count = %du;\n' % len(slots))


if __name__ == '__main__':
    # Lists the redirected constants with their values (sanity check).
    import sys
    from analyze import Analyzer
    an = Analyzer(sys.argv[1])
    mem, imm, slots = build(an)
    for a, orig, bits, size, kind, mode, exp, scope in slots:
        v = bits
        if kind == KIND_FLOAT:
            v = struct.unpack('<f' if size == 4 else '<d', bits.to_bytes(size, 'little'))[0]
        print('%08X  orig %08X  %r  %s scope %d' % (a, orig, v, 'pow' if mode else 'exp %d' % exp, scope))
