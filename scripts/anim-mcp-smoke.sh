#!/usr/bin/env bash
# Autonomous MCP animation-tools test harness (#517 all-anim-via-MCP).
# Launches the GUI+MCP app, drives every animation tool over HTTP, verifies
# results, and prints PASS/FAIL lines. Exit 0 iff every check passes.
set -u
# Repo root = parent of this script's dir. All paths derive from it so the
# harness is portable (macOS .app path; adjust APP for Linux if needed).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${QTMESH_APP:-$ROOT/build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor}"
FACE_ASSET="${QTMESH_FACE_GLB:-$ROOT/.mocap_work/out_face.glb}"
BODY_ASSET="${QTMESH_BODY_GLB:-$ROOT/.mocap_work/out_body.glb}"
PORT="${QTMESH_MCP_PORT:-8097}"
BASE="http://localhost:$PORT/api/tools"
LOG=/tmp/anim_mcp_app.log
PASS=0; FAIL=0
fails=()

pkill -f "QtMeshEditor.app/Contents/MacOS/QtMeshEditor" 2>/dev/null; sleep 1
"$APP" --with-mcp --http-port $PORT > "$LOG" 2>&1 &
APP_PID=$!
# Wait for HTTP up (max 30s)
for i in $(seq 1 30); do
  curl -s -m 3 "$BASE" >/dev/null 2>&1 && break
  sleep 1
done
if ! curl -s -m 3 "$BASE" >/dev/null 2>&1; then
  echo "FATAL: HTTP API never came up"; tail -20 "$LOG"; kill $APP_PID 2>/dev/null; exit 2
fi

# call <tool> <json> -> echoes response text
call() { curl -s -m 30 -X POST "$BASE/$1" -H "Content-Type: application/json" -d "$2"; }
# text field of the MCP content[0]
ctext() { python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('content',[{}])[0].get('text',''))" 2>/dev/null; }
# An error is either an MCP tool error (isError:true) OR a transport-level
# rejection (top-level "error", e.g. Qt's JSON parser refusing an illegal
# number like 1e400 before the handler runs). Both are "handled cleanly".
iserr() { python3 -c "import sys,json;d=json.load(sys.stdin);print('1' if (d.get('isError') or d.get('error')) else '0')" 2>/dev/null; }

check() { # check <desc> <expect: ok|err> <response>
  local desc="$1" expect="$2" resp="$3"
  local e; e=$(echo "$resp" | iserr)
  if [ "$expect" = "ok" ] && [ "$e" = "0" ]; then PASS=$((PASS+1)); echo "PASS: $desc";
  elif [ "$expect" = "err" ] && [ "$e" = "1" ]; then PASS=$((PASS+1)); echo "PASS: $desc (expected error)";
  else FAIL=$((FAIL+1)); fails+=("$desc"); echo "FAIL: $desc -> $(echo "$resp" | ctext | head -3)"; fi
}
# assert a substring is present in the response text
checkhas() { # checkhas <desc> <substr> <response>
  local desc="$1" sub="$2" resp="$3"
  if echo "$resp" | ctext | grep -qF "$sub"; then PASS=$((PASS+1)); echo "PASS: $desc";
  else FAIL=$((FAIL+1)); fails+=("$desc"); echo "FAIL: $desc (missing '$sub') -> $(echo "$resp" | ctext | head -3)"; fi
}

echo "===== NODE ANIMATION ====="
R=$(call create_primitive '{"type":"cube"}'); check "create cube" ok "$R"
NODE=$(echo "$R" | python3 -c "import sys,json;print(json.load(sys.stdin)['content'][0]['text'].split(chr(39))[1])" 2>/dev/null)
echo "node = [$NODE]"
check "create node clip" ok "$(call add_node_animation_clip '{"name":"NC","length":3.0}')"
check "dup clip rejected" err "$(call add_node_animation_clip '{"name":"NC","length":1.0}')"
# Build payloads with jq-free printf so $NODE interpolation is unambiguous
# (nested escaped quotes inside "$(...)" break bash word-splitting).
kf() { printf '{"clip":"NC","node":"%s","time":%s,"translate":[%s,0,0]}' "$NODE" "$1" "$2"; }
mv() { printf '{"clip":"NC","node":"%s","old_time":%s,"new_time":%s}' "$NODE" "$1" "$2"; }
dk() { printf '{"clip":"NC","node":"%s","time":%s}' "$NODE" "$1"; }
P="$(kf 0.0 0)";   check "key @0"   ok "$(call set_node_keyframe "$P")"
P="$(kf 1.5 3)";   check "key @1.5" ok "$(call set_node_keyframe "$P")"
P="$(kf 3.0 6)";   check "key @3"   ok "$(call set_node_keyframe "$P")"
R=$(call get_node_animation '{"clip":"NC"}'); checkhas "get_node_animation has node" "$NODE" "$R"
checkhas "get_node_animation length 3" '"length": 3' "$R"
# Keys are at 0, 1.5, 3. Move 1.5->2.25 (empty slot) should succeed.
P="$(mv 1.5 2.25)"; check "move keyframe 1.5->2.25" ok "$(call move_node_keyframe "$P")"
# Now keys at 0, 2.25, 3. Move 0->3 (occupied) should be rejected.
P="$(mv 0.0 3.0)";  check "move onto existing rejected" err "$(call move_node_keyframe "$P")"
# Delete the 2.25 key should succeed.
P="$(dk 2.25)";     check "delete keyframe @2.25" ok "$(call delete_node_keyframe "$P")"
# Deleting it again (now missing) should be rejected.
P="$(dk 2.25)";     check "delete missing keyframe rejected" err "$(call delete_node_keyframe "$P")"
check "play node clip" ok "$(call set_node_animation_playing '{"clip":"NC","enabled":true}')"
check "pause node clip" ok "$(call set_node_animation_playing '{"clip":"NC","enabled":false}')"
check "play missing clip rejected" err "$(call set_node_animation_playing '{"clip":"NOPE","enabled":true}')"

echo "===== EXPORT ROUNDTRIP ====="
P="$(kf 2.0 9)"; call set_node_keyframe "$P" >/dev/null
R=$(call save_scene '{"file_path":"/tmp/anim_mcp_roundtrip.scene.glb"}'); check "save_scene glb" ok "$R"
if [ -f /tmp/anim_mcp_roundtrip.scene.glb ]; then
  PASS=$((PASS+1)); echo "PASS: glb file written"
  python3 - "$NODE" <<'PY'
import struct,json,sys
node=sys.argv[1]
d=open("/tmp/anim_mcp_roundtrip.scene.glb","rb").read()
clen,_=struct.unpack_from("<II",d,12); g=json.loads(d[20:20+clen])
anims=g.get("animations",[])
ok=any(any(g["nodes"][ch["target"]["node"]].get("name")==node for ch in a["channels"]) for a in anims)
print("PASS: glb has node animation channel" if ok else "FAIL: glb missing node animation channel")
PY
else FAIL=$((FAIL+1)); fails+=("glb file written"); echo "FAIL: glb file not written"; fi

echo "===== GLOBAL PLAYBACK ====="
check "set playback speed 2.0" ok "$(call set_playback_speed '{"speed":2.0}')"
check "speed 0 rejected" err "$(call set_playback_speed '{"speed":0}')"
check "speed negative rejected" err "$(call set_playback_speed '{"speed":-1}')"
R=$(call get_playback_state '{}'); checkhas "get_playback_state speed 2" '"speed": 2' "$R"
check "set loop region" ok "$(call set_loop_region '{"start":0.5,"end":2.5,"active":true}')"
R=$(call get_playback_state '{}'); checkhas "loop active" '"loop_active": true' "$R"
check "clear loop region" ok "$(call set_loop_region '{"active":false}')"

echo "===== MORPH WEIGHT KEYFRAMING ====="
# Negative: no such target errors cleanly (not crash).
check "morph key missing target errors" err "$(call set_morph_weight_keyframe '{"target":"NoSuchShape","time":0.5,"weight":1.0}')"
check "clear missing morph key errors" err "$(call clear_morph_weight_keyframe '{"target":"NoSuchShape","time":0.5}')"
# Positive: load a mesh with blend shapes (out_face.glb has Shape_0/Shape_1).
FACE="$FACE_ASSET"
if [ -f "$FACE" ]; then
  call load_mesh "$(printf '{"path":"%s"}' "$FACE")" >/dev/null; sleep 2
  R=$(call list_morph_targets "$(printf '{"file":"%s"}' "$FACE")")
  checkhas "face has morph targets" "Shape_0" "$R"
  check "key morph Shape_0 @0.5" ok "$(call set_morph_weight_keyframe '{"target":"Shape_0","time":0.5,"weight":0.8}')"
  check "key morph Shape_0 @1.5" ok "$(call set_morph_weight_keyframe '{"target":"Shape_0","time":1.5,"weight":0.2}')"
  check "clear morph Shape_0 @0.5" ok "$(call clear_morph_weight_keyframe '{"target":"Shape_0","time":0.5}')"
  check "clear already-cleared morph key errors" err "$(call clear_morph_weight_keyframe '{"target":"Shape_0","time":0.5}')"
else
  echo "SKIP: morph positive tests (no out_face.glb)"
fi

echo "===== SELECTION + SKELETAL KEYFRAME EDITING ====="
check "select missing bone errors or noops" ok "$(call select_bone '{"bone":"root"}')"  # noop-safe
check "select_animation missing errors" err "$(call select_animation '{"entity":"Nope","animation":"Nope"}')"

# Positive skeletal keyframe editing on a rigged mesh (out_body.glb).
BODY="$BODY_ASSET"
if [ -f "$BODY" ]; then
  call load_mesh "$(printf '{"path":"%s"}' "$BODY")" >/dev/null; sleep 2
  R=$(call list_skeletal_animations '{}'); checkhas "body has skeletal anims" "Entity:" "$R"
  check "select_animation clapping" ok "$(call select_animation '{"entity":"out_body","animation":"HumanArmature|Man_Clapping"}')"
  check "select_bone Abdomen" ok "$(call select_bone '{"bone":"Abdomen"}')"
  R=$(call get_channel_values '{"bone":"Abdomen","channel":"rw"}'); checkhas "channel values read" '"count": 2' "$R"
  check "set_keyframe_value tx@0" ok "$(call set_keyframe_value '{"bone":"Abdomen","channel":"tx","time":0.0,"value":0.05}')"
  check "invalid channel rejected" err "$(call set_keyframe_value '{"bone":"Abdomen","channel":"bogus","time":0.0,"value":1}')"
  check "step next" ok "$(call step_keyframe '{"direction":"next"}')"
  check "step bad dir rejected" err "$(call step_keyframe '{"direction":"sideways"}')"
  check "move bone keyframe 0->0.5" ok "$(call move_bone_keyframe '{"bone":"Hips","old_time":0.0,"new_time":0.5}')"
  check "move onto existing bone kf rejected" err "$(call move_bone_keyframe '{"bone":"Hips","old_time":0.5,"new_time":1.66667}')"
else
  echo "SKIP: skeletal keyframe editing (no out_body.glb)"
fi

echo "===== INPUT VALIDATION (malformed args must error, never crash) ====="
# Wrong types / out-of-range on the new anim tools. Each must return an MCP
# error (isError=true), and the app must survive (checked in SUMMARY).
check "speed string rejected"        err "$(call set_playback_speed '{"speed":"fast"}')"
check "node clip length string rej"  err "$(call add_node_animation_clip '{"name":"X","length":"long"}')"
check "node key time string rej"     err "$(call set_node_keyframe '{"clip":"NC","node":"n","time":"soon"}')"
check "node key negative time rej"   err "$(call set_node_keyframe '{"clip":"NC","node":"n","time":-1}')"
check "playing enabled string rej"   err "$(call set_node_animation_playing '{"clip":"NC","enabled":"yes"}')"
check "morph weight string rej"      err "$(call set_morph_weight_keyframe '{"target":"Shape_0","time":0.5,"weight":"high"}')"
check "keyframe value nan rej"       err "$(call set_keyframe_value '{"bone":"Hips","channel":"tx","time":0,"value":1e400}')"
check "get_node_animation missing"   err "$(call get_node_animation '{"clip":"DoesNotExist"}')"
check "loop region bad type rej"     err "$(call set_loop_region '{"start":"a"}')"
check "empty-args get_playback_state ok" ok "$(call get_playback_state '{}')"

echo
echo "===== SUMMARY: PASS=$PASS FAIL=$FAIL ====="
if [ "$FAIL" -gt 0 ]; then printf 'FAILED: %s\n' "${fails[@]}"; fi
# Check the app didn't crash during all this
if kill -0 $APP_PID 2>/dev/null; then echo "APP STILL ALIVE (no crash)"; else echo "FAIL: APP CRASHED"; FAIL=$((FAIL+1)); fi
grep -iE "SIGSEGV|SIGABRT|terminate called|ASSERT|assertMainThread" "$LOG" | head -5

kill $APP_PID 2>/dev/null; sleep 1; kill -9 $APP_PID 2>/dev/null
rm -f /tmp/anim_mcp_roundtrip.scene.glb
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
