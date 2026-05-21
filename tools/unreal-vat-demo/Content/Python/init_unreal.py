# init_unreal.py — auto-run on every editor open.
#
# Unreal's PythonScriptPlugin executes any `init_unreal.py` found on
# its startup-script paths when the editor finishes loading. Both
# `<project>/Content/Python/` and the engine's `Python/` directory
# are on that path by default in UE 5.x, so dropping this file under
# `Content/Python/` is enough — no Project Settings tweak needed.
#
# We deliberately keep this thin: it runs `build_vat_demo.main()` if
# the bake is present AND the existing M_OpenVAT (if any) is older
# than the script's `OPENVAT_BUILD` constant. That way:
#   - First open of the project: bootstrap runs, dancer spawns.
#   - Subsequent opens with no script changes: skipped so we don't
#     wipe + recreate on every launch.
#   - Subsequent opens after the script bumps `OPENVAT_BUILD` (e.g.
#     to fix a coordinate-system bug): bootstrap reruns and replaces
#     the stale material.
#
# If you want to force a rebuild without bumping the version, delete
# `/Game/VATDemo/M_OpenVAT` from the Content Browser and reopen the
# project, or run `py Content/Python/build_vat_demo.py` from the
# Python console.

import os
import unreal


def _should_run_bootstrap():
    """Run when the bake is present AND either the material is missing
    or its `OpenVATBuild` metadata tag is older than what
    `build_vat_demo.OPENVAT_BUILD` claims. This avoids wiping +
    rebuilding on every open while still picking up script updates
    that fix the material graph (e.g. coordinate-system fixes)."""
    here = os.path.dirname(os.path.abspath(__file__))
    rumba = os.path.normpath(os.path.join(here, "..", "Rumba"))
    needed = ("source.gltf", "mixamo.com_pos.png",
              "mixamo.com-remap_info.json")
    for f in needed:
        if not os.path.exists(os.path.join(rumba, f)):
            unreal.log("init_unreal: skipping bootstrap — missing "
                       + f + " in Content/Rumba/.")
            return False
    if not unreal.EditorAssetLibrary.does_asset_exist("/Game/VATDemo/M_OpenVAT"):
        return True
    # If the material exists but no mesh asset does, an earlier
    # bootstrap clearly failed mid-flight (e.g. Interchange import
    # never produced a .uasset). Rebuild rather than skipping —
    # otherwise the actor in the level renders nothing on disk.
    el = unreal.EditorAssetLibrary
    mesh_paths = (
        "/Game/Rumba/SM_Rumba",
        "/Game/Rumba/SK_Rumba",
        "/Game/Rumba/source/StaticMeshes/SM_Rumba",
        "/Game/Rumba/source/StaticMeshes/Rumba_Dancing_mesh",
        "/Game/Rumba/source/SkeletalMeshes/SK_Rumba",
        "/Game/Rumba/source/SkeletalMeshes/SM_Rumba",
        "/Game/Rumba/source/SkeletalMeshes/Rumba_Dancing_mesh",
        "/Game/Rumba/Rumba_Dancing_mesh",
    )
    if not any(el.does_asset_exist(p) for p in mesh_paths):
        unreal.log("init_unreal: no Rumba mesh on disk under /Game/Rumba/ "
                   "— rebuilding to re-import the glTF.")
        return True
    # Material AND mesh present — compare the build stamp to the script's.
    try:
        # Importing here (not at module scope) keeps init_unreal robust
        # if build_vat_demo has a syntax error: we'd still log a clear
        # message instead of crashing the editor's Python init.
        import build_vat_demo
        expected = int(getattr(build_vat_demo, "OPENVAT_BUILD", 0))
    except Exception as e:
        unreal.log_warning("init_unreal: could not read OPENVAT_BUILD "
                           "from build_vat_demo (" + str(e) + "); "
                           "rebuilding to be safe.")
        return True
    mat = unreal.load_asset("/Game/VATDemo/M_OpenVAT")
    stamped_str = unreal.EditorAssetLibrary.get_metadata_tag(
        mat, "OpenVATBuild") if mat else ""
    try:
        stamped = int(stamped_str) if stamped_str else 0
    except ValueError:
        stamped = 0
    if stamped < expected:
        unreal.log("init_unreal: M_OpenVAT build " + str(stamped)
                   + " is older than script build " + str(expected)
                   + " — rebuilding.")
        return True
    unreal.log("init_unreal: M_OpenVAT is up to date (build "
               + str(stamped) + "); skipping bootstrap.")
    return False


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
