# Procedural animation generators (#524)

A **generator** fills an animatable property from a formula instead of hand-set keyframes.
It is layered on top of whatever animation the property already has, and can be
baked into ordinary keyframes at any time.

| Type | Parameters | Typical use |
|---|---|---|
| `sine` | `amplitude`, `frequency` (Hz), `phase` (°), `offset` | hovering, breathing, swaying |
| `noise` | `amplitude`, `noise_frequency` (features/s), `octaves` (1–8), `seed` | camera shake, weapon sway, flicker |
| `ramp` | `from`, `to`, `ease` (`linear`/`smooth`) | fades, slow drifts |
| `spring` | `from`, `to`, `stiffness` (ω, rad/s), `damping` (ζ; 1 = critical) | game-feel overshoot, settle |
| `follow-path` | `points` (`x,y,z;…`), `closed`, `constant_speed`, `orient`, `loops` | patrols, fly-throughs |

Every type also takes `start` and `duration` (seconds; `0` = until the clip ends — runtime targets
default to 4 s) and `fps` (the sampling density used for the track and for baking).

## Targets

`kind:object[/sub]/channel[@clip]`

| Kind | Object / sub | Channels | Bound how |
|---|---|---|---|
| `bone` | entity / bone | `position.x/y/z`, `rotation.x/y/z` (°, local axis), `scale.x/y/z`, `position` (path) | written into the skeletal clip (default: selected / first clip) |
| `node` | scene node | same as bone | written into a node clip (default `Generators`, created and enabled) |
| `morph` | entity / morph target | `weight` | written into the weight clip (default `MorphAnim`) |
| `pose` | entity / saved pose | `weight` (0 = bind, 1 = pose) | driven every frame |
| `light` | light | `intensity`, `diffuse.r/g/b`, `specular.r/g/b` | driven every frame |
| `material` | material | `diffuse.r/g/b/a`, `ambient.*`, `specular.*`, `emissive.*`, `shininess` | driven every frame |

On the CLI, `*` as the object means the imported mesh (or its node for `node:`).

## Layering, mute and bake

* Generators **add** to the base: `value(t) = base(t) + Σ active generators(t)`. `follow-path` is the
  exception — it **replaces** the position (and orientation with `orient`), because a drawn path is
  where the object should be.
* Bone / node / morph generators are written into the real animation track: its original keys are
  kept as the *base*, and the track becomes the base keys outside the generator's window plus dense
  samples inside it. Playback, scrubbing, the dope sheet and every exporter see the motion.
* **Mute** (uncheck in the panel, `enabled: false` over MCP) puts the original keys back exactly.
* **Bake** folds the generator into the base. For track targets the keys become permanent (removing
  the generator later keeps them); for pose / light / material the baked curve is kept with the
  generators and is removed with its generator. A baked generator stays attached but inactive.
* Pose / light / material targets run on the generator clock: it advances while playing and follows
  the timeline slider while paused.
* Every operation is one undo step.

## Where the generators are saved

Next to every export, `<file>.generators.json` (schema `qtmesh-anim-generators-v1`) stores the
generators **and** their base snapshots, so they stay editable — and muteable back to the original
motion — after a reload. The exported file itself already contains the generated motion, so other
tools see it without the sidecar.

## Surfaces

* **GUI:** Animation mode → Mode Tools → **Generators**. Pick type, target and channel, *Add
  generator*, then edit the parameters of the selected row. For a follow-path, *Edit in viewport*
  shows the path; drag its points with the Select tool.
* **CLI:**
  ```
  qtmesh anim <file> --generator <type> --target <target> [--<param> <value> …] [--generator …] [--bake] -o <out>
  qtmesh anim <file> --list-generators [--json]
  ```
* **MCP:** `list_generators`, `add_generator {type, target, params, name?, enabled?, bake?}`,
  `set_generator {id, params?, enabled?}`, `bake_generator {id}`, `remove_generator {id}`.
