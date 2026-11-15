# Map scene draws: VS/PS hashes, cbuffer bindings, depth-stencil state, viewports; dump VS disassembly;
# depth texture flags; API call order around EASU dispatch. Run inside qrenderdoc --py.
# Env: EDFSR_CAPTURE, EDFSR_OUT (dir), EDFSR_DEPTH (default 4488), EDFSR_EASU (default 2f7d47465d50053c)
import os, sys, struct, hashlib, traceback, collections
out = []
def P(*a): out.append(" ".join(str(x) for x in a))

def shash(refl):
    return hashlib.sha1(bytes(refl.rawBytes)).hexdigest()[:16] if refl else "-"

def sdfmt(o, depth=0):
    s = o.name
    try:
        n = o.NumChildren()
    except Exception:
        n = 0
    if n == 0:
        try:
            v = getattr(o.data, "str", None) or None
            if v is None:
                bt = o.type.basetype
                import renderdoc as rd
                if bt == rd.SDBasic.Resource: v = "Res" + str(o.data.basic.id)
                elif bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.Enum): v = str(o.data.basic.u)
                elif bt == rd.SDBasic.SignedInteger: v = str(o.data.basic.i)
                elif bt == rd.SDBasic.Float: v = "%g" % o.data.basic.d
                elif bt == rd.SDBasic.Boolean: v = str(o.data.basic.b)
                elif bt == rd.SDBasic.Null: v = "NULL"
                else: v = "?"
            return "%s=%s" % (s, v)
        except Exception as e:
            return "%s=<%s>" % (s, e)
    if depth > 2: return "%s{%d}" % (s, n)
    return "%s{%s}" % (s, ", ".join(sdfmt(o.GetChild(i), depth + 1) for i in range(min(n, 12))))

def main():
    import renderdoc as rd
    od = os.environ["EDFSR_OUT"]; os.makedirs(od, exist_ok=True)
    asmdir = os.path.join(od, "vs_asm"); os.makedirs(asmdir, exist_ok=True)
    depth_id = os.environ.get("EDFSR_DEPTH", "4488")
    easu = os.environ.get("EDFSR_EASU", "2f7d47465d50053c")
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctrl.GetStructuredFile()

    # ---- depth texture description
    for t in ctrl.GetTextures():
        if str(t.resourceId).endswith("::" + depth_id):
            P("DEPTH TEX", t.resourceId, t.width, t.height, t.format.Name(), "creationFlags=", str(t.creationFlags),
              "mips", t.mips, "msSamp", t.msSamp)
    for r in ctrl.GetResources():
        if str(r.resourceId).endswith("::" + depth_id):
            P("RESOURCE", r.name, "initChunks", list(r.initialisationChunks), "derived", [str(x) for x in r.derivedResources])
            for ci in list(r.initialisationChunks)[:3]:
                P("  init chunk", ci, sdfmt(sf.chunks[ci]))
            for d in r.derivedResources:
                for r2 in ctrl.GetResources():
                    if r2.resourceId == d:
                        for ci in list(r2.initialisationChunks)[:2]:
                            P("  derived", r2.name, sdfmt(sf.chunks[ci]))

    acts = []
    def walk(l):
        for a in l:
            if a.children: walk(a.children)
            else: acts.append(a)
    walk(ctrl.GetRootActions())

    vs_seen = {}
    rows = []
    for a in acts:
        is_draw = bool(a.flags & rd.ActionFlags.Drawcall)
        is_disp = bool(a.flags & rd.ActionFlags.Dispatch)
        dsv = str(a.depthOut)
        on_depth = dsv.endswith("::" + depth_id)
        if not (is_draw or is_disp):
            rows.append("e%d %s" % (a.eventId, a.customName or a.GetName(sf)))
            continue
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        d3 = ctrl.GetD3D11PipelineState()
        vrefl = ps.GetShaderReflection(rd.ShaderStage.Vertex)
        prefl = ps.GetShaderReflection(rd.ShaderStage.Pixel)
        crefl = ps.GetShaderReflection(rd.ShaderStage.Compute)
        vh, ph, ch = shash(vrefl), shash(prefl), shash(crefl)
        line = "e%d %s idx=%d inst=%d dsv=%s rt0=%s vs=%s ps=%s cs=%s" % (
            a.eventId, "DRAW" if is_draw else "DISP", a.numIndices, a.numInstances, dsv.replace("ResourceId::", ""),
            str(a.outputs[0]).replace("ResourceId::", ""), vh, ph, ch)
        if is_draw:
            try:
                om = d3.outputMerger; dss = om.depthStencilState
                ff = dss.frontFace
                line += " | Z en=%d wr=%d fn=%s ST en=%d ref=%d rmask=%x wmask=%x fn=%s pass=%s" % (
                    dss.depthEnable, dss.depthWrites, str(dss.depthFunction).split(".")[-1], dss.stencilEnable,
                    ff.reference, ff.compareMask, ff.writeMask, str(ff.function).split(".")[-1], str(ff.passOperation).split(".")[-1])
                bs = om.blendState.blends[0]
                line += " blend=%d" % bs.enabled
            except Exception as e:
                line += " dss_err=%s" % e
            try:
                vp = d3.rasterizer.viewports[0]
                line += " vp=(%g,%g %gx%g)" % (vp.x, vp.y, vp.width, vp.height)
            except Exception as e:
                line += " vp_err=%s" % e
            if vrefl:
                cbs = []
                for i, cb in enumerate(vrefl.constantBlocks):
                    try:
                        b = ps.GetConstantBlock(rd.ShaderStage.Vertex, i, 0); d = b.descriptor
                        cbs.append("b%d:%s@%d" % (cb.fixedBindNumber, str(d.resource).replace("ResourceId::", ""), d.byteOffset))
                    except Exception as e:
                        cbs.append("b%d:err" % cb.fixedBindNumber)
                line += " vscb=" + ",".join(cbs)
            if on_depth and vrefl and vh not in vs_seen:
                vs_seen[vh] = a.eventId
                dis = ctrl.DisassembleShader(ps.GetGraphicsPipelineObject(), vrefl, ctrl.GetDisassemblyTargets(True)[0])
                open(os.path.join(asmdir, "vs_%s_e%d.asm" % (vh, a.eventId)), "w").write(dis)
                # dump VS per-draw cbuffer (non-b1) contents
                cbtxt = []
                for i, cb in enumerate(vrefl.constantBlocks):
                    b = ps.GetConstantBlock(rd.ShaderStage.Vertex, i, 0); d = b.descriptor
                    if cb.fixedBindNumber == 1: continue
                    data = bytes(ctrl.GetBufferData(d.resource, d.byteOffset, min(cb.byteSize, 512)))
                    fl = struct.unpack("<%df" % (len(data) // 4), data)
                    cbtxt.append("cb b%d %s size=%d" % (cb.fixedBindNumber, cb.name, cb.byteSize))
                    for v in cb.variables:
                        cbtxt.append("   var %s off=%d" % (v.name, v.byteOffset))
                    for k in range(0, len(fl), 4):
                        cbtxt.append("  [%3d] %s" % (k * 4, " ".join("%12.6g" % x for x in fl[k:k + 4])))
                open(os.path.join(asmdir, "vs_%s_e%d.cb.txt" % (vh, a.eventId)), "w").write("\n".join(cbtxt))
        rows.append(line)
        if is_disp and ch == easu:
            P("\n== EASU dispatch e%d, events in action:" % a.eventId)
            for ev in a.events:
                P("  ev %d chunk %d: %s" % (ev.eventId, ev.chunkIndex, sdfmt(sf.chunks[ev.chunkIndex])))
    open(os.path.join(od, "actions.txt"), "w").write("\n".join(rows))
    P("unique VS on depth", depth_id, ":", len(vs_seen), vs_seen)
    ctrl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    P(traceback.format_exc())
open(os.path.join(os.environ["EDFSR_OUT"], "report.txt"), "w").write("\n".join(out))
os._exit(0)
