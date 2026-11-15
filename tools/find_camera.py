# Find per-frame camera constant buffers in an ED capture. Run inside qrenderdoc --py.
# Env: EDFSR_CAPTURE, EDFSR_OUT (text report), EDFSR_DEPTH (main depth resource id number, e.g. 4488)
import os, sys, struct, hashlib, traceback, collections
out = []
def P(*a): out.append(" ".join(str(x) for x in a))

def get_cb(ctrl, ps, stage, refl, rd):
    res = []
    for i, cb in enumerate(refl.constantBlocks):
        try:
            b = ps.GetConstantBlock(stage, i, 0)
        except Exception as e:
            P("GetConstantBlock err", e); continue
        d = getattr(b, "descriptor", b)
        rid = getattr(d, "resource", None) or getattr(b, "resourceId", None)
        off = getattr(d, "byteOffset", getattr(b, "byteOffset", 0))
        size = getattr(d, "byteSize", getattr(b, "byteSize", 0)) or cb.byteSize
        size = min(size, cb.byteSize) if cb.byteSize else size
        if rid is None or rid == rd.ResourceId.Null(): continue
        data = bytes(ctrl.GetBufferData(rid, off, size))
        res.append((i, str(rid), off, data))
    return res

def proj_score(m):
    # m: 16 floats. Check row-major and column-major perspective patterns.
    best = None
    for name, g in (("row", lambda r, c: m[r * 4 + c]), ("col", lambda r, c: m[c * 4 + r])):
        sx, sy = g(0, 0), g(1, 1)
        if abs(sx) < 0.05 or abs(sy) < 0.05: continue
        zeros = [g(0, 1), g(0, 3), g(1, 0), g(1, 3), g(3, 0), g(3, 1)]
        if max(abs(z) for z in zeros) > 1e-3: continue
        w = abs(g(2, 3)) if abs(abs(g(2, 3)) - 1) < 1e-3 else abs(g(3, 2))
        if abs(w - 1) > 1e-3: continue
        best = (name, sy / sx)
    return best

def main():
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    depth_id = os.environ.get("EDFSR_DEPTH", "4488")
    acts = []
    def walk(l):
        for a in l:
            if a.children: walk(a.children)
            elif (a.flags & rd.ActionFlags.Drawcall) and str(a.depthOut).endswith("::" + depth_id): acts.append(a)
    walk(ctrl.GetRootActions())
    P("scene draws on depth", depth_id, ":", len(acts), "eids", acts[0].eventId if acts else None, "..", acts[-1].eventId if acts else None)
    step = max(1, len(acts) // 60)
    seen = collections.defaultdict(lambda: collections.Counter())  # (rid,off,size) -> data hash counts
    datas = {}
    slots = collections.defaultdict(set)
    for a in acts[::step]:
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        for sname, stg in (("vs", rd.ShaderStage.Vertex), ("ps", rd.ShaderStage.Pixel)):
            refl = ps.GetShaderReflection(stg)
            if not refl: continue
            for i, rid, off, data in get_cb(ctrl, ps, stg, refl, rd):
                k = (rid, off, len(data))
                h = hashlib.sha1(data).hexdigest()[:10]
                seen[k][h] += 1; datas[(k, h)] = data
                slots[k].add("%s%d" % (sname, refl.constantBlocks[i].fixedBindNumber))
    n = len(acts[::step])
    P("sampled draws:", n)
    P("\n== buffers used by >=30% of sampled draws (likely per-frame/per-view)")
    for k, cnt in sorted(seen.items(), key=lambda x: -sum(x[1].values())):
        tot = sum(cnt.values())
        if tot < 0.3 * n: continue
        h, c = cnt.most_common(1)[0]
        data = datas[(k, h)]
        P("\n%s off=%d size=%d used=%d distinct=%d slots=%s" % (k[0], k[1], k[2], tot, len(cnt), sorted(slots[k])))
        fl = struct.unpack("<%df" % (len(data) // 4), data[: len(data) // 4 * 4])
        for r in range(0, len(fl), 4):
            P("  [%3d] %s" % (r * 4, "  ".join("%12.6g" % v for v in fl[r:r + 4])))
        for o in range(0, len(fl) - 15, 4):
            s = proj_score(fl[o:o + 16])
            if s: P("  >> PROJECTION-like at byte %d: %s aspect(sy/sx)=%.4f" % (o * 4, s[0], s[1]))
    P("\n== projection-like 4x4 in ANY sampled cbuffer")
    for (k, h), data in datas.items():
        fl = struct.unpack("<%df" % (len(data) // 4), data[: len(data) // 4 * 4])
        for o in range(0, len(fl) - 15, 4):
            s = proj_score(fl[o:o + 16])
            if s: P("%s off=%d size=%d slots=%s byte %d %s aspect=%.4f  m=%s" % (k[0], k[1], k[2], sorted(slots[k]), o * 4, s[0], s[1],
                    " ".join("%.5g" % v for v in fl[o:o + 16])))
    ctrl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    P(traceback.format_exc())
with open(os.environ["EDFSR_OUT"], "w") as f:
    f.write("\n".join(out))
os._exit(0)
