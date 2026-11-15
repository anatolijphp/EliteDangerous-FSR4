# ED-FSR stage 4: reactive-mask investigation. Dumps per-draw OM/blend/depth state, VS/PS hashes
# (sha1[:16] + FNV-1a 64 of raw bytecode) and HDR-target writers.
# Run:  set EDFSR_CAPTURE=...rdc ; EDFSR_OUT=<dir> ; qrenderdoc.exe --py reactive_scan.py
import os, json, hashlib, traceback
OUT = []
def P(*a): OUT.append(" ".join(str(x) for x in a))

def fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def main():
    import renderdoc as rd
    od = os.environ["EDFSR_OUT"]; os.makedirs(od, exist_ok=True)
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctrl.GetStructuredFile()
    tex = {str(t.resourceId): t for t in ctrl.GetTextures()}
    names = {str(r.resourceId): r.name for r in ctrl.GetResources()}
    acts = []
    def walk(lst, path):
        for a in lst:
            nm = a.GetName(sf)
            if a.children: walk(a.children, path + [nm])
            else: acts.append((a, "/".join(path), nm))
    walk(ctrl.GetRootActions(), [])

    shc = {}
    def sh(refl):
        if refl is None: return None
        k = str(refl.resourceId)
        if k not in shc:
            b = bytes(refl.rawBytes)
            shc[k] = dict(sha=hashlib.sha1(b).hexdigest()[:16], fnv="0x%016x" % fnv(b), size=len(b))
        return shc[k]

    def tdesc(rid):
        t = tex.get(str(rid))
        if t is None: return str(rid)
        return "%s %dx%d %s" % (str(rid).split("::")[-1], t.width, t.height, t.format.Name())

    rows = []
    for a, mk, nm in acts:
        f = a.flags
        isdraw = bool(f & rd.ActionFlags.Drawcall); isdisp = bool(f & rd.ActionFlags.Dispatch)
        row = dict(eid=a.eventId, name=nm, marker=mk, draw=isdraw, disp=isdisp,
                   outputs=[str(o) for o in a.outputs if o != rd.ResourceId.Null()],
                   copy=bool(f & (rd.ActionFlags.Copy | rd.ActionFlags.Resolve | rd.ActionFlags.Clear)),
                   nidx=a.numIndices, ninst=a.numInstances)
        if isdraw or isdisp:
            ctrl.SetFrameEvent(a.eventId, False)
            ps = ctrl.GetPipelineState()
            d = ctrl.GetD3D11PipelineState()
            if isdraw:
                row["vs"] = sh(ps.GetShaderReflection(rd.ShaderStage.Vertex))
                row["ps"] = sh(ps.GetShaderReflection(rd.ShaderStage.Pixel))
                om = d.outputMerger
                rts = []
                for r in om.renderTargets:
                    if r.resource != rd.ResourceId.Null():
                        rts.append(dict(res=str(r.resource), view=str(getattr(r, "view", "")),
                                        fmt=r.format.Name(), desc=tdesc(r.resource)))
                row["rts"] = rts
                dt = om.depthTarget
                row["dsv"] = str(dt.resource) if dt.resource != rd.ResourceId.Null() else None
                row["dsv_view"] = str(getattr(dt, "view", ""))
                row["dsv_ro_depth"] = bool(om.depthReadOnly); row["dsv_ro_stencil"] = bool(om.stencilReadOnly)
                ds = om.depthStencilState
                row["depth"] = dict(en=bool(ds.depthEnable), wr=bool(ds.depthWrites),
                                    fn=str(ds.depthFunction).split(".")[-1], st=bool(ds.stencilEnable))
                row["dss_id"] = str(ds.resourceId)
                bs = om.blendState
                row["bs_id"] = str(bs.resourceId)
                b0 = bs.blends[0]
                row["blend0"] = dict(en=bool(b0.enabled),
                    c="%s*%s %s" % (str(b0.colorBlend.source).split(".")[-1], str(b0.colorBlend.destination).split(".")[-1],
                                     str(b0.colorBlend.operation).split(".")[-1]),
                    a="%s*%s" % (str(b0.alphaBlend.source).split(".")[-1], str(b0.alphaBlend.destination).split(".")[-1]),
                    wm=int(b0.writeMask))
                row["a2c"] = bool(bs.alphaToCoverage)
                rs = d.rasterizer.state
                row["cull"] = str(rs.cullMode).split(".")[-1]
                vp = d.rasterizer.viewports[0]
                row["vp"] = [vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth]
                # PS SRVs
                srv = []
                try:
                    for s in ps.GetReadOnlyResources(rd.ShaderStage.Pixel):
                        if s.descriptor.resource != rd.ResourceId.Null():
                            srv.append("t%d=%s" % (s.access.index, str(s.descriptor.resource).split("::")[-1]))
                except Exception as e:
                    srv.append("err %s" % e)
                row["ps_srv"] = srv
            else:
                row["cs"] = sh(ps.GetShaderReflection(rd.ShaderStage.Compute))
        rows.append(row)

    # usage of all 1715x964 colour textures in HDR formats
    hdr = {}
    for k, t in tex.items():
        fn = t.format.Name()
        if ("R11G11B10" in fn or "R16G16B16A16" in fn) and t.width >= 1700 and t.height >= 960:
            us = []
            for u in ctrl.GetUsage(t.resourceId):
                us.append((u.eventId, str(u.usage).split(".")[-1]))
            hdr[k] = dict(desc=tdesc(t.resourceId), name=names.get(k, ""), usage=us,
                          flags=str(t.creationFlags))
    json.dump(dict(rows=rows, hdr=hdr, shaders=shc), open(os.path.join(od, "scan.json"), "w"), indent=1, default=str)
    ctrl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    P(traceback.format_exc())
open(os.path.join(os.environ["EDFSR_OUT"], "scan_log.txt"), "w").write("\n".join(OUT) + "\n")
os._exit(0)
