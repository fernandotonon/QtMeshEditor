"""QTM3D interchange writer/reader.

The QtMeshEditor <-> TRELLIS.2 sidecar interchange container. One little-endian
binary file carrying named typed arrays plus a JSON manifest. Deliberately
trivial so the C++ reader (src/ImageTo3D/Trellis2Interchange.{h,cpp}) needs no
zip/npz/hdf5 dependency.

Layout:
    bytes 0..7   magic  b"QTMESH3D"
    u32          version (currently 1)
    u32          json manifest byte length J
    J bytes      UTF-8 JSON manifest
    padding      zeros up to the next 16-byte boundary (relative to file start)
    blobs        raw little-endian array data; each array's "offset" in the
                 manifest is relative to the START OF THE BLOB SECTION (the
                 16-byte boundary after the JSON), and every blob starts on a
                 16-byte boundary.

Manifest schema (JSON object):
    {
      "generator": "qtmesh-trellis2",
      "formatVersion": 1,
      "meta": { ... free-form generation metadata ... },
      "arrays": {
        "<name>": {"dtype": "f32|f16|u8|u16|u32|i32",
                    "shape": [..], "offset": N, "byteLength": N}
      }
    }

Array conventions used by the TRELLIS.2 backend:
    positions     f32 [N,3]   mesh vertices (TRELLIS space, aabb [-0.5,0.5]^3)
    indices       u32 [M,3]   triangle indices
    voxel_coords  u16 [L,3]   occupied sparse-voxel integer coordinates
    voxel_attrs   u8  [L,6]   base_color.rgb, metallic, roughness, alpha (0..255)
    vertex_colors u8  [N,4]   per-vertex base_color.rgb + alpha (optional)
meta keys: resolution (int grid res), voxelSize (float), origin ([3] floats),
    seed, preset, pipelineType, trellis2Revision, sourceImage.

This file must never import (or require) the prohibited nvdiffrast/nvdiffrec
NVIDIA-licensed packages - they are prohibited across QtMeshEditor's TRELLIS.2
integration (see docs/trellis2-dependencies.md).
"""

from __future__ import annotations

import json
import struct

import numpy as np

MAGIC = b"QTMESH3D"
VERSION = 1

_DTYPES = {
    "f32": np.dtype("<f4"),
    "f16": np.dtype("<f2"),
    "u8": np.dtype("<u1"),
    "u16": np.dtype("<u2"),
    "u32": np.dtype("<u4"),
    "i32": np.dtype("<i4"),
}
_DTYPE_NAMES = {v: k for k, v in _DTYPES.items()}


def _dtype_name(arr: np.ndarray) -> str:
    dt = arr.dtype.newbyteorder("<")
    name = _DTYPE_NAMES.get(np.dtype(dt))
    if name is None:
        raise ValueError(f"unsupported dtype for QTM3D: {arr.dtype}")
    return name


def _align16(n: int) -> int:
    return (n + 15) & ~15


def write(path: str, arrays: dict, meta: dict | None = None) -> None:
    """Write a QTM3D file. `arrays` maps name -> numpy array."""
    entries = {}
    blobs = []
    offset = 0
    for name, arr in arrays.items():
        arr = np.ascontiguousarray(arr)
        data = arr.astype(arr.dtype.newbyteorder("<"), copy=False).tobytes()
        entries[name] = {
            "dtype": _dtype_name(arr),
            "shape": list(arr.shape),
            "offset": offset,
            "byteLength": len(data),
        }
        blobs.append((offset, data))
        offset = _align16(offset + len(data))

    manifest = {
        "generator": "qtmesh-trellis2",
        "formatVersion": VERSION,
        "meta": meta or {},
        "arrays": entries,
    }
    mjson = json.dumps(manifest, separators=(",", ":")).encode("utf-8")

    header_len = len(MAGIC) + 8 + len(mjson)
    blob_base = _align16(header_len)
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<II", VERSION, len(mjson)))
        f.write(mjson)
        f.write(b"\x00" * (blob_base - header_len))
        pos = 0
        for off, data in blobs:
            if off > pos:
                f.write(b"\x00" * (off - pos))
                pos = off
            f.write(data)
            pos += len(data)


def read(path: str) -> tuple[dict, dict]:
    """Read a QTM3D file -> (arrays dict of numpy arrays, meta dict)."""
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:8] != MAGIC:
        raise ValueError("not a QTM3D file")
    version, jlen = struct.unpack_from("<II", blob, 8)
    if version != VERSION:
        raise ValueError(f"unsupported QTM3D version {version}")
    manifest = json.loads(blob[16 : 16 + jlen].decode("utf-8"))
    base = _align16(16 + jlen)
    arrays = {}
    for name, e in manifest["arrays"].items():
        dt = _DTYPES[e["dtype"]]
        start = base + e["offset"]
        raw = blob[start : start + e["byteLength"]]
        arrays[name] = np.frombuffer(raw, dtype=dt).reshape(e["shape"]).copy()
    return arrays, manifest.get("meta", {})
