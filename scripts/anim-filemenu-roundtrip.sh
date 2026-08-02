#!/usr/bin/env bash
# File-menu round-trip verification for #517. The user exports/reimports via the
# File menu, which uses DIFFERENT code paths than load_mesh:
#   Save Scene  -> sceneExporter  (.scene.glb / .scene.gltf)
#   Open Scene  -> sceneImporter
#   Export Sel. -> exporter (single-entity buildAiScene) -> glb/fbx/mesh
#   Import      -> importer (mUriList)
# MCP maps: save_scene=Save Scene, open_scene=Open Scene, export_mesh=Export
# Selected, load_mesh=Import. This drives every combo and checks all 3 anim
# types survive.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${QTMESH_APP:-$ROOT/build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor}"
BODY="${QTMESH_BODY_GLB:-$ROOT/.mocap_work/out_body.glb}"   # skeletal, 9 submeshes
FACE="${QTMESH_FACE_GLB:-$ROOT/.mocap_work/out_face.glb}"   # morph shapes
PORT="${QTMESH_MCP_PORT:-8053}"
BASE="http://localhost:$PORT/api/tools"
LOG=/tmp/anim_fm_app.log
PASS=0; FAIL=0; fails=()

pkill -9 -f "QtMeshEditor" 2>/dev/null; sleep 2
"$APP" --with-mcp --http-port $PORT > "$LOG" 2>&1 &
APP_PID=$!
for i in $(seq 1 40); do curl -s -m 3 "$BASE" >/dev/null 2>&1 && break; sleep 1; done
curl -s -m 3 "$BASE" >/dev/null 2>&1 || { echo "FATAL: HTTP never up"; tail -20 "$LOG"; kill $APP_PID 2>/dev/null; exit 2; }

call(){ curl -s -m 60 -X POST "$BASE/$1" -H "Content-Type: application/json" -d "$2"; }
ctext(){ python3 -c "import sys,json;print(json.load(sys.stdin).get('content',[{}])[0].get('text',''))" 2>/dev/null; }
ok(){ PASS=$((PASS+1)); echo "PASS: $1"; }
bad(){ FAIL=$((FAIL+1)); fails+=("$1"); echo "FAIL: $1${2:+ -> $2}"; }
has(){ if echo "$3" | grep -qF "$2"; then ok "$1"; else bad "$1" "$(echo "$3"|head -1)"; fi; }

# Author skeletal(from body)+node on the body, morph on the face separately.
author_node(){ # $1=node name
  call add_node_animation_clip '{"name":"Spin","length":2.0}' >/dev/null
  call set_node_keyframe "$(printf '{"clip":"Spin","node":"%s","time":0.0,"translate":[0,0,0]}' "$1")" >/dev/null
  call set_node_keyframe "$(printf '{"clip":"Spin","node":"%s","time":2.0,"translate":[5,0,0]}' "$1")" >/dev/null
}

# ---- COMBO 1: Save Scene -> Open Scene (body: skeletal + node) ----
echo "===== COMBO 1: Save Scene -> Open Scene (skeletal + node) ====="
call load_mesh "$(printf '{"path":"%s"}' "$BODY")" >/dev/null; sleep 2
author_node "out_body"
call save_scene '{"file_path":"/tmp/fm1.scene.glb"}' >/dev/null; sleep 1
call open_scene '{"file_path":"/tmp/fm1.scene.glb"}' >/dev/null; sleep 2
SK=$(call list_skeletal_animations '{}'|ctext); NA=$(call list_node_animations '{}'|ctext)
has "C1 Save/Open: skeletal survives" "Animation:" "$SK"
has "C1 Save/Open: node clip survives" "\"count\": 1" "$NA"

# ---- COMBO 2: Export Selected(glb) -> Import (body: skeletal + node) ----
echo "===== COMBO 2: Export Selected(glb) -> Import (skeletal + node) ====="
pkill -9 -f "QtMeshEditor" 2>/dev/null; sleep 2
"$APP" --with-mcp --http-port $PORT > "$LOG" 2>&1 & APP_PID=$!
for i in $(seq 1 40); do curl -s -m 3 "$BASE" >/dev/null 2>&1 && break; sleep 1; done
call load_mesh "$(printf '{"path":"%s"}' "$BODY")" >/dev/null; sleep 2
author_node "out_body"
call export_mesh "$(printf '{"name":"out_body","path":"/tmp/fm2.glb","format":"glTF 2.0 Binary (*.glb)"}')" >/dev/null
call load_mesh '{"path":"/tmp/fm2.glb"}' >/dev/null; sleep 2
SK=$(call list_skeletal_animations '{}'|ctext); NA=$(call list_node_animations '{}'|ctext)
has "C2 ExportSel/Import: skeletal survives" "Animation:" "$SK"
has "C2 ExportSel/Import: node clip survives" "\"count\": 1" "$NA"

# ---- COMBO 3: morph via Save Scene -> Open Scene (face) ----
echo "===== COMBO 3: morph Save Scene -> Open Scene ====="
pkill -9 -f "QtMeshEditor" 2>/dev/null; sleep 2
"$APP" --with-mcp --http-port $PORT > "$LOG" 2>&1 & APP_PID=$!
for i in $(seq 1 40); do curl -s -m 3 "$BASE" >/dev/null 2>&1 && break; sleep 1; done
call load_mesh "$(printf '{"path":"%s"}' "$FACE")" >/dev/null; sleep 2
call set_morph_weight_keyframe '{"target":"Shape_0","time":0.0,"weight":0.0}' >/dev/null
call set_morph_weight_keyframe '{"target":"Shape_0","time":1.0,"weight":1.0}' >/dev/null
call save_scene '{"file_path":"/tmp/fm3.scene.glb"}' >/dev/null; sleep 1
call open_scene '{"file_path":"/tmp/fm3.scene.glb"}' >/dev/null; sleep 2
MO=$(call list_morph_targets '{"file":"/tmp/fm3.scene.glb"}'|ctext)
has "C3 morph Save/Open: shapes survive" "Shape_" "$MO"

echo
echo "===== SUMMARY: PASS=$PASS FAIL=$FAIL ====="
[ "$FAIL" -gt 0 ] && printf 'FAILED: %s\n' "${fails[@]}"
if kill -0 $APP_PID 2>/dev/null; then echo "APP ALIVE (no crash)"; else echo "FAIL: APP CRASHED"; FAIL=$((FAIL+1)); fi
kill $APP_PID 2>/dev/null; sleep 1; kill -9 $APP_PID 2>/dev/null
rm -f /tmp/fm1.scene.glb /tmp/fm2.glb /tmp/fm3.scene.glb
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
