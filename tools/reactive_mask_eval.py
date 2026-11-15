# Offline: evaluate candidate reactive masks = |final - snapshot| for snapshot points A (after stars) and
# B (game's pre-HUD copy), from reactive_snap.py dumps. Writes mask PNGs + coverage stats.
# Usage: python reactive_mask_eval.py <analysis>/reactive A_eid B_eid
import sys, os, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
d = sys.argv[1]; A = int(sys.argv[2]); B = int(sys.argv[3]); W, H = 1715, 964
eids = [int(x) for x in open(os.path.join(d, "snap_eids.txt")).read().split(",")]
def load(e):
    u = np.fromfile(os.path.join(d, "snap", "e%d.bin" % e), dtype=np.uint32)[:W * H].reshape(H, W)
    def f(bits, mb, sh):
        v = (u >> sh) & ((1 << bits) - 1); m = v & ((1 << mb) - 1); ex = v >> mb
        return np.where(ex == 0, m / (1 << mb) * 2.0 ** -14, (1 + m / (1 << mb)) * 2.0 ** (ex.astype(np.float64) - 15)).astype(np.float32)
    return np.stack([f(11, 6, 0), f(11, 6, 11), f(10, 5, 22)], -1)
def lum(x): return x @ np.array([0.2126, 0.7152, 0.0722], np.float32)
F = load(eids[-1]); od = os.path.join(d, "diff")
for tag, e in (("A", A), ("B", B)):
    S = load(e)
    # perceptual-ish: diff of tonemapped luminance
    tf = lambda x: x / (1 + x)
    r = np.abs(tf(lum(F)) - tf(lum(S)))
    Image.fromarray((np.clip(r * 4, 0, 1) * 255).astype(np.uint8)).save(os.path.join(od, "mask_%s_e%d.png" % (tag, e)))
    print("%s e%-6d coverage  >0.01: %5.1f%%  >0.05: %5.1f%%  >0.2: %5.1f%%" % (tag, e,
          100 * (r > 0.01).mean(), 100 * (r > 0.05).mean(), 100 * (r > 0.2).mean()))
