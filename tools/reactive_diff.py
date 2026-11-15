# Offline: per-draw HDR diffs from reactive_snap.py dumps; thumbnails + stats.
# Usage: python reactive_diff.py <analysis>/reactive
import sys, os, json, numpy as np
from PIL import Image, ImageDraw
d = sys.argv[1]; W, H = 1715, 964
J = json.load(open(os.path.join(d, "scan.json"))); by = {r["eid"]: r for r in J["rows"]}
eids = [int(x) for x in open(os.path.join(d, "snap_eids.txt")).read().split(",")]
def load(e):
    u = np.fromfile(os.path.join(d, "snap", "e%d.bin" % e), dtype=np.uint32)[:W * H].reshape(H, W)
    def f(bits, mb, sh):
        v = (u >> sh) & ((1 << bits) - 1); m = v & ((1 << mb) - 1); ex = v >> mb
        return np.where(ex == 0, m / (1 << mb) * 2.0 ** -14, (1 + m / (1 << mb)) * 2.0 ** (ex.astype(np.float64) - 15)).astype(np.float32)
    return np.stack([f(11, 6, 0), f(11, 6, 11), f(10, 5, 22)], -1)
def tm(x): return (np.clip(x / (1 + x), 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
od = os.path.join(d, "diff"); os.makedirs(od, exist_ok=True)
prev = load(eids[0]); base = prev
Image.fromarray(tm(base)).save(os.path.join(od, "base_e%d.png" % eids[0]))
out = []; tiles = []
for e in eids[1:]:
    cur = load(e); dif = np.abs(cur - prev).max(-1); m = dif > 1e-3
    r = by[e]; n = int(m.sum())
    if n:
        ys, xs = np.nonzero(m); bb = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
    else: bb = None
    lum_add = float((cur - prev).mean(-1)[m].mean()) if n else 0
    out.append("e%-6d VS %s PS %s px=%-7d (%.1f%%) bbox=%s dmean=%+.4f dep=%d/%d bl=%s" % (e, r["vs"]["sha"], (r["ps"] or {}).get("sha"),
               n, 100.0 * n / (W * H), bb, lum_add, r["depth"]["en"], r["depth"]["wr"], r["blend0"]["c"] if r["blend0"]["en"] else "opaque"))
    img = tm(cur).copy(); img[m] = (img[m] * 0.4 + np.array([255, 0, 255]) * 0.6).astype(np.uint8)
    t = Image.fromarray(img).resize((429, 241)); ImageDraw.Draw(t).text((4, 4), "e%d %s" % (e, (r["ps"] or {}).get("sha")), fill=(255, 255, 0))
    tiles.append(t); prev = cur
Image.fromarray(tm(prev)).save(os.path.join(od, "final_e%d.png" % eids[-1]))
cols = 4
for s in range(0, len(tiles), 24):
    chunk = tiles[s:s + 24]; rows_ = (len(chunk) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 429, rows_ * 241))
    for i, t in enumerate(chunk): sheet.paste(t, ((i % cols) * 429, (i // cols) * 241))
    sheet.save(os.path.join(od, "sheet_%02d.png" % (s // 24)))
open(os.path.join(d, "draw_diffs.txt"), "w").write("\n".join(out) + "\n"); print("\n".join(out))
