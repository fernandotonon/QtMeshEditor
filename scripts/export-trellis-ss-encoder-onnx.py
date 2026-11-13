#!/usr/bin/env python3
"""Export the TRELLIS sparse-structure VAE ENCODER to ONNX (offline dev tool, not shipped).

The shape guide (blockout control) of the TRELLIS.2 backend starts the
sparse-structure flow from the SS-VAE latent of the user's guide mesh
(SpaceControl, MIT). TRELLIS.2 reuses TRELLIS-1's sparse-structure VAE
(`ss_enc_conv3d_16l8_fp16`, microsoft/TRELLIS-image-large, MIT), and
trellis.cpp ships only its DECODER, so QtMeshEditor runs the encoder itself
through ONNX Runtime (src/ImageTo3D/BlockoutControl.cpp).

Contract of the exported graph (verified below, the export refuses to write a
graph that does not match torch):
    input  occupancy  float32 [1,1,64,64,64]   surface voxels of the guide, 0/1,
                                               index (x,y,z) of the TRELLIS grid
    output latent     float32 [1,8,16,16,16]   posterior MEAN (no sampling)

usage:
    export-trellis-ss-encoder-onnx.py --repo <TRELLIS checkout> \
        --ckpt-dir <dir with ss_enc_conv3d_16l8_fp16.{json,safetensors}> \
        -o ss_encoder.onnx [--package trellis]
"""
import argparse
import importlib
import json
import os
import sys

import numpy as np
import torch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', required=True, help='directory that contains the TRELLIS python package')
    ap.add_argument('--package', default='trellis', help='package name inside --repo (default trellis)')
    ap.add_argument('--ckpt-dir', required=True)
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()

    sys.path.insert(0, os.path.abspath(a.repo))
    vae = importlib.import_module(a.package + '.models.sparse_structure_vae')
    from safetensors.torch import load_file

    name = 'ss_enc_conv3d_16l8_fp16'
    cfg = json.load(open(os.path.join(a.ckpt_dir, name + '.json')))
    args = dict(cfg['args'])
    args['use_fp16'] = False
    enc = vae.SparseStructureEncoder(**args)
    enc.load_state_dict(load_file(os.path.join(a.ckpt_dir, name + '.safetensors')))
    enc = enc.float().eval()
    enc.dtype = torch.float32

    class Wrap(torch.nn.Module):
        def __init__(self, e):
            super().__init__()
            self.e = e

        def forward(self, occupancy):
            return self.e(occupancy, sample_posterior=False)

    m = Wrap(enc).eval()
    rng = np.random.default_rng(0)
    # A hollow box surface: the kind of input the editor sends.
    occ = np.zeros((64, 64, 64), np.float32)
    occ[12:52, 8:56, 20:44] = 1
    occ[13:51, 9:55, 21:43] = 0
    x = torch.from_numpy(occ)[None, None]
    with torch.no_grad():
        ref = m(x).numpy()
    torch.onnx.export(m, (x,), a.out, input_names=['occupancy'], output_names=['latent'],
                      opset_version=17, dynamo=False)

    import onnxruntime as ort
    s = ort.InferenceSession(a.out, providers=['CPUExecutionProvider'])
    for trial in range(2):
        inp = occ if trial == 0 else (rng.random((64, 64, 64)) > 0.97).astype(np.float32)
        with torch.no_grad():
            want = m(torch.from_numpy(inp)[None, None]).numpy()
        got = s.run(None, {'occupancy': inp[None, None]})[0]
        err = float(np.abs(got - want).max())
        print(json.dumps({'trial': trial, 'shape': list(got.shape), 'max_abs_err': err,
                          'finite': bool(np.isfinite(got).all()), 'std': float(want.std())}))
        if got.shape != (1, 8, 16, 16, 16) or not np.isfinite(got).all() or err > 1e-3:
            os.remove(a.out)
            sys.exit('exported graph does not match torch; removed ' + a.out)
    print('wrote', a.out, os.path.getsize(a.out), 'bytes')


if __name__ == '__main__':
    main()
