# Motion graph (#526)

> **Preview / authoring only.** The graph lets you try out clip logic inside QtMeshEditor. Export writes only the clips. Engine-native graphs (Unity Mecanim, Unreal AnimBP, Godot AnimationTree) are out of scope; the graph itself is saved only in a sidecar next to the asset.

A motion graph is a small state machine over an entity's clips. Use it to preview character logic such as idle → walk → run, or a weapon draw that returns to idle, before rebuilding it in your engine.

## Concepts

| | |
|---|---|
| **State** | A clip on the entity, with **loop** (on by default) and **speed**. One state is the **entry** (▶), where Play starts. |
| **Transition** | An arrow from a state (or from **Any** state) to another state. It fires when **all** its conditions hold. If it has an **exit time** (0–1 of the source clip), it also waits until the source clip reaches that point. It cross-fades over its **duration** with a **curve**: linear, ease, or step (step snaps with no blend). |
| **Parameter** | A `float`, `bool`, or `trigger`. Conditions compare parameters with `> < >= <= == != true false`. When a trigger fires a transition, the trigger is reset (consumed). |
| **Bone mask** | Optional, per transition. A masked transition is **partial**: the target clip drives only the masked bones, and the clip that was playing keeps driving the rest. Use it for an upper-body wave over a running lower body. |

Transitions are checked in list order every frame. **▲▼** changes the priority, and the first match wins. A transition from **Any** never re-enters the state that is already playing.

## Playback

**▶ Play graph** runs the graph on the entity, independently of the main timeline. Playback speed still applies. Change parameters in the panel while it plays and the transitions react live.

While the graph plays, it owns the entity's own animation states. It sets each clip's time, a weight of 1, and a **per-bone blend mask** holding that clip's share of each bone. The skeleton runs in Ogre's `ANIMBLEND_CUMULATIVE` mode, because in the default `AVERAGE` mode Ogre would halve two complementary masked clips. The per-bone weights are normalised to sum to 1.

Because the graph works through the entity's normal animation states, constraints (#525) and the viewport keep working on top of it. **Stop** restores every state flag, time, weight, mask, and the blend mode exactly as they were.

## Authoring (Animation mode → Mode Tools → Motion Graph)

- **Building the graph**
  - **+ State** adds a state for the selected clip.
  - **Template** builds idle / walk / run from the entity's clips, driven by a `speed` float.
  - Drag the nodes to arrange them. The view scales itself to fit.
- **Editing a state:** click it to edit its name, clip, loop, and speed, or to set it as the entry.
- **Adding a transition:** click **→ Transition…**, then click the target state. **Any → …** adds a transition from any state.
- **Editing a transition:** click its arrow to edit blend duration, curve, exit time, and conditions. Set the mask with **Subtree** of a bone (for example `Spine` for the upper body), or **Clear** it. Masked transitions are drawn dashed.
- **Undo:** every edit is one undo step. A whole node drag is a single step.

## Persistence

The graph is saved in `<file>.animgraph.json` (schema `qtmesh-anim-graphs-v1`, holding one `qtmesh-anim-graph-v1` graph per entity). It is written beside each export and loaded on import. When you import a single entity under a new name, its graph binds to the imported entity.

## CLI / MCP

```bash
qtmesh anim hero.glb --graph-info [--json]   # inspect the saved graph (no mesh load)
```

MCP tools:

- `get_motion_graph`
- `set_motion_graph` (a whole graph as JSON; every clip must exist)
- `play_motion_graph` / `stop_motion_graph`
- `set_motion_graph_param` (the live copy while playing, otherwise the stored default)

Sentry breadcrumbs: `scene.anim.graph.{add_state,add_transition,add_param,edit,mask,template,play,stop,param,transition,save,load,clear,undo,redo}`.

## Follow-ups

- Additive layers that don't come from a transition.
- Blend trees: 1-D speed blending between clips.
- Export to engine graphs.
