#!/usr/bin/env python3
"""Convention-free animation fidelity: angle between each bone's WORLD DIRECTION (joint -> its child) in a reference clip and
in a retargeted/captured clip of the SAME rig, sampled at the same NORMALISED times.

Why not world ORIENTATION (creature-retarget-error.py)? Orientation depends on each rig's local bone axes and rest
conventions, so a capture that puts every limb exactly where the reference has it still scores tens of degrees whenever the
retarget re-aligns rest directions. Direction is what the eye sees. Grouped by body part so a failing group points at its cause
(measured on a Mixamo target: spine 25 deg with rest-direction alignment on, 5.6 off; arms 18 deg with it on, 40 off).

  scripts/anim-bone-direction-error.py reference.glb RefClip captured.glb CapturedClip [--samples 12] [-v]

Both clips must be on skeletons with the same bone names (e.g. export the original model and the capture target from the same
file). Times: sample k of N is at k/(N-1) of each clip's own length, so clips of different frame rates / lengths compare by phase.
"""
import sys, math, importlib.util
import numpy as np

here = __file__.rsplit("/", 1)[0]
spec = importlib.util.spec_from_file_location("cre", here + "/creature-retarget-error.py")
cre = importlib.util.module_from_spec(spec); spec.loader.exec_module(cre)


def qrot(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    return v + 2 * np.cross(u, np.cross(u, v) + w * v)


def clip_length(j, acc, clip):
    a = [a for a in j["animations"] if clip in a["name"]][0]
    return max(float(j["accessors"][s["input"]]["max"][0]) for s in a["samplers"])


def skeleton_positions(path, clip, fractions):
    j, acc = cre.load(path)
    nodes = j["nodes"]
    parent = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    anim = [a for a in j["animations"] if clip in a["name"]][0]
    length = clip_length(j, acc, clip)
    rot = {}
    for ch in anim["channels"]:
        if ch["target"]["path"] == "rotation":
            s = anim["samplers"][ch["sampler"]]
            rot[ch["target"]["node"]] = (acc(s["input"]).ravel(), acc(s["output"]), s.get("interpolation", "LINEAR"))
    order = []

    def visit(i):
        order.append(i)
        for c in nodes[i].get("children", []):
            visit(c)

    for r in [i for i in range(len(nodes)) if i not in parent]:
        visit(r)
    frames = []
    for f in fractions:
        t = f * length
        wq, wp = {}, {}
        for i in order:
            n = nodes[i]
            q = cre._sample(rot[i][0], rot[i][1], t, rot[i][2]) if i in rot else np.array(n.get("rotation", [0, 0, 0, 1]), dtype=float)
            tr = np.array(n.get("translation", [0, 0, 0]), dtype=float)
            p = parent.get(i)
            if p is None:
                wq[i], wp[i] = q, tr
            else:
                wq[i] = cre.qmul(wq[p], q)
                wp[i] = wp[p] + qrot(wq[p], tr)
        frames.append(wp)
    return nodes, [n.get("name", "") for n in nodes], frames


GROUPS = {
    "spine": ["Spine", "Spine1", "Spine2", "Neck", "Head"],
    "arms": ["LeftShoulder", "LeftArm", "LeftForeArm", "RightShoulder", "RightArm", "RightForeArm"],
    "legs": ["LeftUpLeg", "LeftLeg", "RightUpLeg", "RightLeg"],
    "hands": ["LeftHand", "RightHand"],
}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if len(args) < 4:
        print(__doc__)
        return 2
    n = int(sys.argv[sys.argv.index("--samples") + 1]) if "--samples" in sys.argv else 12
    if "--samples" in sys.argv:
        args.remove(sys.argv[sys.argv.index("--samples") + 1])
    ref, refclip, cap, capclip = args[:4]
    verbose = "-v" in sys.argv
    fr = np.linspace(0.0, 1.0, n)
    rn, rnames, rf = skeleton_positions(ref, refclip, fr)
    cn, cnames, cf = skeleton_positions(cap, capclip, fr)
    ri = {nm: i for i, nm in enumerate(rnames)}
    ci = {nm: i for i, nm in enumerate(cnames)}
    prefix = next((p for p in ("mixamorig:", "") if p + "Hips" in ri), "")
    everything = []
    for group, bones in GROUPS.items():
        errs = []
        for b in bones:
            name = prefix + b
            if name not in ri or name not in ci:
                continue
            common = [c for c in rn[ri[name]].get("children", []) if rnames[c].startswith(prefix) and rnames[c] in ci]
            if not common:
                continue
            child = common[0]
            per = []
            for t in range(n):
                a = rf[t][child] - rf[t][ri[name]]
                s = cf[t][ci[rnames[child]]] - cf[t][ci[name]]
                if np.linalg.norm(a) < 1e-9 or np.linalg.norm(s) < 1e-9:
                    continue
                per.append(math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(a, s) / np.linalg.norm(a) / np.linalg.norm(s)))))))
            if per:
                errs += per
                if verbose:
                    print("    %-14s mean %5.1f  %s" % (b, np.mean(per), [round(x) for x in per]))
        if errs:
            print("%-6s mean %5.1f deg  (max %5.1f)" % (group, np.mean(errs), np.max(errs)))
            everything += errs
    if not everything:
        print("ERROR: no common bones with children were found")
        return 1
    print("OVERALL %.1f deg" % np.mean(everything))
    return 0


if __name__ == "__main__":
    sys.exit(main())
