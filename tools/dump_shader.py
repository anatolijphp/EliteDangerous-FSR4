# Dump shaders (bytecode, bindings, cbuffer data, disassembly) for given CS hashes. Run in qrenderdoc --py.
# Env: EDFSR_CAPTURE, EDFSR_OUT (dir), EDFSR_HASHES (comma list of sha1[:16] of rawBytes)
import os, hashlib, struct, traceback
out = []
def P(*a): out.append(" ".join(str(x) for x in a))

def main():
    import renderdoc as rd
    od = os.environ["EDFSR_OUT"]; os.makedirs(od, exist_ok=True)
    want = set(os.environ["EDFSR_HASHES"].split(","))
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    acts = []
    def walk(l):
        for a in l:
            if a.children: walk(a.children)
            elif a.flags & rd.ActionFlags.Dispatch: acts.append(a)
    walk(ctrl.GetRootActions())
    done = set()
    for a in acts:
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        refl = ps.GetShaderReflection(rd.ShaderStage.Compute)
        if not refl: continue
        raw = bytes(refl.rawBytes); h = hashlib.sha1(raw).hexdigest()[:16]
        if h not in want or h in done: continue
        done.add(h)
        open(os.path.join(od, h + ".dxbc"), "wb").write(raw)
        # FNV-1a 64 of bytecode (same as proxy)
        f = 0xcbf29ce484222325
        for b in raw: f = ((f ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
        P("==== CS %s  eid=%d dispatch=%s  bytes=%d fnv64=0x%016x" % (h, a.eventId, list(a.dispatchDimension), len(raw), f))
        P("threads", list(refl.dispatchThreadsDimension))
        for r in refl.readOnlyResources: P(" SRV", r.name, "t%d" % r.fixedBindNumber, r.textureType)
        for r in refl.readWriteResources: P(" UAV", r.name, "u%d" % r.fixedBindNumber, r.textureType)
        for s in refl.samplers: P(" SMP", s.name, "s%d" % s.fixedBindNumber)
        pipe = ps.GetComputePipelineObject()
        for i, cb in enumerate(refl.constantBlocks):
            P(" CB", cb.name, "b%d" % cb.fixedBindNumber, "size", cb.byteSize)
            try:
                b = ps.GetConstantBlock(rd.ShaderStage.Compute, i, 0)
                d = getattr(b, "descriptor", b)
                data = bytes(ctrl.GetBufferData(d.resource, d.byteOffset, cb.byteSize))
                u = struct.unpack("<%dI" % (len(data) // 4), data)
                fl = struct.unpack("<%df" % (len(data) // 4), data)
                for k in range(0, len(u), 4):
                    P("   [%3d] u=%s  f=%s" % (k * 4, " ".join("%08x" % x for x in u[k:k+4]), " ".join("%.6g" % x for x in fl[k:k+4])))
            except Exception as e:
                P("   cb read err", e)
        tgt = ctrl.GetDisassemblyTargets(True)[0]
        dis = ctrl.DisassembleShader(pipe, refl, tgt)
        open(os.path.join(od, h + ".asm"), "w").write(dis)
        P(" disasm ->", h + ".asm", "lines", dis.count("\n"))
    ctrl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    P(traceback.format_exc())
open(os.path.join(os.environ["EDFSR_OUT"], "shaders.txt"), "w").write("\n".join(out))
os._exit(0)
