#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# The cut-guard test set from CC-BY films (CI only; vsplugin-gpu.yml, job cutset). For each film:
#   1. TransNetV2 (MIT, pinned commit) and PySceneDetect (BSD-3) on the whole film;
#   2. labels: a hard cut where both detectors agree within one frame and TransNetV2's transition run is at most two
#      frames long; a gradual transition (dissolve, fade) where TransNetV2's run is three frames or longer; negatives
#      at random places at least 12 frames from any candidate of either detector;
#   3. 7-frame windows around each label (the boundary between offsets 3 and 4), downscaled to 640 px wide, written
#      to <out>/<film>.y4m with one label per line in <out>/<film>.labels, and the frame numbers in <out>/<film>.json
#      (frozen labels: only these small files would ever be committed, never the media).
# The labels are the two detectors' consensus, not a human verdict; <out>/<film>-review.jpg shows prev | next of every
# hard-cut candidate on which they disagreed, for a later look.
# Usage: cutset.py --film name=url ... --out dir [--negatives 300] [--transnet dir-of-the-TransNetV2-checkout]
import argparse, json, os, random, subprocess, sys

def run_transnet(path, transnet_dir):
    sys.path.insert(0, os.path.join(transnet_dir, "inference"))
    from transnetv2 import TransNetV2   # noqa: E402 (the pinned checkout)
    model = TransNetV2(os.path.join(transnet_dir, "inference", "transnetv2-weights"))
    _, single, _ = model.predict_video(path)
    return [float(x) for x in single.reshape(-1)]

def run_scenedetect(path):
    from scenedetect import open_video, SceneManager
    from scenedetect.detectors import ContentDetector
    video = open_video(path)
    sm = SceneManager()
    sm.add_detector(ContentDetector())
    sm.auto_downscale = True
    sm.detect_scenes(video)
    return [s[0].get_frames() for s in sm.get_scene_list()[1:]]

def runs_above(p, thr=0.5):
    out, start = [], None
    for i, v in enumerate(p + [0.0]):
        if v > thr and start is None: start = i
        if v <= thr and start is not None: out.append((start, i - 1)); start = None
    return out

def probe(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_packets", "-show_entries",
                        "stream=width,height,nb_read_packets", "-of", "json", path], capture_output=True, text=True, check=True)
    s = json.loads(r.stdout)["streams"][0]
    return int(s["width"]), int(s["height"]), int(s["nb_read_packets"])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--film", action="append", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--negatives", type=int, default=300)
    ap.add_argument("--transnet", default="")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    random.seed(1)
    summary = {}
    for spec in a.film:
        name, path = spec.split("=", 1)
        w, h, n = probe(path)
        tn = []
        if a.transnet:
            try: tn = run_transnet(path, a.transnet)
            except Exception as e: print(f"::warning::TransNetV2 failed on {name}: {e}")
        sd = run_scenedetect(path)
        tn_runs = runs_above(tn) if tn else []
        hard, gradual, disagree = [], [], []
        sd_set = set(sd)
        for (s, e) in tn_runs:
            if e - s + 1 >= 3: gradual.append((s + e) // 2)
            else:
                f = e + 1   # the first frame after the transition run: the cut lies between f - 1 and f
                if any(abs(f - x) <= 1 for x in sd_set): hard.append(f)
                else: disagree.append(f)
        if not tn:   # without TransNetV2 (weights not fetched): PySceneDetect alone, flagged in the summary
            hard = list(sd)
        for x in sd:
            if tn and not any(abs(x - f) <= 1 for f in hard) and not any(abs(x - m) <= 6 for m in gradual): disagree.append(x)
        cand = sorted(set(hard) | set(gradual) | set(disagree) | set(sd))
        negs = []
        tries = 0
        while len(negs) < a.negatives and tries < 100000:
            tries += 1
            g = random.randint(10, n - 10)
            if all(abs(g - c) >= 12 for c in cand) and all(abs(g - x) >= 8 for x in negs): negs.append(g)
        # windows: (first frame, label); disjoint, in frame order
        wins = [(f - 4, "hard") for f in hard] + [(m - 3, "gradual") for m in gradual] + [(g - 3, "none") for g in negs]
        wins = sorted(x for x in wins if x[0] >= 0 and x[0] + 7 <= n)
        keep, last = [], -100
        for s, lab in wins:
            if s >= last + 7: keep.append((s, lab)); last = s
        ow = 640; oh = int(round(h * ow / w / 2)) * 2
        need = {}
        for i, (s, lab) in enumerate(keep):
            for k in range(7): need[s + k] = i
        y4m = open(os.path.join(a.out, f"{name}.y4m"), "wb")
        y4m.write(f"YUV4MPEG2 W{ow} H{oh} F24:1 Ip A1:1 C420jpeg\n".encode())
        fs = ow * oh * 3 // 2
        p = subprocess.Popen(["ffmpeg", "-v", "error", "-i", path, "-vf", f"scale={ow}:{oh}", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], stdout=subprocess.PIPE)
        idx = 0
        written = 0
        while True:
            buf = p.stdout.read(fs)
            if len(buf) < fs: break
            if idx in need:
                y4m.write(b"FRAME\n"); y4m.write(buf); written += 1
            idx += 1
        p.wait(); y4m.close()
        complete = written // 7
        with open(os.path.join(a.out, f"{name}.labels"), "w") as f:
            for s, lab in keep[:complete]: f.write(lab + "\n")
        with open(os.path.join(a.out, f"{name}.json"), "w") as f:
            json.dump({"film": name, "frames": n, "size": [w, h], "transnet": bool(tn), "hard": hard, "gradual": gradual,
                       "disagree": sorted(set(disagree)), "windows": [{"first": s, "label": lab} for s, lab in keep[:complete]]}, f, indent=1)
        summary[name] = {"frames": n, "hard": len(hard), "gradual": len(gradual), "disagree": len(set(disagree)), "windows": complete, "transnet": bool(tn)}
        print(name, summary[name])
        # review sheet for the disagreements (prev | next thumbnails)
        if disagree:
            sel = "+".join(f"eq(n\\,{x - 1})+eq(n\\,{x})" for x in sorted(set(disagree))[:60])
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path, "-vf", f"select='{sel}',scale=240:-2,tile=2x30", "-frames:v", "1",
                            os.path.join(a.out, f"{name}-review.jpg")], check=False)
    json.dump(summary, open(os.path.join(a.out, "summary.json"), "w"), indent=1)

if __name__ == "__main__":
    main()
