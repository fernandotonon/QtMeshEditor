# Animation retargeting

Move a clip from one skeleton onto a **different** skeleton: a Mixamo
animation onto your own character, a Unity/HumanIK clip onto an Unreal
mannequin, a mocap take onto an auto-rigged mesh. The skeletons may differ in
bone names, bone axes, proportions, rest pose (A-pose vs T-pose) and up axis
(Y-up vs Z-up). (For skeletons that already share names and rest poses,
`qtmesh anim --merge` is simpler.)

## In the editor

Animation mode → **Retarget** section → **Retarget Animation…**

1. **Source clip.** Pick the entity with the animation, or **Import file…**.
2. **Target skeleton.** Pick the character that should receive it. The
   selected skeletal entity is preselected.
3. **Bone map.** The bones are auto-mapped as soon as both are set. Review
   the table and override any row with the drop-down on its right. Use
   **Show unmapped only** to find gaps. **Use bundled** loads a built-in map;
   **Load… / Save…** read and write `.bonemap` files you can reuse across
   projects.
4. **Options.** See below.
5. **Preview side by side** plays the source clip and the retargeted result
   together. The target slides sideways if the two overlap, and everything is
   restored when the preview stops. **Apply** creates the clip on the target
   as **one undo step**. The clip is an ordinary skeletal animation: the
   timeline, dope sheet and exporters treat it like any other.

## Command line

```bash
qtmesh anim walk.fbx --retarget hero.glb -o hero_walking.glb             # auto-map, every clip
qtmesh anim walk.fbx --retarget hero.fbx --bonemap mixamo_to_unity --anim Walk -o hero.fbx
qtmesh anim walk.fbx --retarget hero.glb --bonemap mine.bonemap --name Walk --translation none -o out.glb
qtmesh anim walk.fbx --retarget hero.glb --print-bonemap                 # show the auto map, write nothing
qtmesh anim walk.fbx --retarget hero.glb --save-bonemap walk_to_hero.bonemap -o out.glb
```

`--json` reports, per clip, the frames written, the bones driven, whether the
humanoid alignment was found, and the height scale.

## MCP

`retarget_animation`. Give `source_entity` or `source_file` and
`target_entity` or `target_file` + `output_path`, plus `animation`. The map
comes from `pairs` (inline `[{source,target}]`), `bonemap` (bundled name or
path), or auto-mapping when neither is given. `dry_run: true` returns only the
map. An in-scene target is undoable.

## Bone maps

Auto-mapping tries, in order:

1. the same name once namespaces (`mixamorig:`, `Armature|`) and rig prefixes
   (`Bip01 `, `DEF-`) are stripped;
2. the humanoid role of each bone (hips, spine, neck, head, clavicle, upper
   arm, forearm, hand, thigh, shin, foot, fingers), across Mixamo, HumanIK,
   Unreal, Unity, 3ds Max Biped, CMU and Quaternius spellings. Bones that
   share a role (two lower-spine bones) pair in hierarchy order;
3. a side-aware synonym match (`thigh` = `UpLeg`, `calf` = `Leg`,
   `clavicle` = `Shoulder`, `palm` = `Hand`, …), then a close spelling on
   the same side.

Each target bone is used once. Unmapped target bones keep their bind pose.

Bundled maps: `mixamo_to_humanik` (also `mixamo_to_unity`: Unity's
Humanoid/HumanIK names are the ones Mixamo carries under its prefix) and
`mixamo_to_unreal` (UE mannequin: `pelvis`, `spine_01`, `upperarm_l`,
`index_02_r`, …). A map written for `mixamorig:` also applies to a
`mixamorig1:` rig: names are resolved leniently.

`.bonemap` files are JSON:

```json
{ "schema": "qtmesh-bonemap-v1", "name": "walk_to_hero",
  "pairs": [ { "source": "mixamorig:Hips", "target": "pelvis" } ] }
```

## Options

| Option | Values | Use |
|---|---|---|
| Translation | `none` | Rotation only. The target keeps its own bone lengths exactly. |
| | `root` (default) | Also the hips' motion, scaled by the height ratio of the two rigs, so it travels and bobs without sliding. |
| | `all` | Every mapped bone's translation, scaled. For faces and fingers animated by translation. |
| Source rest | `bind` (default) | Motion is measured from the source's bind pose. |
| | `first-frame` | Measured from the clip's first frame. Use it when the source's bind pose is rotated relative to its animation (some exported armatures). |
| Match rest directions | on (default) | Rotates each target rest bone onto the source's rest direction, so an A-pose source drives a T-pose target with the arms where they belong. |

## How it works

Per mapped bone, the source's world-space rotation away from its rest pose is
applied to the target's rest pose. A global alignment, derived from each rig's
hips → head and left → right legs (or arms), absorbs Y-up vs Z-up, ±Z
facing and armature rotations. Branching bones (chest, hips) align to their
central child, so a chest is never rolled toward one arm. The root
translation follows the source's hips even when the target lists another
bone first (IK feet parented above the hips).

## Limits

- Non-humanoid rigs work with a manual map, but without the global humanoid
  alignment the two rigs must share their world axes.
- Auto-mapping a humanoid clip onto a quadruped maps legs to the front legs.
  Review the map.
- Bones absent from the target (e.g. no clavicles on a UniRig skeleton) drop
  that part of the motion.
