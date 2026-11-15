# Dump the VS t33 structured buffer (stride 336, per-object transforms) bound at given draws, plus the
# per-instance object index stream (v0.x) so records actually used can be identified.
# Env: EDFSR_CAPTURE, EDFSR_OUT (dir), EDFSR_EIDS (comma list)
import os, traceback, struct
LOG = []
def main():
    import renderdoc as rd
    od = os.environ["EDFSR_OUT"]; os.makedirs(od, exist_ok=True)
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    bufs = {str(b.resourceId): b for b in ctrl.GetBuffers()}
    saved = set()
    for e in [int(x) for x in os.environ["EDFSR_EIDS"].split(",")]:
        ctrl.SetFrameEvent(e, True)
        try:
            a = ctrl.GetAction(e)
            LOG.append("e%d draw: inst=%d instOffset=%d" % (e, a.numInstances, a.instanceOffset))
        except Exception as ex:
            LOG.append("e%d draw info err %s" % (e, ex))
        ps = ctrl.GetPipelineState()
        refl = ps.GetShaderReflection(rd.ShaderStage.Vertex)
        slots = {i: r.fixedBindNumber for i, r in enumerate(refl.readOnlyResources)}
        for s in ps.GetReadOnlyResources(rd.ShaderStage.Vertex):
            if slots.get(s.access.index) == 33 and s.descriptor.resource != rd.ResourceId.Null():
                rid = s.descriptor.resource; b = bufs.get(str(rid))
                LOG.append("e%d t33=%s size=%s firstEl=%s numEl=%s" % (e, rid, b.length if b else "?",
                           s.descriptor.firstElement if hasattr(s.descriptor, "firstElement") else "?",
                           s.descriptor.numElements if hasattr(s.descriptor, "numElements") else "?"))
                fn = os.path.join(od, "t33_e%d.bin" % e)
                open(fn, "wb").write(bytes(ctrl.GetBufferData(rid, 0, 0)))
        # vertex inputs: find the stream that feeds v0 (per-instance object index)
        try:
            vi = ps.GetVertexInputs(); vbs = ps.GetVBuffers()
            a = ctrl.GetCurrentAction() if hasattr(ctrl, "GetCurrentAction") else None
            for i, attr in enumerate(vi):
                LOG.append("   attr%d %s fmt=%s vb=%d off=%d perInst=%s" % (i, attr.name, attr.format.Name(), attr.vertexBuffer,
                           attr.byteOffset, attr.perInstance))
            # post-VS input data for v0 via mesh data is complex; record VB 0..n info
            for j, vb in enumerate(vbs):
                if vb.resourceId != rd.ResourceId.Null() and j == 0:
                    open(os.path.join(od, "vb0_e%d.bin" % e), "wb").write(bytes(ctrl.GetBufferData(vb.resourceId, vb.byteOffset, 0))[:65536])
                if vb.resourceId != rd.ResourceId.Null():
                    LOG.append("   vb%d %s stride=%d off=%d" % (j, vb.resourceId, vb.byteStride, vb.byteOffset))
        except Exception as ex:
            LOG.append("   vi err %s" % ex)
    ctrl.Shutdown(); cap.Shutdown()
try:
    main()
except Exception:
    LOG.append(traceback.format_exc())
open(os.path.join(os.environ["EDFSR_OUT"], "t33_log.txt"), "w").write("\n".join(LOG) + "\n")
os._exit(0)
