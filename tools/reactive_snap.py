# Dump raw HDR target bytes after given events (for per-group diffing offline).
# Env: EDFSR_CAPTURE, EDFSR_OUT (dir), EDFSR_EIDS (comma list), EDFSR_TEX (resource id number, e.g. 4493)
import os, traceback
LOG = []
def main():
    import renderdoc as rd
    od = os.environ["EDFSR_OUT"]; os.makedirs(od, exist_ok=True)
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["EDFSR_CAPTURE"], "", None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    want = os.environ["EDFSR_TEX"]
    t = [x for x in ctrl.GetTextures() if str(x.resourceId).split("::")[-1] == want][0]
    for e in [int(x) for x in os.environ["EDFSR_EIDS"].split(",")]:
        ctrl.SetFrameEvent(e, True)
        b = ctrl.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))
        open(os.path.join(od, "e%d.bin" % e), "wb").write(bytes(b))
    LOG.append("ok %dx%d %s" % (t.width, t.height, t.format.Name()))
    ctrl.Shutdown(); cap.Shutdown()
try:
    main()
except Exception:
    LOG.append(traceback.format_exc())
open(os.path.join(os.environ["EDFSR_OUT"], "snap_log.txt"), "w").write("\n".join(LOG) + "\n")
os._exit(0)
