# Animation constraints (#525)

A constraint drives a **bone** or **scene node** from another object every frame, on top of
whatever its animation does. Constraints are live: move the target and the owner follows.
When you're done, **Bake** writes the result into ordinary keyframes.

| Type | What it does |
|---|---|
| **Look at** | Rotates the owner so its aim axis (default −Z, Ogre's forward — the way cameras and lights face) points at the target. The up axis (default +Y) stays as close to world up as it can. |
| **IK (2-bone)** | Analytical two-bone IK. Put it on the **END bone** (a hand or a foot). Its parent and grandparent (elbow/shoulder, knee/hip) bend so the end reaches the target. An optional **pole** object picks the bend direction; without one the current bend plane is kept. Bone lengths never change. |
| **Child of** (`parent-of`) | The owner follows the target as if parented to it, without changing the scene hierarchy. The offset is captured when you add the constraint, so nothing jumps. |
| **Copy rotation** | Takes the target's world rotation. |
| **Copy position** | Takes the target's world position, with a per-axis mask. |
| **Limit rotation** | Clamps the owner's local Euler XYZ angles, e.g. knees that don't bend backwards. It needs no target. |

Every constraint has an **influence** (0–1) that blends it with the incoming value.

## IK chains with handles (#560)

**Create IK chain** (Constraints panel, with a bone owner; MCP `create_ik_chain`) is the
one-click way to rig an arm or a leg. Pick the **end bone** (hand or foot) and it does the following:

- creates an orange **target** handle at the end bone and, with *with pole* on (the default),
  a cyan **pole** handle on the side the elbow/knee already bends to. A straight limb gets
  knees forward and elbows back. Nothing moves until a handle is dragged;
- adds an `ik` constraint wired to them;
- selects the target, so the transform gizmo can drag it immediately.

Handles are ordinary scene nodes with a small sphere, drawn through the character. That means
the gizmo, the Scene tree and node animation all work on them, so keyframing a handle
animates the IK. Selecting a handle shows its chain in the panel.

The handles belong to the constraints:

- they are created and destroyed with the constraint that references them, and every step is undoable
  (creation is ONE step, handles included);
- scene export skips them, so they never become geometry;
- they are saved in the sidecar's `handles` array and recreated on import, together with the
  node-animation keys that move them (a keyframed target survives Save Scene / Open Scene, and
  undoing a removed chain brings its keys back);
- importing a second character with the same bone names gives it its own handles
  (`IK_<bone>_target1`, …); re-loading the same chain reuses the existing ones.

**FK ↔ IK** is the constraint's `influence`: 0 = the clip (FK), 1 = pure IK, in between a
blend. **Bake** turns the IK into FK keys and mutes the constraint. Engines without runtime IK
need this, so export offers it.

**Joint limits:** put a *Limit rotation* constraint on the elbow/knee (or shoulder/hip). It is
re-applied after the IK solve, so the IK cannot push a joint past its limits.

Longer chains (CCD / FABRIK) are out of scope: 2-bone analytical IK covers arms and legs.

## Stacks: the top wins

Constraints on one owner form a stack. The stack is evaluated **bottom-up**, so the top-most
constraint is applied last and wins where two conflict. A new constraint goes on top. Reorder
with ▲ ▼ (GUI), `move_constraint` (MCP), or by the order of `--constraint` arguments (CLI;
each one goes on top).

Evaluation order inside a frame:

1. Node owners run first, in creation order.
2. Bone owners run next, per skeleton, parents before children.

A node that copies a **bone** therefore sees the bone's pose from the previous frame. The
reverse case (a bone driven by a node) is exact.

## How it runs

The `ConstraintManager` evaluates from an Ogre `SceneManager` listener (`preUpdateSceneGraph`).
That runs after scene/node clips are applied and before the skin is computed:

- **Node owners.** The node's local transform is the input. If the node still holds the value
  written last frame, the stored base is reused; otherwise the user or a node clip moved it,
  and that becomes the new base. As a result, an influence below 1 never creeps, and removing
  or muting a constraint puts the node back.
- **Bone owners.** The skeleton is posed from its clips right there. The constraints are then
  written on top, and Ogre is told to skip its own re-sampling for that frame. Bones are not
  made manual, so muting a constraint simply lets the clip drive them again.

## Bake

Bake samples the constrained motion at a chosen fps (default 30) over the clip:

- **Bone owners** are written into the skeletal clip (the playing, selected, or first clip),
  **including the parents an IK bends**.
- **Node owners** go into the node clip that already animates them, else a new `Constraints` clip.

The baked constraints are then **muted**, so the keys carry the motion. Bake is one undo
step, and undo restores the tracks exactly.

glTF/FBX can't store constraints. **Export** therefore asks whether to *Bake & Export*,
*Export without baking*, or *Cancel*.

## Persistence

Constraints are saved in `<file>.constraints.json` (schema `qtmesh-anim-constraints-v1`)
next to every export, together with the IK handles they reference (`handles`: name, role,
world position, radius, node-animation `tracks`), and loaded again on import:

- A single-entity export keeps the constraints owned by that entity or its node.
- `meta` records the exported names, so a renamed re-import rebinds the constraints.

Constraints do **not** take part in retargeting (#523).

## Surfaces

- **GUI:** Animation mode → Mode Tools → **Constraints**. It offers an owner picker (defaults
  to the selected node, or the selected bone in the animation panel), type/target/pole
  pickers, the stack with mute ▲ ▼ ✕, per-type parameters, and Bake. Every action is one
  undo step.
- **CLI:**
  ```bash
  qtmesh anim hero.glb --list-constraints [--json]
  qtmesh anim hero.fbx --constraint ik --owner "bone:*/mixamorig:LeftHand" --target "node:Cup" \
                       --constraint limit-rotation --owner "bone:*/mixamorig:LeftForeArm" --max-y 10 \
                       --bake-constraints --animation Idle --fps 30 -o out.glb
  ```
  `*` stands for the imported entity (bone refs) or its node (node refs). Constraints already
  in the input's sidecar are loaded first.
- **MCP:** `list_constraints`, `add_constraint`, `set_constraint` (params + `enabled`),
  `move_constraint`, `remove_constraint`, `bake_constraints`.

Parameter keys: `influence`, `aim`/`up` (`x|y|z|-x|-y|-z`), `x`/`y`/`z` (copy-position
axes), `limit_x|y|z`, `min_x|y|z`/`max_x|y|z` (degrees), `target`, `pole`, `name`.

Sentry breadcrumbs: `scene.anim.constraint.{add,remove,edit,enable,reorder,bake,save,load,clear,cli,undo,redo,error}`.

## Follow-ups

- FABRIK / long-chain IK.
- A viewport gizmo for pole targets.
- Per-constraint evaluation-order overrides across owners.
