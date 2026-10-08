# Creature motion (quadruped / winged biped)

Template clips retargeted onto creature rigs, the non-humanoid counterpart of
the #411 text-to-motion library.

## Verifying a retarget

**Use `scripts/creature-retarget-error.py`.** It measures world-orientation
error against the rig's own NATIVE clip. A SELF-retarget (clip source rig ==
target rig) must score a few degrees; the library currently sits at a median
of 5.8 deg with nothing above 20.

```
scripts/creature-retarget-error.py native.glb "Armature|Walk" \
                                   retarget.glb creature_Walk
```

Two cheaper checks are actively misleading and both shipped a broken clip:

- **Per-joint angle RANGE** matched the source to 0.1 deg while the limb was
  placed completely wrong. Range says nothing about placement.
- **Frame-to-frame delta** ("is the clip live?") reported a 50 %-dead clip as
  98.9 % live, because a held pose still jitters. Measure distance from the
  FINAL pose instead — that is what the extractor's dead-tail trim does.

When rendering to judge by eye, use **>= 16 frames** and a **true side view**
(`isometric --directions 1 --start-azimuth 90`). Sprite sheets at 5-10 frames
alias badly on a walk cycle and made a frozen second half look fine; row 0 of
a multi-direction sheet is the FRONT camera, which foreshortens a striding or
toppled body into nonsense.

## How the retarget works

Clips store per-role WORLD orientations plus the source rig's bind pose
(`restWorld`) and a scale-normalised root displacement (`rootOffset`).

`CreatureMotionRetarget::apply` runs a **per-frame** pass: it poses the
skeleton from the keys solved so far, reads each mapped bone's LIVE parent
world, and solves

    key = bindLocal^-1 * parentWorld^-1 * (dWorld * bindWorld)

Solving against the live parent is what makes it correct on rigs that carry
MORE limb segments than the canonical skeleton (Leg -> UpLeg -> LowLeg -> Foot
against canonical UpLeg -> LowLeg -> Foot). One bone per limb is unmapped and
stays frozen at bind; a key solved against the BIND parent never reaches its
intended world orientation, which showed up as ~22 deg of error concentrated
entirely in the lower legs while the limb roots were already correct to ~1 deg.

**Ogre gotcha:** a track key POST-multiplies onto the bind LOCAL pose
(`node->rotate(key)` on a node reset to bind). When posing a skeleton to
evaluate the chain you must set `bindLocal * key`, not `key` — setting the key
alone converges on a mirrored solution, a clean ~177 deg flip.

### Root translation

Rotation alone cannot express a jump (the body rises), a death (it topples) or
a lunge: with translation discarded the body pivots about a root pinned at its
bind position and folds through its own legs. `rootOffset` is stored in units
of the SOURCE rig's hip height and replayed against the TARGET's, so the same
motion reads correctly on a horse and on a pug.

It is applied to the skeleton's **top-level** bone, not canonical role 0: role
0 resolves to `Hips`, which sits mid-hierarchy (the back legs hang off it, the
front legs do not), so translating it tore the body in half.

## Bone mapping

The canonical skeletons are in `src/QuadrupedSkeleton.{h,cpp}`. Two rules are
not obvious:

- A bare `FrontLeg.L` is the limb ROOT on the three-segment Quaternius rigs,
  not the middle segment, and it BEATS the explicitly-named `FrontUpLeg.L` for
  the upper-leg role. Hierarchy position is stronger evidence than a name.
- AutoRig's own quadruped template names the spine `SpineFront` / `SpineMid` /
  `SpineBack` with no root or chest, so those map onto the canonical anchors —
  without that, a generated + auto-rigged creature is refused outright and the
  generate -> auto-rig -> animate path cannot play any clip.

A rig whose bones are named `bone_01`, `bone_02`, … is REFUSED with a clear
message rather than guessed at.

## Building the library

```
scripts/build-creature-library.py --corpus ~/motion_corpus/raw -o creature-library.json
```

Rebuild after ANY change to the canonical skeletons or the bone mapper: clips
store per-role world orientations, so a mapper change silently re-points every
clip's data at different bones.
