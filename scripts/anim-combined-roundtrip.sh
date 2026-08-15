#!/usr/bin/env bash
# Combined skeletal + morph + node round-trip regression (#517).
#
# Guards the bug where a mesh carrying BOTH a skeletal animation AND a morph
# (blend-shape) weight clip exported a glb with two same-named "MorphAnim"
# animations → Ogre refused to re-import it (duplicate animation name) → the
# whole file failed to load (0 entities), so node/skeletal/morph all appeared
# "not exported". Root cause + fix: commit 2528c082.
#
# The fixture (tests/fixtures/combined_skel_morph.glb) is out_body.glb with 2
# morph POSITION targets + a MorphAnim weights animation grafted onto the
# skinned mesh — the smallest asset that has a real skeleton AND a morph clip.
#
# Drives the app over HTTP-MCP. Exit 0 iff every check passes.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${QTMESH_APP:-$ROOT/build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor}"
FIX="${QTMESH_COMBINED_GLB:-$ROOT/tests/fixtures/combined_skel_morph.glb}"
PORT="${QTMESH_MCP_PORT:-8069}"
BASE="http://localhost:$PORT/api/tools"
LOG=/tmp/anim_combined_app.log
PASS=0; FAIL=0; fails=()

pkill -9 -f "QtMeshEditor" 2>/dev/null; sleep 2
[ -f "$FIX" ] || { echo "FATAL: fixture missing: $FIX"; exit 2; }
"$APP" --with-mcp --http-port $PORT > "$LOG" 2>&1 &
APP_PID=$!
for i in $(seq 1 40); do curl -s -m 3 "$BASE" >/dev/null 2>&1 && break; sleep 1; done
curl -s -m 3 "$BASE" >/dev/null 2>&1 || { echo "FATAL: HTTP never up"; tail -20 "$LOG"; kill -9 $APP_PID 2>/dev/null; exit 2; }

call(){ curl -s -m 90 -X POST "$BASE/$1" -H "Content-Type: application/json" -d "$2"; }
ctext(){ python3 -c "import sys,json;print(json.load(sys.stdin).get('content',[{}])[0].get('text',''))" 2>/dev/null; }
ok(){ PASS=$((PASS+1)); echo "PASS: $1"; }
bad(){ FAIL=$((FAIL+1)); fails+=("$1"); echo "FAIL: $1${2:+ -> $2}"; }
has(){ if echo "$3" | grep -qF "$2"; then ok "$1"; else bad "$1" "$(echo "$3"|head -1)"; fi; }

# ── Load the combined fixture; add a node clip so all three types coexist ──
call load_mesh "$(printf '{"path":"%s"}' "$FIX")" >/dev/null; sleep 2
SI=$(call get_scene_info '{}' | ctext)
NODE=$(echo "$SI" | grep -oE 'combined_skel_morph[A-Za-z0-9_]*' | head -1)
[ -z "$NODE" ] && NODE="combined_skel_morph"
echo "  node=$NODE"
has "fixture loaded (entity present)" "combined_skel_morph" "$SI"

call add_node_animation_clip '{"name":"Spin","length":2.0}' >/dev/null
call set_node_keyframe "$(printf '{"clip":"Spin","node":"%s","time":0.0,"translate":[0,0,0]}' "$NODE")" >/dev/null
call set_node_keyframe "$(printf '{"clip":"Spin","node":"%s","time":2.0,"translate":[5,0,0]}' "$NODE")" >/dev/null

rm -f /tmp/combined_out.glb
call export_mesh "$(printf '{"name":"%s","path":"/tmp/combined_out.glb","format":"glTF 2.0 Binary (*.glb)"}' "$NODE")" >/dev/null
[ -f /tmp/combined_out.glb ] && ok "combined glb export wrote file" || bad "combined glb export wrote file"

# ── The core regression: no duplicate animation names in the exported glb ──
DUPCHECK=$(python3 - <<'PY'
import struct,json
from collections import Counter
d=open("/tmp/combined_out.glb","rb").read()
c,_=struct.unpack_from("<II",d,12);g=json.loads(d[20:20+c])
dups={n:k for n,k in Counter(a.get("name") for a in g.get("animations",[])).items() if k>1}
print("DUPS" if dups else "CLEAN", dups)
PY
)
echo "  $DUPCHECK"
if echo "$DUPCHECK" | grep -q "^CLEAN"; then ok "no duplicate animation names in export"; else bad "no duplicate animation names in export" "$DUPCHECK"; fi

# ── The user-visible symptom: re-import must succeed (was 0 entities) ──
call load_mesh '{"path":"/tmp/combined_out.glb"}' >/dev/null; sleep 2
SI2=$(call get_scene_info '{}' | ctext)
ENTS=$(echo "$SI2" | grep -oE 'Entities: [0-9]+' | grep -oE '[0-9]+')
echo "  entities after reimport: ${ENTS:-?}"
if [ "${ENTS:-0}" -ge 2 ]; then ok "combined glb re-imports (entity count grew)"; else bad "combined glb re-imports" "entities=$ENTS (0 = the old duplicate-name load failure)"; fi

# ── All three animation kinds survive the round-trip ──
SK=$(call list_skeletal_animations '{}' | ctext)
NA=$(call list_node_animations '{}' | ctext)
MO=$(call list_morph_targets '{"file":"/tmp/combined_out.glb"}' | ctext)
has "skeletal survives combined round-trip" "Animation:" "$SK"
has "node clip survives combined round-trip" "Spin" "$NA"
has "morph shapes survive combined round-trip" "Shape_" "$MO"

# ── Reimport hygiene (the "dope sheet shows only morph, wrong keyframes" bug) ──
# On reimport, node/pose-shape animations must NOT leak into the SKELETAL list
# as phantom clips (AnimationProcessor drops 0-node-track clips), the node clip
# must be RECONSTRUCTED even though the scene node was renamed after the file
# (name-match relaxed to the aiScene root name), and the auto-selected animation
# must be the REAL skeletal clip — not a length-0 "Shape_N" pose that renders an
# empty dope sheet. Guards commits: this slice.
# NB load_mesh is additive: the authored "Spin" stays in-scene and the
# reimported file contributes another (uniquified "Spin_1"), so expect >=2.
NACOUNT=$(echo "$NA" | grep -oE '"count": *[0-9]+' | grep -oE '[0-9]+' | head -1)
if [ "${NACOUNT:-0}" -ge 2 ]; then ok "node clip reconstructed on reimport (not leaked as skeletal)"; else bad "node clip reconstructed on reimport" "node anims: $(echo "$NA"|tr -d '\n')"; fi
PS=$(call get_playback_state '{}' | ctext)
SEL=$(echo "$PS" | grep -oE '"selected_animation":[^,]*' | head -1)
LEN=$(echo "$PS" | grep -oE '"length": *[0-9.]+' | grep -oE '[0-9.]+' | head -1)
echo "  reimport auto-selected: $SEL (length $LEN)"
# The real skeletal clip has non-zero length; a leaked Shape_N pose is length 0.
if echo "$SEL" | grep -qiE 'Shape_'; then
  bad "reimport does not auto-select a pose-shape clip" "$SEL"
else
  ok "reimport does not auto-select a pose-shape clip"
fi
if [ -n "$LEN" ] && python3 -c "import sys;sys.exit(0 if float('$LEN')>0.01 else 1)" 2>/dev/null; then
  ok "reimport auto-selects a real (non-zero-length) animation"
else
  bad "reimport auto-selects a real animation" "selected length=$LEN"
fi

echo
echo "===== SUMMARY: PASS=$PASS FAIL=$FAIL ====="
[ "$FAIL" -gt 0 ] && printf 'FAILED: %s\n' "${fails[@]}"
if kill -0 $APP_PID 2>/dev/null; then echo "APP STILL ALIVE (no crash)"; else echo "FAIL: APP CRASHED"; FAIL=$((FAIL+1)); fi
kill -9 $APP_PID 2>/dev/null; pkill -9 -f "QtMeshEditor" 2>/dev/null
rm -f /tmp/combined_out.glb
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
