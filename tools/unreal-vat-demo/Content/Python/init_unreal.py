# init_unreal.py — auto-run on every editor open.
#
# Unreal's PythonScriptPlugin executes any `init_unreal.py` found on
# its startup-script paths when the editor finishes loading. Both
# `<project>/Content/Python/` and the engine's `Python/` directory
# are on that path by default in UE 5.x, so dropping this file under
# `Content/Python/` is enough — no Project Settings tweak needed.
#
# We deliberately keep this thin: it runs `build_vat_demo.main()` if
# the bake is present and the demo material doesn't already exist.
# That way:
#   - First open of the project: bootstrap runs, dancer spawns.
#   - Subsequent opens: skipped (material is already there) so we
#     don't wipe + recreate on every launch.
#
# If you want to force a rebuild, delete `/Game/VATDemo/M_OpenVAT`
# from the Content Browser and reopen the project, or run
# `py Content/Python/build_vat_demo.py` from the Python console.

import os
import unreal


def _should_run_bootstrap():
    """Run only when the bake is present AND the material doesn't
    already exist. Both are cheap to check on the game thread."""
    here = os.path.dirname(os.path.abspath(__file__))
    rumba = os.path.normpath(os.path.join(here, "..", "Rumba"))
    needed = ("source.gltf", "mixamo.com_pos.png",
              "mixamo.com-remap_info.json")
    for f in needed:
        if not os.path.exists(os.path.join(rumba, f)):
            unreal.log("init_unreal: skipping bootstrap — missing "
                       + f + " in Content/Rumba/.")
            return False
    if unreal.EditorAssetLibrary.does_asset_exist("/Game/VATDemo/M_OpenVAT"):
        unreal.log("init_unreal: skipping bootstrap — M_OpenVAT "
                   "already exists. Delete it to force a rebuild.")
        return False
    return True


def _run_when_editor_ready():
    """Defer the bootstrap until the editor has finished initialising.
    `init_unreal.py` fires very early — before the level loader has
    finalised — and `spawn_actor_from_class` returns None at that
    point. We hand the work off to the next editor tick via a one-shot
    timer so the world is ready by the time we try to spawn.

    `register_slate_post_tick_callback` is the UE-Python idiom for
    "run this on the next editor frame". The returned handle is
    captured so we can unregister and prevent re-fire on subsequent
    frames.
    """
    state = {"fired": False, "handle": None}

    def _tick(_delta):
        if state["fired"]:
            return
        state["fired"] = True
        try:
            unreal.unregister_slate_post_tick_callback(state["handle"])
        except Exception:
            pass
        try:
            import build_vat_demo
            build_vat_demo.main()
        except Exception as e:
            unreal.log_error("init_unreal: bootstrap failed: " + str(e))

    try:
        state["handle"] = unreal.register_slate_post_tick_callback(_tick)
    except Exception:
        # Older builds without the slate callback API — fall back to
        # firing immediately. Worst case: the spawn step fails with a
        # clear error and the user re-runs the script manually.
        try:
            import build_vat_demo
            build_vat_demo.main()
        except Exception as e:
            unreal.log_error("init_unreal: bootstrap failed: " + str(e))


if _should_run_bootstrap():
    unreal.log("init_unreal: scheduling OpenVAT bootstrap "
               "(runs on the next editor tick so the level is ready).")
    _run_when_editor_ready()
