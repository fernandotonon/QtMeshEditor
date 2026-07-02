# Third-party HDRI environments (Slice F, issue #472)

QtMeshEditor ships a small catalog of CC0 HDR environments under `media/hdri/`
for out-of-the-box PBR image-based lighting. Optional extras can be fetched with
`qtmesh hdri --download <name>` into `<AppData>/hdri/`.

## Bundled Poly Haven assets (CC0)

| File | Poly Haven ID | License |
|------|---------------|---------|
| `studio_neutral.hdr` | [studio_small_09](https://polyhaven.com/a/studio_small_09) | CC0 |
| `sunset_outdoor.hdr` | [sunset_forest](https://polyhaven.com/a/sunset_forest) | CC0 |
| `overcast_outdoor.hdr` | [overcast_soil_puresky](https://polyhaven.com/a/overcast_soil_puresky) | CC0 |
| `indoor_window.hdr` | [anniversary_lounge](https://polyhaven.com/a/anniversary_lounge) | CC0 |

Source: [Poly Haven](https://polyhaven.com/) — HDRIs are **CC0 1.0 Universal**
(public domain). Attribution is appreciated but not required.

Downloads use the Poly Haven API (`https://api.polyhaven.com/files/<id>`) and
pull the **2k** `hdr` variant to keep release size reasonable (~4–6 MB each).

## Synthetic asset

| File | Notes |
|------|-------|
| `flat_grey.hdr` | Generated neutral-grey equirect (no external source). Used for material debugging. |

## Maintainer script

Run `./scripts/download-bundled-hdris.sh` from the repo root to populate
`media/hdri/` before packaging a release. The script also regenerates
`flat_grey.hdr`.
