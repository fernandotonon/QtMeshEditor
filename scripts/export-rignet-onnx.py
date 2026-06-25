#!/usr/bin/env python3
"""Export RigNet to ONNX for #408 (ML auto-rigging).

This is a ONE-TIME, OFFLINE developer tool — it is NOT shipped with the app and
the app never runs Python. The app runs the resulting rignet.onnx in C++ via
ONNX Runtime (see src/RigNetPredictor.cpp).

RigNet: "RigNet: Neural Rigging for Articulated Characters" (Xu, Zhou, Joshi,
Kalogerakis et al., SIGGRAPH 2020).
  Project: https://zhan-xu.github.io/rig-net/
  Code:    https://github.com/zhan-xu/RigNet   (LICENSE: GPL-3.0 for the code;
           the *trained weights* are released for research — confirm terms
           before redistributing the exported .onnx)

WHY THIS SCRIPT EXISTS (the design contract RigNetPredictor targets)
--------------------------------------------------------------------
The published RigNet is PyTorch-Geometric (PyG) and is a MULTI-STAGE pipeline:
  1. GMEdgeConv graph attention over a mesh graph built from geodesic + topo
     edges  ->  per-vertex displacement / attention
  2. joint localisation via mean-shift clustering on the attention field
  3. bone connectivity via a learned cost + minimum spanning tree (BoneNet)
PyG's scatter/gather + dynamic clustering do NOT trace to a single clean ONNX
graph. Exporting therefore means:
  * re-expressing the graph convs with plain torch ops (or torch_scatter ops
    that have ONNX symbolics) so torch.onnx.export can trace them, AND
  * either baking the mean-shift + MST into the graph with fixed iteration
    counts, OR exposing the raw joint/attention tensors and doing clustering +
    MST on the C++ side.
RigNetPredictor takes the SECOND approach for connectivity: it accepts a joint
tensor and (optionally) a parent tensor, and if no parents are emitted it builds
the MST itself. So the minimal viable export only needs to emit joint positions;
emitting parents too is a bonus.

TARGET ONNX I/O CONTRACT (what src/RigNetPredictor.cpp expects)
---------------------------------------------------------------
Inputs  (names matched by substring, order-independent):
  "vertices"  float32 [1, N, 3]   mesh-local verts, normalised to a centred
                                   unit box, +Y up  (the C++ side normalises)
  "edges"     int64   [2, E]      OPTIONAL undirected edge list (both dirs);
                                   only consumed if an input name contains
                                   "edge" or "adj"
Outputs (located by dtype/shape, names ignored):
  a float32 tensor whose last dim == 3   -> joint positions [1, J, 3] or [J, 3]
  an int32/int64 tensor (optional)       -> per-joint parent index [J] (-1=root)
Use dynamic axes for N, E, J so one model serves any mesh.

USAGE
-----
    python3 -m venv venv
    ./venv/bin/pip install torch torch_geometric onnx onnxscript
    # clone RigNet + download its checkpoints per its README, then:
    ./venv/bin/python scripts/export-rignet-onnx.py \
        --rignet-repo /path/to/RigNet --out dist/rignet/rignet.onnx
Then host rignet.onnx (e.g. the QtMeshEditor-models HF repo under rignet/) and
point QTMESH_RIGNET_MODEL_BASE_URL / QSettings ai/rignetModelBaseUrl at it (the
default base URL already includes the rignet/ path).

STATUS: this is a SKELETON of the export — the RigNet graph-conv rewrite is the
substantive work and is left as documented TODOs below, because it depends on
the exact checkpoint layout in the upstream repo (which must be cloned). The
app ships WITHOUT the model and falls back to Pinocchio until the .onnx is
hosted; see RigNetPredictor.h.
"""
import argparse
import sys


def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Export RigNet to ONNX (#408).")
    p.add_argument("--rignet-repo", required=True,
                   help="Path to a clone of github.com/zhan-xu/RigNet with its "
                        "checkpoints downloaded (see that repo's README).")
    p.add_argument("--out", default="dist/rignet/rignet.onnx",
                   help="Output .onnx path (default: dist/rignet/rignet.onnx).")
    p.add_argument("--opset", type=int, default=17,
                   help="ONNX opset (default 17).")
    p.add_argument("--max-verts", type=int, default=5000,
                   help="Dummy vertex count used for tracing (dynamic at runtime).")
    return p


def main() -> int:
    args = build_argparser().parse_args()

    try:
        import torch  # noqa: F401
    except ImportError:
        print("error: install torch + torch_geometric + onnx first "
              "(see the module docstring).", file=sys.stderr)
        return 2

    print(f"[export-rignet] repo   : {args.rignet_repo}")
    print(f"[export-rignet] out    : {args.out}")
    print(f"[export-rignet] opset  : {args.opset}")

    # --- TODO (the substantive export work) -------------------------------
    # 1. sys.path.insert(0, args.rignet_repo); import RigNet's models
    #    (models.GCN / models.SAMPLE etc.) and load the released checkpoint(s).
    # 2. Wrap the joint-prediction sub-network in an nn.Module whose forward is
    #    forward(vertices[1,N,3], edges[2,E]) -> joints[1,J,3] (+ optional
    #    parents[J]). Re-express any torch_geometric MessagePassing layer with
    #    torch ops or torch_scatter ops that have ONNX symbolics so it traces.
    # 3. torch.onnx.export(wrapped, (dummy_vertices, dummy_edges), args.out,
    #        input_names=["vertices", "edges"],
    #        output_names=["joints", "parents"],
    #        dynamic_axes={"vertices": {1: "N"}, "edges": {1: "E"},
    #                      "joints": {1: "J"}, "parents": {0: "J"}},
    #        opset_version=args.opset, dynamo=False)
    # 4. onnx.checker.check_model + a quick onnxruntime smoke run, then verify
    #    the I/O matches the contract in this docstring / RigNetPredictor.h.
    print("error: the RigNet graph-conv → ONNX rewrite is not implemented in "
          "this skeleton — see the TODO block. The app falls back to Pinocchio "
          "until a hosted rignet.onnx matching the documented I/O exists.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
