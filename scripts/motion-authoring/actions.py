#!/usr/bin/env python3
"""Procedural humanoid animation library, authored against the calibrated
Mixamo local-axis conventions (probe renders, 2026-09-08):

  arm    X+ = swing down toward body     Z+ = swing forward   Y+ = twist
  forearm Z+ = elbow curl (anatomical flexion); other axes unused
  upleg  X+ = thigh raise forward        Z+ = leg out to the side
  leg    X+ = knee flexion (heel back)
  foot   X+ = toe down (plantarflex)
  spine  X+ = bend forward   Y+ = torso twist   Z+ = side bend
  head   X+ = look down      Y+ = turn          Z+ = tilt
  hips   X+ = whole-body forward lean  Y+ = yaw  Z+ = lateral roll

All angles are ANATOMICAL deltas on the bind pose (T-pose): the author layer
mirrors Y and Z for right-side joints (the Mixamo right bind frames are
mirrored, so raw same-sign Y/Z would move the right side the opposite way —
numerically verified; X is symmetric as-is).

Each action returns (seconds, pose_fn, hips_translate_fn|None).
pose_fn(t01) -> {joint: (x,y,z) degrees}. t01 in [0,1]; clips are LOOPED by
the app, so pose_fn(0) == pose_fn(1) for cyclic actions. Clip frame 0 is the
retarget's delta reference — start near a calm/neutral pose.
"""
import math

TWO_PI = 2.0 * math.pi


def _s(t, ph=0.0):
    return math.sin(TWO_PI * t + ph)


def _c(t, ph=0.0):
    return math.cos(TWO_PI * t + ph)


def _bump(x):
    """Smooth 0->1->0 bump for x in [0,1] (0 outside)."""
    if x <= 0.0 or x >= 1.0:
        return 0.0
    return 0.5 - 0.5 * math.cos(TWO_PI * x)


def _ease(x):
    """Smoothstep 0..1."""
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


def _seg(t, a, b):
    """Normalized position of t inside [a,b], clamped."""
    if b <= a:
        return 0.0
    return max(0.0, min(1.0, (t - a) / (b - a)))


# ---------------------------------------------------------------------------
# WALK — 2 full strides over the clip (1 cycle = 1s at 2 steps).
# ---------------------------------------------------------------------------
def walk():
    seconds = 2.0
    cycles = 2.0            # two full gait cycles

    def pose(t):
        p = cycles * t      # gait phase in cycles
        swingL = _s(p)      # +1 = left thigh forward
        swingR = _s(p, math.pi)
        # knee flexes during the leg's swing (thigh moving forward)
        kneeL = 40.0 * _bump((p % 1.0))            # left swing first half
        kneeR = 40.0 * _bump(((p + 0.5) % 1.0))
        return {
            "l_upleg": (24.0 * swingL - 4.0, 0, 0),
            "r_upleg": (24.0 * swingR - 4.0, 0, 0),
            "l_leg": (kneeL + 6.0, 0, 0),
            "r_leg": (kneeR + 6.0, 0, 0),
            "l_foot": (-8.0 * swingL, 0, 0),
            "r_foot": (-8.0 * swingR, 0, 0),
            # arms hang (X+78) and swing opposite to their leg
            "l_arm": (78.0, 0, 16.0 * swingR),
            "r_arm": (78.0, 0, 16.0 * swingL),
            "l_forearm": (0, 0, 14.0 + 8.0 * swingR),
            "r_forearm": (0, 0, 14.0 + 8.0 * swingL),
            "spine": (4.0, 5.0 * swingL, 0),
            "spine1": (2.0, 3.0 * swingL, 0),
            "head": (-3.0, -4.0 * swingL, 0),
            "hips": (2.0, -6.0 * swingL, 2.0 * _s(p, math.pi / 2)),
        }

    def hips_tr(t):
        p = cycles * t
        return (0.0, -0.025 + 0.02 * abs(_c(p)), 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# RUN — faster, deeper knees, forward lean, bigger arm drive.
# ---------------------------------------------------------------------------
def run():
    # Whole number of gait cycles — the app LOOPS clips, so a fractional
    # cycle count snaps the legs/hips at the wrap point. Cadence kept at
    # 1.5625 cycles/s (the original 2.5-cycles-in-1.6s feel).
    seconds = 1.28
    cycles = 2.0

    def pose(t):
        p = cycles * t
        swingL = _s(p)
        swingR = _s(p, math.pi)
        kneeL = 85.0 * _bump(p % 1.0)
        kneeR = 85.0 * _bump((p + 0.5) % 1.0)
        return {
            "l_upleg": (38.0 * swingL + 6.0, 0, 0),
            "r_upleg": (38.0 * swingR + 6.0, 0, 0),
            "l_leg": (kneeL + 12.0, 0, 0),
            "r_leg": (kneeR + 12.0, 0, 0),
            "l_foot": (-10.0 * swingL + 5.0, 0, 0),
            "r_foot": (-10.0 * swingR + 5.0, 0, 0),
            "l_arm": (74.0, 0, 46.0 * swingR),
            "r_arm": (74.0, 0, 46.0 * swingL),
            "l_forearm": (0, 0, 70.0 + 22.0 * swingR),
            "r_forearm": (0, 0, 70.0 + 22.0 * swingL),
            "spine": (14.0, 7.0 * swingL, 0),
            "spine1": (6.0, 4.0 * swingL, 0),
            "head": (-10.0, -5.0 * swingL, 0),
            "hips": (7.0, -8.0 * swingL, 2.5 * _s(p, math.pi / 2)),
        }

    def hips_tr(t):
        p = cycles * t
        return (0.0, -0.03 + 0.045 * abs(_s(p)), 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# IDLE — subtle breathing sway, weight shift.
# ---------------------------------------------------------------------------
def idle():
    seconds = 3.0

    def pose(t):
        b = _s(t, -math.pi / 2)          # breath, starts at minimum
        w = _s(2 * t)                    # slow weight shift
        look = _bump(_seg(t, 0.45, 0.85))   # brief glance to the side
        return {
            "l_arm": (72.0 + 4.0 * b, 0, 4.0 + 2.0 * b),
            "r_arm": (72.0 + 4.0 * b, 0, 4.0 + 2.0 * b),
            "l_forearm": (0, 0, 12.0 + 5.0 * b),
            "r_forearm": (0, 0, 12.0 + 5.0 * b),
            "spine": (2.5 + 2.5 * b, 3.0 * w, 1.5 * w),
            "spine1": (1.5 + 1.5 * b, 2.0 * w, 0),
            "head": (-2.0 - 2.0 * b + 3.0 * look, 5.0 * w + 22.0 * look, 0),
            "hips": (0, 2.0 * w, 2.0 * w),
            "l_upleg": (-1.5, 0, 2.0 + 1.0 * w),
            "r_upleg": (-1.5, 0, 2.0 - 1.0 * w),
            "l_leg": (3.0, 0, 0),
            "r_leg": (3.0, 0, 0),
        }

    def hips_tr(t):
        b = _s(t, -math.pi / 2)
        w = _s(2 * t)
        return (0.006 * w, 0.006 * b, 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# WAVE — right arm raised overhead, forearm waves side to side.
# ---------------------------------------------------------------------------
def wave():
    seconds = 2.4
    up_t, down_t = 0.18, 0.85            # raise / lower windows

    def pose(t):
        raise01 = _ease(_seg(t, 0.0, up_t)) * (1.0 - _ease(_seg(t, down_t, 1.0)))
        wig = _s(3.0 * _seg(t, up_t, down_t)) * raise01
        return {
            # left arm hangs relaxed
            "l_arm": (84.0, 0, 2.0),
            "l_forearm": (0, 0, 10.0),
            # right arm: hanging (X 84, close to the body) -> straight overhead
            # (X -84). A splayed arm (the old 72 -> -58) sits near the
            # retarget's shoulder singularity and inverts.
            "r_arm": (84.0 - 140.0 * raise01 + 14.0 * wig, 0, 78.0 * raise01),
            # the visible wave: forearm curls in-and-out around a half-bent
            # elbow while the arm is up
            "r_forearm": (0, 0, 18.0 * raise01 + 46.0 * wig),
            "r_hand": (0, 0, 12.0 * wig),
            "spine": (0, -4.0 * raise01, -4.0 * raise01),
            "head": (0, 6.0 * raise01, 5.0 * raise01),
            "l_upleg": (0, 0, 1.5),
            "r_upleg": (0, 0, 1.5),
        }

    return seconds, pose, None


# ---------------------------------------------------------------------------
# JUMP — anticipation crouch, extend, airborne tuck, land, recover.
# ---------------------------------------------------------------------------
def jump():
    seconds = 1.6
    # phases: 0-0.25 crouch, 0.25-0.4 launch, 0.4-0.65 air, 0.65-0.8 land, ->1 recover

    def pose(t):
        crouch = _ease(_seg(t, 0.0, 0.22)) * (1.0 - _ease(_seg(t, 0.22, 0.38)))
        launch = _ease(_seg(t, 0.22, 0.38)) * (1.0 - _ease(_seg(t, 0.55, 0.75)))
        air = _bump(_seg(t, 0.35, 0.72))
        land = _bump(_seg(t, 0.68, 0.95))
        legX = 55.0 * crouch + 30.0 * air + 45.0 * land
        kneeX = 80.0 * crouch + 45.0 * air + 70.0 * land
        return {
            "l_upleg": (legX, 0, 2.0),
            "r_upleg": (legX, 0, 2.0),
            "l_leg": (kneeX, 0, 0),
            "r_leg": (kneeX, 0, 0),
            "l_foot": (-10.0 * crouch + 25.0 * launch - 12.0 * land, 0, 0),
            "r_foot": (-10.0 * crouch + 25.0 * launch - 12.0 * land, 0, 0),
            # arms swing back in crouch, up during launch/air
            "l_arm": (72.0 + 12.0 * crouch - 95.0 * air, 0,
                      -28.0 * crouch + 20.0 * launch),
            "r_arm": (72.0 + 12.0 * crouch - 95.0 * air, 0,
                      -28.0 * crouch + 20.0 * launch),
            "l_forearm": (0, 0, 12.0 + 15.0 * crouch),
            "r_forearm": (0, 0, 12.0 + 15.0 * crouch),
            "spine": (18.0 * crouch - 6.0 * air + 14.0 * land, 0, 0),
            "head": (-10.0 * crouch + 6.0 * air - 8.0 * land, 0, 0),
            "hips": (6.0 * crouch + 4.0 * land, 0, 0),
        }

    def hips_tr(t):
        crouch = _ease(_seg(t, 0.0, 0.22)) * (1.0 - _ease(_seg(t, 0.22, 0.38)))
        air = _bump(_seg(t, 0.3, 0.78))
        land = _bump(_seg(t, 0.68, 0.95))
        return (0.0, -0.16 * crouch + 0.30 * air - 0.10 * land, 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# PUNCH — boxing guard, right straight, return to guard.
# ---------------------------------------------------------------------------
def punch():
    seconds = 1.4

    def pose(t):
        guard_in = _ease(_seg(t, 0.0, 0.18))
        # trapezoid strike: extend, HOLD at full extension, retract — the
        # plateau survives the retarget's smoothing pass where a narrow
        # bump got averaged away
        strike = _ease(_seg(t, 0.26, 0.42)) * (1.0 - _ease(_seg(t, 0.6, 0.78)))
        guard_out = _ease(_seg(t, 0.82, 1.0))
        guard = guard_in * (1.0 - guard_out)
        return {
            # guard: arms down-forward, elbows curled hard (fists up)
            "l_arm": (84.0 - 20.0 * guard, 0, 26.0 * guard),
            "l_forearm": (0, 0, 100.0 * guard),
            # right arm: guard -> extended straight forward
            "r_arm": (84.0 - 26.0 * guard - 58.0 * strike, 0,
                      20.0 * guard + 74.0 * strike),
            "r_forearm": (0, 0, 105.0 * guard * (1.0 - strike) + 8.0 * strike),
            "spine": (6.0 * guard, -9.0 * strike, 0),
            "spine1": (2.0 * guard, -4.0 * strike, 0),
            "hips": (2.0 * guard, -4.0 * strike, 0),
            "head": (2.0 * guard - 2.0 * strike, 4.0 * strike, 0),
            # slight stance: left leg forward, knees soft
            "l_upleg": (14.0 * guard, 0, 3.0 * guard),
            "r_upleg": (-6.0 * guard, 0, 3.0 * guard),
            "l_leg": (12.0 * guard, 0, 0),
            "r_leg": (14.0 * guard, 0, 0),
        }

    def hips_tr(t):
        guard = _ease(_seg(t, 0.0, 0.18)) * (1.0 - _ease(_seg(t, 0.82, 1.0)))
        return (0.0, -0.04 * guard, 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# KICK — right front kick: chamber, extend, retract.
# ---------------------------------------------------------------------------
def kick():
    seconds = 1.4

    def pose(t):
        prep = _ease(_seg(t, 0.0, 0.2)) * (1.0 - _ease(_seg(t, 0.82, 1.0)))
        chamber = _bump(_seg(t, 0.12, 0.46))
        # HOLD the extension instead of a narrow bump: the strike used to last
        # ~0.1s, so it was invisible between sampled frames (and the 12fps
        # smooth-bake in the generate path averaged it away).
        extend = _ease(_seg(t, 0.36, 0.52)) * (1.0 - _ease(_seg(t, 0.66, 0.82)))
        return {
            # right leg: thigh up (chamber+extend), knee folds then snaps out
            "r_upleg": (52.0 * chamber + 78.0 * extend, 0, 4.0 * prep),
            "r_leg": (100.0 * chamber * (1.0 - extend) + 3.0 * extend, 0, 0),
            "r_foot": (15.0 * extend, 0, 0),
            # support leg braces
            "l_upleg": (-6.0 * prep, 0, 3.0 * prep),
            "l_leg": (10.0 * prep, 0, 0),
            # arms in loose guard, counter-swing
            "l_arm": (82.0, 0, 22.0 * prep + 10.0 * extend),
            "l_forearm": (0, 0, 70.0 * prep),
            "r_arm": (82.0, 0, 14.0 * prep - 14.0 * extend),
            "r_forearm": (0, 0, 55.0 * prep),
            "spine": (-4.0 * extend + 4.0 * prep, 8.0 * extend, 0),
            "hips": (-6.0 * extend + 2.0 * prep, 6.0 * extend, 0),
            "head": (-2.0 * prep, -4.0 * extend, 0),
        }

    def hips_tr(t):
        prep = _ease(_seg(t, 0.0, 0.2)) * (1.0 - _ease(_seg(t, 0.75, 1.0)))
        return (0.0, -0.03 * prep, 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# MARCH — exaggerated military march in place, 2 cycles.
# ---------------------------------------------------------------------------
def march():
    seconds = 2.0
    cycles = 2.0

    def pose(t):
        p = cycles * t
        swingL = _s(p)
        swingR = _s(p, math.pi)
        liftL = max(0.0, swingL)
        liftR = max(0.0, swingR)
        return {
            "l_upleg": (55.0 * liftL - 3.0, 0, 0),
            "r_upleg": (55.0 * liftR - 3.0, 0, 0),
            "l_leg": (70.0 * liftL + 4.0, 0, 0),
            "r_leg": (70.0 * liftR + 4.0, 0, 0),
            "l_foot": (5.0 * liftL, 0, 0),
            "r_foot": (5.0 * liftR, 0, 0),
            # stiff straight arm swing, opposite the leg
            "l_arm": (80.0, 0, 34.0 * swingR),
            "r_arm": (80.0, 0, 34.0 * swingL),
            "l_forearm": (0, 0, 8.0 + 6.0 * max(0.0, swingR)),
            "r_forearm": (0, 0, 8.0 + 6.0 * max(0.0, swingL)),
            "spine": (-3.0, 0, 0),           # chest up
            "head": (2.0, 0, 0),
            "hips": (0, 0, 2.0 * _s(p, math.pi / 2)),
        }

    def hips_tr(t):
        p = cycles * t
        return (0.0, -0.02 + 0.02 * abs(_c(p)), 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# CHEER — both arms thrown up twice, small hop.
# ---------------------------------------------------------------------------
def cheer():
    seconds = 2.0

    def pose(t):
        up1 = _bump(_seg(t, 0.05, 0.5))
        up2 = _bump(_seg(t, 0.5, 0.95))
        up = max(up1, up2)
        return {
            "l_arm": (72.0 - 135.0 * up, 0, 6.0 * up),
            "r_arm": (72.0 - 135.0 * up, 0, 6.0 * up),
            "l_forearm": (0, 0, 20.0 * (1.0 - up) + 8.0),
            "r_forearm": (0, 0, 20.0 * (1.0 - up) + 8.0),
            "spine": (-8.0 * up, 0, 0),
            "head": (-8.0 * up, 0, 0),
            "l_upleg": (10.0 * up, 0, 2.0),
            "r_upleg": (10.0 * up, 0, 2.0),
            "l_leg": (16.0 * up, 0, 0),
            "r_leg": (16.0 * up, 0, 0),
        }

    def hips_tr(t):
        up1 = _bump(_seg(t, 0.05, 0.5))
        up2 = _bump(_seg(t, 0.5, 0.95))
        hop = max(up1, up2)
        return (0.0, 0.06 * hop - 0.03 * (1 - hop), 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# SIT — lower onto an (invisible) chair, settle, stay seated.
# ---------------------------------------------------------------------------
def sit():
    seconds = 2.2

    def pose(t):
        down = _ease(_seg(t, 0.1, 0.55))
        settle = _bump(_seg(t, 0.5, 0.8)) * 0.15
        d = min(1.0, down + settle)
        return {
            "l_upleg": (74.0 * d, 0, 4.0 * d),
            "r_upleg": (74.0 * d, 0, 4.0 * d),
            "l_leg": (76.0 * d, 0, 0),
            "r_leg": (76.0 * d, 0, 0),
            "l_foot": (-4.0 * d, 0, 0),
            "r_foot": (-4.0 * d, 0, 0),
            "l_arm": (84.0 - 6.0 * d, 0, 14.0 * d),
            "r_arm": (84.0 - 6.0 * d, 0, 14.0 * d),
            "l_forearm": (0, 0, 30.0 * d),
            "r_forearm": (0, 0, 30.0 * d),
            "spine": (6.0 * d - 3.0 * _seg(t, 0.6, 1.0), 0, 0),
            "head": (-6.0 * d + 4.0 * _seg(t, 0.6, 1.0), 0, 0),
            "hips": (4.0 * d, 0, 0),
        }

    def hips_tr(t):
        down = _ease(_seg(t, 0.1, 0.55))
        return (0.0, -0.26 * down, 0.0)

    return seconds, pose, hips_tr




# ---------------------------------------------------------------------------
# THROW — overhand throw: wind-up over the shoulder, step, release, follow.
# ---------------------------------------------------------------------------
def throw():
    seconds = 1.6

    def pose(t):
        windup = _ease(_seg(t, 0.08, 0.35)) * (1.0 - _ease(_seg(t, 0.42, 0.55)))
        # The strike itself: rises fast, then HOLDS (a decaying release made
        # the arm retreat to neutral, so the throw visibly stopped mid-motion).
        release = _ease(_seg(t, 0.42, 0.58))
        # Follow-through: the arm keeps travelling DOWN and ACROSS the body
        # after the ball leaves, which is what sells the throw. Continues to
        # the end of the clip instead of unwinding.
        follow = _ease(_seg(t, 0.55, 0.88))
        active = _ease(_seg(t, 0.05, 0.2))
        return {
            # right arm: back over the shoulder (X -> raised-back), then
            # whipped forward past horizontal
            # Overhand arc: up-and-back (windup) -> forward past vertical
            # (release) -> down across the body (follow-through).
            "r_arm": (84.0 - 190.0 * windup - 20.0 * release + 150.0 * follow, 0,
                      -45.0 * windup + 80.0 * release - 30.0 * follow),
            "r_forearm": (0, 0, 85.0 * windup + 8.0 * release + 30.0 * follow),
            "r_hand": (0, 0, -15.0 * windup + 10.0 * release),
            # left arm points at the target during wind-up, tucks on release
            "l_arm": (84.0 - 30.0 * windup + 18.0 * release, 0,
                      30.0 * windup - 10.0 * release),
            "l_forearm": (0, 0, 10.0 + 25.0 * release),
            # torso coils back then uncoils through the throw
            "spine": (-6.0 * windup + 14.0 * release + 6.0 * follow,
                      8.0 * windup - 10.0 * release, 0),
            "spine1": (-3.0 * windup + 8.0 * release, 4.0 * windup - 5.0 * release, 0),
            "hips": (0, 4.0 * windup - 6.0 * release, 0),
            "head": (4.0 * windup - 6.0 * release, -12.0 * windup + 8.0 * release, 0),
            # stagger stance: left leg forward on release
            "l_upleg": (8.0 * active + 14.0 * release, 0, 2.0 * active),
            "r_upleg": (-6.0 * active - 10.0 * release, 0, 2.0 * active),
            "l_leg": (10.0 * active, 0, 0),
            "r_leg": (12.0 * active + 10.0 * release, 0, 0),
        }

    def hips_tr(t):
        release = _ease(_seg(t, 0.42, 0.58)) * (1.0 - _ease(_seg(t, 0.72, 0.95)))
        return (0.0, -0.05 * release, 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# DANCE — simple two-beat groove: side sway, arm pumps, knee bounce.
# ---------------------------------------------------------------------------
def dance():
    seconds = 2.4
    beats = 3.0

    def pose(t):
        p = beats * t
        sway = _s(p)                     # lateral hip sway
        bounce = abs(_c(p))              # knee bounce, 2 per sway
        pump = _s(p, math.pi / 2)        # arm pump alternation
        return {
            "hips": (2.0, 14.0 * sway, 11.0 * sway),
            "spine": (4.0, -16.0 * sway, -9.0 * sway),
            "spine1": (2.0, -6.0 * sway, -2.0 * sway),
            "head": (-3.0, 8.0 * sway, -4.0 * sway),
            # arms: elbows bent, alternating up-down pumps
            "l_arm": (72.0 - 34.0 * pump, 0, 24.0 + 14.0 * sway),
            "r_arm": (72.0 + 34.0 * pump, 0, 24.0 - 14.0 * sway),
            "l_forearm": (0, 0, 75.0 + 20.0 * pump),
            "r_forearm": (0, 0, 75.0 - 20.0 * pump),
            # legs: weight shifts with the sway, knees bounce
            "l_upleg": (6.0 + 6.0 * max(0.0, sway), 0, 4.0 + 3.0 * sway),
            "r_upleg": (6.0 + 6.0 * max(0.0, -sway), 0, 4.0 - 3.0 * sway),
            "l_leg": (12.0 + 26.0 * bounce, 0, 0),
            "r_leg": (12.0 + 26.0 * bounce, 0, 0),
            "l_foot": (-4.0 * bounce, 0, 0),
            "r_foot": (-4.0 * bounce, 0, 0),
        }

    def hips_tr(t):
        p = beats * t
        return (0.03 * _s(p), -0.04 * abs(_c(p)), 0.0)

    return seconds, pose, hips_tr



# ---------------------------------------------------------------------------
# HANG — dangling from a ledge/bar overhead: arms straight up, body hanging,
# legs dangling with a gentle sway. Loopable.
# ---------------------------------------------------------------------------
def hang():
    seconds = 3.0

    def pose(t):
        sway = _s(t)                      # slow pendulum
        sway2 = _s(2 * t, math.pi / 3)    # leg dangle detail
        return {
            # both arms straight overhead (T-pose horizontal -> vertical up),
            # slightly narrowed so the hands read as gripping above the head
            "l_arm": (-96.0 + 2.0 * sway, 0, 6.0),
            "r_arm": (-96.0 - 2.0 * sway, 0, 6.0),
            "l_forearm": (0, 0, 8.0),
            "r_forearm": (0, 0, 8.0),
            "l_hand": (0, 0, 10.0),
            "r_hand": (0, 0, 10.0),
            # shoulders shrugged up toward the ears (weight on the arms)
            "l_shoulder": (-14.0, 0, 0),
            "r_shoulder": (-14.0, 0, 0),
            # body dangles: slight arch + pendulum roll
            "spine": (-6.0, 2.0 * sway, 4.0 * sway),
            "spine1": (-3.0, 1.0 * sway, 2.0 * sway),
            "hips": (2.0, 0, 5.0 * sway),
            "head": (-14.0, 4.0 * sway, -3.0 * sway),   # looking up at the grip
            # legs hang loose, knees softly bent, small alternating dangle
            "l_upleg": (6.0 + 3.0 * sway2, 0, 2.0),
            "r_upleg": (6.0 - 3.0 * sway2, 0, 2.0),
            "l_leg": (14.0 + 5.0 * sway2, 0, 0),
            "r_leg": (14.0 - 5.0 * sway2, 0, 0),
            "l_foot": (8.0, 0, 0),        # toes pointed (unloaded)
            "r_foot": (8.0, 0, 0),
        }

    def hips_tr(t):
        sway = _s(t)
        # stretched by the body weight, drifting with the pendulum
        return (0.02 * sway, 0.06, 0.0)

    return seconds, pose, hips_tr


# ---------------------------------------------------------------------------
# CRAWL — hands-and-knees crawl cycle: torso pitched horizontal, alternating
# contralateral arm reach + knee step. 2 cycles, loopable.
# ---------------------------------------------------------------------------
def crawl():
    seconds = 2.4
    cycles = 2.0

    def pose(t):
        p = cycles * t
        reachL = _s(p)                    # +1 = left arm reaching forward
        reachR = _s(p, math.pi)
        return {
            # The whole-body pitch lives on the SPINE chain, not the hips —
            # the retarget locks the root's orientation to the standing pose,
            # so hip pitch would be discarded (a hips-pitched first draft
            # retargeted as an upright kneel with zombie arms).
            "spine": (42.0, 3.0 * reachL, 0),
            "spine1": (30.0, 2.0 * reachL, 0),
            "spine2": (14.0, 0, 0),
            "head": (-46.0, -5.0 * reachL, 0),   # face forward
            # kneeling legs: thighs slightly forward, knees folded, shins on
            # the ground; the step is a small hip-flex oscillation
            "l_upleg": (26.0 + 12.0 * reachR, 0, 3.0),
            "r_upleg": (26.0 + 12.0 * reachL, 0, 3.0),
            "l_leg": (96.0 - 8.0 * reachR, 0, 0),
            "r_leg": (96.0 - 8.0 * reachL, 0, 0),
            "l_foot": (12.0, 0, 0),
            "r_foot": (12.0, 0, 0),
            # arms: ventral swing under the (bowed) shoulders, reaching
            # alternately; anatomical convention (right side auto-mirrored)
            "l_arm": (58.0, 0, 66.0 - 12.0 * reachL),
            "r_arm": (58.0, 0, 66.0 - 12.0 * reachR),
            "l_forearm": (0, 0, 8.0 + 12.0 * max(0.0, reachL)),
            "r_forearm": (0, 0, 8.0 + 12.0 * max(0.0, reachR)),
            "hips": (0.0, 0, 1.5 * reachL),   # pitch lives on the spine (root lock)
        }

    def hips_tr(t):
        p = cycles * t
        return (0.0, -0.46 + 0.015 * abs(_s(p)), 0.0)

    return seconds, pose, hips_tr


ACTIONS = {
    "walk": walk,
    "run": run,
    "idle": idle,
    "wave": wave,
    "jump": jump,
    "punch": punch,
    "kick": kick,
    "march": march,
    "cheer": cheer,
    "sit": sit,
    "throw": throw,
    "dance": dance,
    "hang": hang,
    "crawl": crawl,
}


if __name__ == "__main__":
    import sys
    from glbanim import author
    which = sys.argv[1:] or list(ACTIONS)
    for name in which:
        seconds, pose, tr = ACTIONS[name]()
        r = author("rumba.glb", f"out_{name}.glb", name,
                   30, seconds, pose, tr)
        print(name, r)
