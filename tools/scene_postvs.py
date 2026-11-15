# Per-draw post-VS clip-space stats (NDC bbox, w range) for draws on the main depth, plus scan of all
# VS/PS/CS disassembly for reads of per-frame cb (b1) rows 41..48 (bytes 656..783) and 270..275. Run in qrenderdoc --py.
# Env: EDFSR_CAPTURE, EDFSR_OUT (dir), EDFSR_DEPTH (default 4488)
import os, struct, hashlib, traceback, re, collections
out = []
def P(*a): out.append(" ".join(str(x) for x in a))

def main():
    import renderdoc as rd
    od = os.environ["EDFSR_OUT"]; os.makedirs(od, exist_ok=True)
    depth_id = os.environ.get("EDFSR_DEPTH", "4488")
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    acts = []
    def walk(l):
        for a in l:
            if a.children: walk(a.children)
            elif a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch): acts.append(a)
    walk(ctrl.GetRootActions())
    tgt = ctrl.GetDisassemblyTargets(True)[0]
    seen = {}
    rx = re.compile(r"cb(\d+)\[(\d+)\]")
    for a in acts:
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        for sname, stg in (("vs", rd.ShaderStage.Vertex), ("ps", rd.ShaderStage.Pixel), ("cs", rd.ShaderStage.Compute)):
            refl = ps.GetShaderReflection(stg)
            if not refl: continue
            h = hashlib.sha1(bytes(refl.rawBytes)).hexdigest()[:16]
            if (sname, h) in seen: continue
            # which slot is the 4480-byte per-frame cb?
            slots = []
            for i, cb in enumerate(refl.constantBlocks):
                try:
                    d = ps.GetConstantBlock(stg, i, 0).descriptor
                    slots.append((cb.fixedBindNumber, str(d.resource), d.byteOffset))
                except Exception:
                    pass
            pipe = ps.GetComputePipelineObject() if sname == "cs" else ps.GetGraphicsPipelineObject()
            dis = ctrl.DisassembleShader(pipe, refl, tgt)
            body = "\n".join(l for l in dis.split("\n") if "dcl_constantbuffer" not in l)
            regs = collections.Counter((int(m.group(1)), int(m.group(2))) for m in rx.finditer(body))
            seen[(sname, h)] = (a.eventId, slots, regs)
    P("== shaders reading per-frame cb rows 41..48 (bytes 656..783) [slot bound to the 4480B per-frame buffer]")
    pf_res = None
    for (sname, h), (eid, slots, regs) in sorted(seen.items(), key=lambda x: x[1][0]):
        for (slot, res, off) in slots:
            if slot != 1: continue
            r41 = sorted(r for (s, r) in regs if s == 1 and 41 <= r <= 48)
            r270 = sorted(r for (s, r) in regs if s == 1 and 270 <= r <= 275)
            if r41: P("  %s %s first e%d b1=%s@%d rows41-48=%s rows270-275=%s" % (sname, h, eid, res, off, r41, r270))
    P("\n== all VS: b1 rows 270..275 used / any b1 rows / other cbs")
    for (sname, h), (eid, slots, regs) in sorted(seen.items(), key=lambda x: x[1][0]):
        if sname != "vs": continue
        b1 = sorted(r for (s, r) in regs if s == 1)
        oth = sorted(set(s for (s, r) in regs if s != 1))
        P("  vs %s e%d slots=%s b1rows=%s othercb=%s" % (h, eid, [(s, r.replace("ResourceId::", ""), o) for s, r, o in slots], b1, oth))

    P("\n== post-VS clip stats for draws on depth", depth_id)
    for a in acts:
        if not (a.flags & rd.ActionFlags.Drawcall) or not str(a.depthOut).endswith("::" + depth_id): continue
        ctrl.SetFrameEvent(a.eventId, False)
        try:
            m = ctrl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if m.vertexResourceId == rd.ResourceId.Null() or m.vertexByteStride == 0:
                P("  e%d no postvs" % a.eventId); continue
            data = bytes(ctrl.GetBufferData(m.vertexResourceId, m.vertexByteOffset, 0))
            n = min(len(data) // m.vertexByteStride, 200000)
            xs = []; ys = []; ws = []
            for i in range(n):
                x, y, z, w = struct.unpack_from("<4f", data, i * m.vertexByteStride)
                ws.append(w)
                if w > 1e-6: xs.append(x / w); ys.append(y / w)
            if not ws: P("  e%d empty" % a.eventId); continue
            ws.sort()
            P("  e%d verts=%d w[min=%.4g med=%.4g max=%.4g] ndcX[%.3g,%.3g] ndcY[%.3g,%.3g]" % (
                a.eventId, n, ws[0], ws[len(ws) // 2], ws[-1], min(xs) if xs else 0, max(xs) if xs else 0,
                min(ys) if ys else 0, max(ys) if ys else 0))
        except Exception as e:
            P("  e%d err %s" % (a.eventId, e))
    ctrl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    P(traceback.format_exc())
open(os.path.join(os.environ["EDFSR_OUT"], "postvs.txt"), "w").write("\n".join(out))
os._exit(0)
