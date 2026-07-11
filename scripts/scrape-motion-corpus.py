#!/usr/bin/env python3
"""Assemble a PERMISSIVELY-LICENSED animated-humanoid motion corpus (#838).

OFFLINE dev tool — NOT shipped; the app never runs Python. Slice A of the
text-to-motion v2 epic (#837): discover, license-filter, download and validate
animated rigged models so Slice B can retarget their clips onto the canonical
22-joint skeleton (template library v5) and Slice C can train the
flow-matching motion model on them.

LICENSE POLICY (same bar as fetch-training-rigs.sh / THIRD_PARTY_AI_MODELS.md):
only CC0 and CC-BY assets are accepted. CC-BY attributions are collected into
ATTRIBUTION.md, which MUST ship with anything derived from the corpus
(library JSON, model weights). Mixamo (Adobe EULA), LAFAN1 / Bandai-Namco /
AMASS (non-commercial) and game rips are excluded by policy — do not add
sources without recording their license grant here.

SOURCES
  sketchfab    Sketchfab Data API v3 — search animated+downloadable models,
               filter client-side to CC0/CC-BY by the per-model license slug.
               Search needs no auth; DOWNLOADING requires a (free) API token:
               https://sketchfab.com/settings/password → SKETCHFAB_API_TOKEN.
               Downloads come as glTF archives.
  opengameart  opengameart.org advanced search (3D art, CC0/CC-BY licenses),
               HTML-scraped (no API). Only assets whose license list is a
               subset of {CC0, CC-BY 3.0, CC-BY 4.0} are taken.
  packs        Curated CC0 packs with stable URLs (Quaternius mirror — see
               fetch-training-rigs.sh for why most pack URLs are landing
               pages: hardcoded links rot).

VALIDATION: every downloaded model file is checked with `qtmesh info --json`
(the editor's own loader): kept only if it has a skeleton AND >= 1 skeletal
animation (--keep-static keeps un-animated rigs for other corpora).

OUTPUT LAYOUT
  <out>/raw/<source>/<asset-slug>/...          downloaded files (as fetched)
  <out>/manifest.json                          per-asset provenance:
      { schema: "qtmesh-motion-corpus-v1", assets: [ { source, source_url,
        title, author, author_url, license, license_url, tags, files,
        validation: {file, bones, animations[], vertices} } ] }
  <out>/ATTRIBUTION.md                         CC-BY credit list (ship it!)

USAGE
  python3 scripts/scrape-motion-corpus.py --out motion_corpus \
      --sketchfab "walk animation,run animation,zombie animated" \
      --opengameart "animated character" \
      --packs --max-per-query 24
  SKETCHFAB_API_TOKEN=... to actually download Sketchfab results;
  without it the script records the discovery list with download=skipped.
"""

import argparse
import html as html_mod
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import zipfile

UA = {"User-Agent": "QtMeshEditor-motion-corpus/1.0 "
                    "(+https://github.com/fernandotonon/QtMeshEditor; "
                    "offline dev tool, CC0/CC-BY assets only)"}
OK_LICENSES = {"cc0": "CC0-1.0", "by": "CC-BY-4.0"}   # sketchfab slugs
# The search API returns only {uid, label} for the license (no slug) —
# EXACT label match: "CC Attribution" must not catch "CC
# Attribution-NonCommercial"/-ShareAlike/-NoDerivs variants.
OK_LICENSE_LABELS = {"CC0 Public Domain": "CC0-1.0",
                     "CC Attribution": "CC-BY-4.0"}
MODEL_EXTS = (".glb", ".gltf", ".fbx", ".dae", ".blend")
RATE_SLEEP = 1.5   # politeness delay between remote requests, seconds


def http_json(url, token=None):
    req = urllib.request.Request(url, headers=dict(UA))
    if token:
        req.add_header("Authorization", f"Token {token}")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def http_get(url, dest_path):
    req = urllib.request.Request(url, headers=dict(UA))
    with urllib.request.urlopen(req, timeout=300) as r, \
            open(dest_path, "wb") as f:
        shutil.copyfileobj(r, f)


def slugify(s):
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", s).strip("_")
    return s[:80] or "asset"


# ── validation via the editor's own loader ──────────────────────────────────

def find_qtmesh(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for c in (os.path.join(here, "..", "build_local", "bin", "qtmesh"),
              shutil.which("qtmesh")):
        if c and os.path.exists(c):
            return c
    return None


def validate_asset(qtmesh, asset_dir):
    """Run `qtmesh info --json` on model files; return the best validation
    record (skeleton + most animations) or None if nothing qualifies."""
    best = None
    for root, _dirs, files in os.walk(asset_dir):
        for fn in files:
            if not fn.lower().endswith(MODEL_EXTS):
                continue
            p = os.path.join(root, fn)
            try:
                out = subprocess.run(
                    [qtmesh, "info", p, "--json"],
                    capture_output=True, text=True, timeout=300)
                info = json.loads(out.stdout or "{}")
            except Exception:
                continue
            skel = info.get("skeleton") or {}
            rec = {
                "file": os.path.relpath(p, asset_dir),
                "bones": skel.get("boneCount", 0),
                "animations": [a.get("name") for a in
                               info.get("animations", [])],
                "vertices": info.get("vertices",
                                     info.get("vertexCount", 0)),
            }
            if best is None or (rec["bones"], len(rec["animations"])) > \
                    (best["bones"], len(best["animations"])):
                best = rec
    return best


# ── Sketchfab ────────────────────────────────────────────────────────────────

def scrape_sketchfab(queries, out_raw, max_per_query, token):
    assets = []
    for q in queries:
        url = ("https://api.sketchfab.com/v3/search?type=models"
               "&downloadable=true&animated=true&sort_by=-likeCount&q="
               + urllib.parse.quote(q))
        got = 0
        while url and got < max_per_query:
            try:
                page = http_json(url)
            except Exception as e:
                print(f"  sketchfab search failed for {q!r}: {e}")
                break
            for m in page.get("results", []):
                if got >= max_per_query:
                    break
                lic = (m.get("license") or {})
                slug = (lic.get("slug") or "").lower()
                label = (lic.get("label") or "").strip()
                if slug in OK_LICENSES:
                    lic_id = OK_LICENSES[slug]
                elif label in OK_LICENSE_LABELS:
                    lic_id = OK_LICENSE_LABELS[label]
                else:
                    continue    # the license filter — everything else is policy
                uid = m["uid"]
                title = m.get("name") or uid
                user = m.get("user") or {}
                asset = {
                    "source": "sketchfab",
                    "source_url": m.get("viewerUrl")
                                  or f"https://sketchfab.com/3d-models/{uid}",
                    "title": title,
                    "author": user.get("displayName")
                              or user.get("username", "unknown"),
                    "author_url": user.get("profileUrl", ""),
                    "license": lic_id,
                    "license_url": lic.get("url", ""),
                    "tags": [t.get("slug", "") for t in m.get("tags", [])]
                            + [q],
                    "files": [],
                }
                got += 1
                if not token:
                    asset["download"] = "skipped (SKETCHFAB_API_TOKEN not set)"
                    assets.append(asset)
                    continue
                try:
                    dl = http_json(
                        f"https://api.sketchfab.com/v3/models/{uid}/download",
                        token=token)
                    pick = dl.get("gltf") or dl.get("glb") \
                        or dl.get("source") or {}
                    dl_url = pick.get("url")
                    if not dl_url:
                        raise RuntimeError("no downloadable archive offered")
                    adir = os.path.join(out_raw, "sketchfab",
                                        slugify(f"{title}_{uid[:8]}"))
                    os.makedirs(adir, exist_ok=True)
                    zpath = os.path.join(adir, "model.zip")
                    http_get(dl_url, zpath)
                    with zipfile.ZipFile(zpath) as z:
                        z.extractall(adir)
                    os.remove(zpath)
                    asset["files"] = sorted(
                        os.path.relpath(os.path.join(r, f), adir)
                        for r, _d, fs in os.walk(adir) for f in fs)
                    asset["_dir"] = adir
                    print(f"  sketchfab: {title} [{asset['license']}]")
                except Exception as e:
                    asset["download"] = f"failed: {e}"
                assets.append(asset)
                time.sleep(RATE_SLEEP)
            url = page.get("next")
            time.sleep(RATE_SLEEP)
    return assets


# ── OpenGameArt ──────────────────────────────────────────────────────────────

def scrape_opengameart(queries, out_raw, max_per_query):
    assets = []
    for q in queries:
        # The advanced-search endpoint needs form tokens; the plain search
        # works and the PER-PAGE license block below is the authoritative
        # filter anyway.
        search = ("https://opengameart.org/art-search?keys="
                  + urllib.parse.quote(q))
        try:
            req = urllib.request.Request(search, headers=dict(UA))
            page = urllib.request.urlopen(req, timeout=60).read().decode(
                "utf-8", "replace")
        except Exception as e:
            print(f"  opengameart search failed for {q!r}: {e}")
            continue
        links = []
        for link in re.findall(r'href="(/content/[a-z0-9-]+)"', page):
            if link != "/content/faq" and link not in links:
                links.append(link)
        taken = 0
        for link in links:
            if taken >= max_per_query:
                break
            url = "https://opengameart.org" + link
            try:
                req = urllib.request.Request(url, headers=dict(UA))
                doc = urllib.request.urlopen(req, timeout=60).read().decode(
                    "utf-8", "replace")
            except Exception:
                continue
            if "3D Art" not in doc:
                continue                      # 2D asset — not our corpus
            # License(s) block: every listed license must be acceptable.
            lic_block = re.search(
                r'field-name-field-art-licenses.*?(</div>\s*){4}', doc, re.S)
            names = re.findall(r"license-name'>([^<]+)<",
                               lic_block.group(0) if lic_block else doc)
            names = [n.strip() for n in names]
            if not names or not all(
                    n in ("CC0", "CC-BY 3.0", "CC-BY 4.0") for n in names):
                continue
            title_m = re.search(r"<title>([^<|]+)", doc)
            author_m = re.search(r'/users/[^"]+"[^>]*>([^<]+)</a>', doc)
            # Attachment links only (not css/preview images): the art-files
            # field lists direct /sites/default/files/ URLs to zips/models.
            file_urls = [u for u in re.findall(
                r'href="(https://opengameart\.org/sites/default/files/'
                r'[^"]+)"', doc)
                if u.lower().split("?")[0].endswith(
                    MODEL_EXTS + (".zip", ".7z"))]
            if not file_urls:
                continue
            asset = {
                "source": "opengameart",
                "source_url": url,
                "title": html_mod.unescape(
                    (title_m.group(1) if title_m else link).strip()),
                "author": html_mod.unescape(
                    author_m.group(1).strip()) if author_m else "unknown",
                "author_url": "",
                "license": "CC0-1.0" if names == ["CC0"] else
                           ("CC-BY-4.0" if "CC-BY 4.0" in names
                            else "CC-BY-3.0"),
                "license_url": "",
                "tags": [q],
                "files": [],
            }
            adir = os.path.join(out_raw, "opengameart",
                                slugify(asset["title"]))
            os.makedirs(adir, exist_ok=True)
            for fu in file_urls[:6]:
                fn = slugify(os.path.basename(
                    urllib.parse.unquote(fu.split("?")[0])))
                try:
                    dest = os.path.join(adir, fn)
                    http_get(fu, dest)
                    if fn.lower().endswith(".zip"):
                        with zipfile.ZipFile(dest) as z:
                            z.extractall(adir)
                        os.remove(dest)
                except Exception:
                    continue
                time.sleep(RATE_SLEEP)
            asset["files"] = sorted(
                os.path.relpath(os.path.join(r, f), adir)
                for r, _d, fs in os.walk(adir) for f in fs)
            if asset["files"]:
                asset["_dir"] = adir
                print(f"  opengameart: {asset['title']} "
                      f"[{asset['license']}]")
                assets.append(asset)
                taken += 1
            else:
                shutil.rmtree(adir, ignore_errors=True)
            time.sleep(RATE_SLEEP)
    return assets


# ── curated CC0 packs (Quaternius mirror, enumerated live) ──────────────────
#
# The mirror repo also used by fetch-training-rigs.sh. ONLY the free-pack
# subfolders under "Characters and Animals/" are taken (they mirror the packs
# Quaternius publishes as CC0 on quaternius.com); "[Patreon Exclusive]"
# folders are skipped, and each pack is recorded as its own manifest row.
# Paths are enumerated via the GitHub tree API instead of hardcoded URLs —
# hardcoded pack links rot (see fetch-training-rigs.sh header).

QUATERNIUS_MIRROR = "beep2bleep/FreeAssetsByKenneyNLandQuaternius"
QUATERNIUS_ROOT = "FreeModels by Quaternius[Patreon]/Characters and Animals/"


def scrape_packs(out_raw, max_per_pack=8):
    try:
        tree = http_json(f"https://api.github.com/repos/{QUATERNIUS_MIRROR}"
                         "/git/trees/master?recursive=1")
    except Exception as e:
        print(f"  pack mirror enumeration failed: {e}")
        return []
    by_pack = {}
    for t in tree.get("tree", []):
        p = t.get("path", "")
        if (t.get("type") != "blob"
                or not p.startswith(QUATERNIUS_ROOT)
                or "Exclusive" in p
                or not p.lower().endswith((".fbx", ".glb"))):
            continue
        pack = p[len(QUATERNIUS_ROOT):].split("/")[0]
        # Animated packs carry it in the pack name; static ones are for
        # other corpora (validation drops them anyway unless --keep-static).
        by_pack.setdefault(pack, []).append(p)

    assets = []
    for pack, paths in sorted(by_pack.items()):
        # Prefer FBX (the animated flavour in these packs) over GLB dupes.
        paths = sorted(paths,
                       key=lambda p: (not p.lower().endswith(".fbx"), p))
        adir = os.path.join(out_raw, "packs", slugify(pack))
        os.makedirs(adir, exist_ok=True)
        files = []
        for p in paths[:max_per_pack]:
            fn = slugify(os.path.basename(p))
            u = (f"https://raw.githubusercontent.com/{QUATERNIUS_MIRROR}"
                 f"/master/" + urllib.parse.quote(p))
            try:
                http_get(u, os.path.join(adir, fn))
                files.append(fn)
            except Exception as e:
                print(f"  pack file failed ({fn}): {e}")
            time.sleep(RATE_SLEEP)
        if files:
            print(f"  pack: {pack} ({len(files)} file(s))")
            assets.append({
                "source": "packs",
                "source_url": "https://quaternius.com/packs/",
                "title": f"Quaternius — {pack}",
                "author": "Quaternius", "author_url":
                    f"https://github.com/{QUATERNIUS_MIRROR}",
                "license": "CC0-1.0",
                "license_url":
                    "https://creativecommons.org/publicdomain/zero/1.0/",
                "tags": [w.lower() for w in re.findall(r"[A-Za-z]+", pack)],
                "files": files, "_dir": adir,
            })
        else:
            shutil.rmtree(adir, ignore_errors=True)
    return assets


# ── manifest + attribution ───────────────────────────────────────────────────

def write_outputs(out_dir, assets):
    for a in assets:
        d = a.pop("_dir", None)
        if d:
            a["dir"] = os.path.relpath(d, out_dir)
    # Incremental: merge with any manifest already in the corpus dir so
    # successive runs (different sources/queries) accumulate; an asset seen
    # again (same source_url) is replaced by the fresh record.
    prior_path = os.path.join(out_dir, "manifest.json")
    if os.path.exists(prior_path):
        try:
            prior = json.load(open(prior_path)).get("assets", [])
        except Exception:
            prior = []
        fresh = {a["source_url"] for a in assets}
        assets = [a for a in prior if a.get("source_url") not in fresh] \
            + assets
    manifest = {"schema": "qtmesh-motion-corpus-v1", "assets": assets}
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)

    by = [a for a in assets if a["license"].startswith("CC-BY")]
    lines = ["# Attribution",
             "",
             "This corpus (and anything derived from it: motion libraries,"
             " trained weights)",
             "includes CC-BY licensed work requiring the following credits:",
             ""]
    for a in sorted(by, key=lambda a: a["title"].lower()):
        lines.append(f"- \"{a['title']}\" by {a['author']}"
                     f" ({a['source_url']}) — {a['license']}")
    if not by:
        lines.append("_All corpus assets are CC0 — no attribution required"
                     " (kept for provenance in manifest.json)._")
    with open(os.path.join(out_dir, "ATTRIBUTION.md"), "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="motion_corpus")
    ap.add_argument("--sketchfab", default="",
                    help="comma-separated search queries")
    ap.add_argument("--opengameart", default="",
                    help="comma-separated search queries")
    ap.add_argument("--packs", action="store_true",
                    help="fetch the curated CC0 pack list")
    ap.add_argument("--max-per-query", type=int, default=24)
    ap.add_argument("--qtmesh", default="",
                    help="qtmesh binary for validation "
                         "(default: build_local/bin/qtmesh or PATH)")
    ap.add_argument("--keep-static", action="store_true",
                    help="keep rigged-but-unanimated assets too")
    args = ap.parse_args()

    out_raw = os.path.join(args.out, "raw")
    os.makedirs(out_raw, exist_ok=True)
    token = os.environ.get("SKETCHFAB_API_TOKEN", "")

    assets = []
    if args.sketchfab:
        print("== sketchfab ==" + ("" if token else
              "  (no SKETCHFAB_API_TOKEN — discovery only)"))
        assets += scrape_sketchfab(
            [q.strip() for q in args.sketchfab.split(",") if q.strip()],
            out_raw, args.max_per_query, token)
    if args.opengameart:
        print("== opengameart ==")
        assets += scrape_opengameart(
            [q.strip() for q in args.opengameart.split(",") if q.strip()],
            out_raw, args.max_per_query)
    if args.packs:
        print("== packs ==")
        assets += scrape_packs(out_raw, max_per_pack=args.max_per_query)

    qtmesh = find_qtmesh(args.qtmesh)
    if qtmesh:
        print(f"== validating with {qtmesh} ==")
        kept = []
        for a in assets:
            adir = a.get("_dir")
            if not adir:
                kept.append(a)          # discovery-only entries stay listed
                continue
            v = validate_asset(qtmesh, adir)
            if v and v["bones"] > 0 and (v["animations"]
                                         or args.keep_static):
                a["validation"] = v
                kept.append(a)
                print(f"  OK   {a['title']}: {v['bones']} bones, "
                      f"{len(v['animations'])} animation(s)")
            else:
                print(f"  DROP {a['title']}: no skeleton/animation")
                shutil.rmtree(adir, ignore_errors=True)
        assets = kept
    else:
        print("qtmesh not found — skipping validation "
              "(pass --qtmesh or build the CLI)")

    write_outputs(args.out, assets)
    n_dl = sum(1 for a in assets if a.get("files"))
    print(f"\n{len(assets)} asset(s) recorded ({n_dl} downloaded) → "
          f"{args.out}/manifest.json + ATTRIBUTION.md")


if __name__ == "__main__":
    main()
