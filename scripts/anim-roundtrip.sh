#!/usr/bin/env bash
# Round-trip verification for #517: export a mesh carrying skeletal + morph +
# node animation to glb/FBX/.mesh, reimport, and report which anim types
# survive (and whether node-anim scale is preserved). Drives the app over the
# HTTP-MCP API. Exit 0 iff all expected checks pass.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${QTMESH_APP:-$ROOT/build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor}"
FACE="${QTMESH_FACE_GLB:-$ROOT/.mocap_work/out_face.glb}"   # morph targets
BODY="${QTMESH_BODY_GLB:-$ROOT/.mocap_work/out_body.glb}"   # skeletal
PORT="${QTMESH_MCP_PORT:-8065}"
BASE="http://localhost:$PORT/api/tools"
LOG=/tmp/anim_rt_app.log
PASS=0; FAIL=0; fails=()

pkill -9 -f "QtMeshEditor" 2>/dev/null; sleep 1
"$APP" --with-mcp --http-port $PORT > "$LOG" 2>&1 &
APP_PID=$!
for i in $(seq 1 40); do curl -s -m 3 "$BASE" >/dev/null 2>&1 && break; sleep 1; done
curl -s -m 3 "$BASE" >/dev/null 2>&1 || { echo "FATAL: HTTP never came up"; tail -20 "$LOG"; kill $APP_PID 2>/dev/null; exit 2; }

call(){ curl -s -m 60 -X POST "$BASE/$1" -H "Content-Type: application/json" -d "$2"; }
ctext(){ python3 -c "import sys,json;print(json.load(sys.stdin).get('content',[{}])[0].get('text',''))" 2>/dev/null; }
note(){ echo "  · $1"; }
ok(){ PASS=$((PASS+1)); echo "PASS: $1"; }
bad(){ FAIL=$((FAIL+1)); fails+=("$1"); echo "FAIL: $1${2:+ -> $2}"; }

# has <desc> <substr> <response-text>
has(){ if echo "$3" | grep -qF "$2"; then ok "$1"; else bad "$1" "$(echo "$3" | head -1)"; fi; }
hasnt(){ if echo "$3" | grep -qF "$2"; then bad "$1"; else ok "$1"; fi; }

# Decode a node clip's translation values from an exported glb (for scale check)
decode_glb_node_tx(){ python3 - "$1" "$2" <<'PY'
import struct,json,sys
path,node=sys.argv[1],sys.argv[2]
try:
  d=open(path,"rb").read();clen,_=struct.unpack_from("<II",d,12);g=json.loads(d[20:20+clen])
  bin_off=20+clen;bclen,_=struct.unpack_from("<II",d,bin_off);binc=d[bin_off+8:bin_off+8+bclen]
  def acc(i):
    a=g["accessors"][i];bv=g["bufferViews"][a["bufferView"]];off=bv.get("byteOffset",0)+a.get("byteOffset",0)
    n={"SCALAR":1,"VEC3":3,"VEC4":4}[a["type"]];return [struct.unpack_from("<"+"f"*n,binc,off+k*4*n) for k in range(a["count"])]
  # find node index by name
  ni=next((i for i,nn in enumerate(g["nodes"]) if nn.get("name")==node), None)
  for an in g.get("animations",[]):
    for ch in an["channels"]:
      if ch["target"].get("node")==ni and ch["target"]["path"]=="translation":
        vs=acc(an["samplers"][ch["sampler"]]["output"]); print("MAXTX", max(abs(v[0]) for v in vs)); sys.exit(0)
  print("MAXTX none")
except Exception as e: print("MAXTX err",e)
PY
}

echo "===== BUILD SCENE (skeletal + morph + node) ====="
# Prefer the RIGGED body (real skeleton) so we test node+skeleton coexistence.
# It has no morph targets, so morph checks are skipped for it. If you want a
# combined asset, set QTMESH_RT_MESH to one with skeleton+morph.
BASE_MESH="${QTMESH_RT_MESH:-$BODY}"
HAS_MORPH=0
case "$BASE_MESH" in *out_face*) HAS_MORPH=1;; esac
R=$(call load_mesh "$(printf '{"path":"%s"}' "$BASE_MESH")"); note "$(echo "$R"|ctext)"
sleep 2
SI=$(call get_scene_info '{}' | ctext)
NODE=$(echo "$SI" | grep -oE 'out_(body|face)[A-Za-z0-9_]*' | head -1)
[ -z "$NODE" ] && NODE="out_body"
note "node=$NODE has_morph=$HAS_MORPH"
call add_node_animation_clip '{"name":"Spin","length":2.0}' >/dev/null
call set_node_keyframe "$(printf '{"clip":"Spin","node":"%s","time":0.0,"translate":[0,0,0]}' "$NODE")" >/dev/null
call set_node_keyframe "$(printf '{"clip":"Spin","node":"%s","time":2.0,"translate":[5,0,0]}' "$NODE")" >/dev/null
# Morph weight animation over time (only when the base mesh has blend shapes).
if [ "$HAS_MORPH" = "1" ]; then
  call set_morph_weight_keyframe '{"target":"Shape_0","time":0.0,"weight":0.0}' >/dev/null
  call set_morph_weight_keyframe '{"target":"Shape_0","time":1.0,"weight":1.0}' >/dev/null
fi

for FMT in glb fbx mesh; do
  case $FMT in
    glb)  OUT=/tmp/rt_out.glb;  FMTSTR="glTF 2.0 Binary (*.glb)";;
    fbx)  OUT=/tmp/rt_out.fbx;  FMTSTR="FBX Binary (*.fbx)";;
    mesh) OUT=/tmp/rt_out.mesh; FMTSTR="Ogre Mesh (*.mesh)";;
  esac
  echo "===== FORMAT: $FMT ====="
  rm -f "$OUT"
  # Export the whole scene (save_scene = glb only). For fbx/mesh use export_mesh (single entity).
  if [ "$FMT" = "glb" ]; then
    R=$(call save_scene "$(printf '{"file_path":"%s"}' "$OUT")")
  else
    R=$(call export_mesh "$(printf '{"name":"%s","path":"%s","format":"%s"}' "$NODE" "$OUT" "$FMTSTR")")
  fi
  if [ -f "$OUT" ]; then ok "$FMT export wrote file"; else bad "$FMT export wrote file" "$(echo "$R"|ctext|head -1)"; continue; fi

  # Reimport into a fresh scene.
  call load_mesh "$(printf '{"path":"%s"}' "$OUT")" >/dev/null; sleep 2

  SK=$(call list_skeletal_animations '{}' | ctext)
  NA=$(call list_node_animations '{}' | ctext)
  MO=$(call list_morph_targets "$(printf '{"file":"%s"}' "$OUT")" | ctext)
  note "skeletal: $(echo "$SK"|tr '\n' ' '|cut -c1-80)"
  note "node: $(echo "$NA"|tr '\n' ' '|cut -c1-80)"
  note "morph: $(echo "$MO"|tr '\n' ' '|cut -c1-80)"

  has "$FMT: skeletal anim survives" "Animation:" "$SK"
  if [ "$HAS_MORPH" = "1" ]; then
    has "$FMT: morph targets survive" "Shape_" "$MO"
  fi
  # Node anim expected for ALL formats now.
  has "$FMT: node anim survives" "\"count\": 1" "$NA"

  # Node-anim scale check (glb only — can decode). Re-export the reimport and
  # compare the max translation magnitude (should stay ~5, not ~0.05).
  if [ "$FMT" = "glb" ]; then
    RNODE=$(call get_scene_info '{}' | ctext | grep -oE 'out_face[A-Za-z0-9_]*|rt_out[A-Za-z0-9_]*|[A-Za-z0-9_]+' | head -1)
    call save_scene '{"file_path":"/tmp/rt_reexport.glb"}' >/dev/null; sleep 1
    # Try each plausible node name.
    MAXTX=""
    for cand in $(call list_node_animations '{}' >/dev/null; python3 -c "import struct,json;d=open('/tmp/rt_reexport.glb','rb').read();c,_=struct.unpack_from('<II',d,12);g=json.loads(d[20:20+c]);print(' '.join(n.get('name','') for n in g['nodes']))" 2>/dev/null); do
      M=$(decode_glb_node_tx /tmp/rt_reexport.glb "$cand"); V=$(echo "$M"|awk '{print $2}')
      if [ "$V" != "none" ] && [ "$V" != "err" ] && [ -n "$V" ]; then MAXTX="$V"; break; fi
    done
    note "reimported node max|tx| = $MAXTX (expected ~5)"
    if [ -n "$MAXTX" ]; then
      python3 -c "import sys;v=float('$MAXTX');sys.exit(0 if 4.0<v<6.0 else 1)" \
        && ok "$FMT: node anim scale preserved (~5)" \
        || bad "$FMT: node anim scale preserved" "got $MAXTX"
    else
      bad "$FMT: node anim scale preserved" "no node tx found"
    fi
  fi
done

echo
echo "===== SUMMARY: PASS=$PASS FAIL=$FAIL ====="
[ "$FAIL" -gt 0 ] && printf 'FAILED: %s\n' "${fails[@]}"
if kill -0 $APP_PID 2>/dev/null; then echo "APP ALIVE (no crash)"; else echo "FAIL: APP CRASHED"; FAIL=$((FAIL+1)); fi
# Hard-kill immediately — a graceful `kill` lets Ogre's slow static-destructor
# teardown run for minutes on macOS, pushing wall-clock past the watch timeout.
kill -9 $APP_PID 2>/dev/null; pkill -9 -f "QtMeshEditor" 2>/dev/null
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
