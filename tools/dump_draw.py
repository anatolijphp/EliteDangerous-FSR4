# Dump VS/PS disassembly + bound SRVs for given draw event ids. Run in qrenderdoc --py.
# Env: EDFSR_CAPTURE, EDFSR_OUT (dir), EDFSR_EIDS (comma list)
import os, hashlib, traceback
out = []
def P(*a): out.append(" ".join(str(x) for x in a))

def main():
    import renderdoc as rd
    od = os.environ["EDFSR_OUT"]; os.makedirs(od, exist_ok=True)
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    tgt = ctrl.GetDisassemblyTargets(True)[0]
    for e in [int(x) for x in os.environ["EDFSR_EIDS"].split(",")]:
        ctrl.SetFrameEvent(e, True)
        ps = ctrl.GetPipelineState()
        for sname, stg in (("vs", rd.ShaderStage.Vertex), ("ps", rd.ShaderStage.Pixel)):
            refl = ps.GetShaderReflection(stg)
            if not refl: continue
            h = hashlib.sha1(bytes(refl.rawBytes)).hexdigest()[:16]
            dis = ctrl.DisassembleShader(ps.GetGraphicsPipelineObject(), refl, tgt)
            open(os.path.join(od, "e%d_%s_%s.asm" % (e, sname, h)), "w").write(dis)
            P("e%d %s %s inputs=%s" % (e, sname, h, [(s.semanticName, s.semanticIndex) for s in refl.inputSignature]))
            if sname == "ps":
                try:
                    for i, r in enumerate(refl.readOnlyResources):
                        P("   t%d %s" % (r.fixedBindNumber, r.name))
                    srvs = ps.GetReadOnlyResources(stg)
                    for s in srvs:
                        d = s.descriptor
                        if d.resource != rd.ResourceId.Null():
                            P("   bound slot %s -> %s" % (s.access.index, d.resource))
                except Exception as ex:
                    P("   srv err", ex)
    ctrl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    P(traceback.format_exc())
open(os.path.join(os.environ["EDFSR_OUT"], "draws.txt"), "a").write("\n".join(out) + "\n")
os._exit(0)
