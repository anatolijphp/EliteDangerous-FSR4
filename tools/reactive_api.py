# Dump API-call sequence (OMSetRenderTargets / PS/VS SetShader / Copy / Clear / CreateDepthStencilView /
# CreateBlendState / CreateDepthStencilState) with parameters, so we can see what the mod's hooks will observe.
# Env: EDFSR_CAPTURE, EDFSR_OUT, EDFSR_RANGE="first,last" eids (optional)
import os, traceback, json, hashlib
OUT = []
def P(*a): OUT.append(" ".join(str(x) for x in a))
def fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h ^= x; h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def sd(o, depth=0, maxd=4):
    # compact string of an SDObject
    try:
        n = o.NumChildren()
    except Exception:
        n = 0
    if n == 0 or depth >= maxd:
        try:
            bt = o.type.basetype
            import renderdoc as rd
            if bt == rd.SDBasic.Resource: return "R%s" % str(o.AsResourceId()).split("::")[-1]
            if bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.Enum): return str(o.AsInt()) if bt != rd.SDBasic.Enum else o.AsString() or str(o.AsInt())
            if bt == rd.SDBasic.SignedInteger: return str(o.AsInt())
            if bt == rd.SDBasic.Float: return "%g" % o.AsFloat()
            if bt == rd.SDBasic.Boolean: return str(o.AsBool())
            if bt == rd.SDBasic.String: return o.AsString()
            if bt == rd.SDBasic.Null: return "null"
            return o.AsString() or "?"
        except Exception as e:
            return "?"
    return "{" + ", ".join("%s=%s" % (o.GetChild(i).name, sd(o.GetChild(i), depth + 1, maxd)) for i in range(n)) + "}"

def main():
    import renderdoc as rd
    od = os.environ["EDFSR_OUT"]; os.makedirs(od, exist_ok=True)
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctrl.GetStructuredFile()
    rng = [int(x) for x in os.environ.get("EDFSR_RANGE", "0,99999999").split(",")]
    # shader resource id -> hash (from reflection of all shaders used in draws)
    shmap = {}
    def walk(lst):
        for a in lst:
            if a.children: walk(a.children)
            else: yield a
    acts = list(walk(ctrl.GetRootActions()))
    for a in acts:
        if a.flags & rd.ActionFlags.Drawcall and rng[0] <= a.eventId <= rng[1]:
            ctrl.SetFrameEvent(a.eventId, False)
            ps = ctrl.GetPipelineState()
            for stg in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
                r = ps.GetShaderReflection(stg)
                if r and str(r.resourceId) not in shmap:
                    b = bytes(r.rawBytes)
                    shmap[str(r.resourceId)] = "%s/0x%016x" % (hashlib.sha1(b).hexdigest()[:16], fnv(b))
    # view -> parent resource
    par = {}
    for r in ctrl.GetResources():
        if len(r.parentResources):
            par[str(r.resourceId).split("::")[-1]] = ",".join(str(p).split("::")[-1] for p in r.parentResources)
    INTEREST = ("OMSetRenderTargets", "PSSetShader", "VSSetShader", "Copy", "Clear", "OMSetBlendState",
                "OMSetDepthStencilState", "Draw", "Dispatch", "ResolveSubresource")
    for a in acts:
        for ev in a.events:
            if not (rng[0] <= ev.eventId <= rng[1]): continue
            ch = sf.chunks[ev.chunkIndex]; nm = ch.name.split("::")[-1]
            if not any(k in nm for k in INTEREST): continue
            s = sd(ch, 0, 3)
            if "SetShader" in nm:
                for k, v in shmap.items():
                    kk = "R" + k.split("::")[-1]
                    if kk in s: s += "  <" + v + ">"
            # annotate views with parent resources
            import re
            s = re.sub(r"R(\d+)", lambda m: "R%s%s" % (m.group(1), ("(" + par[m.group(1)] + ")") if m.group(1) in par else ""), s)
            P("e%d %s %s" % (ev.eventId, nm, s[:600]))
    # creation chunks for DSVs / blend / depth state (frame-capture initial chunks are not in events; list resources)
    for r in ctrl.GetResources():
        nm = r.name
        if any(k in str(r.type) for k in ("View", "StateObject")) or "DepthStencil" in nm:
            pass
    ctrl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    P(traceback.format_exc())
open(os.path.join(os.environ["EDFSR_OUT"], "api_seq.txt"), "w").write("\n".join(OUT) + "\n")
os._exit(0)
