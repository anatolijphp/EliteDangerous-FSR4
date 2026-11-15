# ED-FSR: frame map analyzer for RenderDoc captures.
# Run:  D:\tools\renderdoc\qrenderdoc.exe --py D:\ed-fsr\tools\analyze_capture.py
# Env:  EDFSR_CAPTURE = path to .rdc ; EDFSR_OUT = output dir (default: <capture>_analysis)
import os, sys, json, hashlib, traceback

LOG = []
def log(*a):
    s = " ".join(str(x) for x in a)
    LOG.append(s)

def main():
    import renderdoc as rd
    cap_path = os.environ["EDFSR_CAPTURE"]
    out_dir = os.environ.get("EDFSR_OUT") or os.path.splitext(cap_path)[0] + "_analysis"
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(os.path.join(out_dir, "img"), exist_ok=True)

    cap = rd.OpenCaptureFile()
    st = cap.OpenFile(cap_path, "", None)
    if st != rd.ResultCode.Succeeded:
        raise RuntimeError("OpenFile: %s" % st)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if st != rd.ResultCode.Succeeded:
        raise RuntimeError("OpenCapture: %s" % st)

    names = {str(r.resourceId): r.name for r in ctrl.GetResources()}
    textures = {}
    for t in ctrl.GetTextures():
        textures[str(t.resourceId)] = dict(
            id=str(t.resourceId), name=names.get(str(t.resourceId), ""),
            w=t.width, h=t.height, d=t.depth, mips=t.mips, arr=t.arraysize,
            fmt=t.format.Name(), flags=str(t.creationFlags), ms=t.msSamp)
    tex_obj = {str(t.resourceId): t for t in ctrl.GetTextures()}

    # Flatten actions
    actions = []
    def walk(lst, path):
        for a in lst:
            name = a.GetName(ctrl.GetStructuredFile())
            p = path + [name] if a.children else path
            if a.children:
                walk(a.children, p)
            else:
                actions.append(dict(eid=a.eventId, name=name, flags=str(a.flags),
                                    marker="/".join(path),
                                    dispatch=list(a.dispatchDimension),
                                    outputs=[str(o) for o in a.outputs if o != rd.ResourceId.Null()],
                                    depth=str(a.depthOut) if a.depthOut != rd.ResourceId.Null() else None,
                                    numIndices=a.numIndices, numInstances=a.numInstances))
    walk(ctrl.GetRootActions(), [])
    by_eid = {a["eid"]: a for a in actions}

    # Resource usage per event (reads/writes) for all textures
    usage = {}  # eid -> list of (texid, usage)
    for tid in textures:
        try:
            for u in ctrl.GetUsage(tex_obj[tid].resourceId):
                usage.setdefault(u.eventId, []).append((tid, str(u.usage).split(".")[-1]))
        except Exception as e:
            log("usage fail", tid, e)
    for a in actions:
        a["usage"] = usage.get(a["eid"], [])

    # Shader hashes for dispatches and draws
    shader_cache = {}
    def shash(refl):
        if refl is None:
            return None
        key = str(refl.resourceId)
        if key not in shader_cache:
            h = hashlib.sha1(bytes(refl.rawBytes)).hexdigest()[:16]
            cbs = []
            for cb in refl.constantBlocks:
                cbs.append(dict(name=cb.name, size=cb.byteSize,
                                vars=[v.name for v in cb.variables][:64]))
            shader_cache[key] = dict(hash=h, entry=refl.entryPoint, cbs=cbs,
                                     ro=[r.name for r in refl.readOnlyResources],
                                     rw=[r.name for r in refl.readWriteResources])
        return shader_cache[key]

    for a in actions:
        if not any(f in a["flags"] for f in ("Dispatch", "Drawcall")):
            continue
        ctrl.SetFrameEvent(a["eid"], False)
        ps = ctrl.GetPipelineState()
        sh = {}
        stages = [("cs", rd.ShaderStage.Compute)] if "Dispatch" in a["flags"] else \
                 [("vs", rd.ShaderStage.Vertex), ("ps", rd.ShaderStage.Pixel)]
        for nm, stg in stages:
            info = shash(ps.GetShaderReflection(stg))
            if info:
                sh[nm] = info["hash"]
        a["shaders"] = sh
        try:
            vp = ps.GetViewport(0)
            a["viewport"] = [vp.width, vp.height]
        except Exception:
            pass

    # Backbuffer guess: largest RGBA8/10 texture with SwapBuffer flag
    bb = [t for t in textures.values() if "SwapBuffer" in t["flags"]]
    out_w = max([t["w"] for t in bb], default=0)
    out_h = max([t["h"] for t in bb], default=0)

    # Upscale candidates: event reads a 2D tex smaller than it writes, write ~= output res
    cands = []
    for a in actions:
        reads = [textures[t] for t, u in a["usage"] if "Resource" in u and "RW" not in u and textures[t]["d"] == 1]
        writes = [textures[t] for t, u in a["usage"] if ("RWResource" in u or "ColorTarget" in u)]
        for w in writes:
            for r in reads:
                if r["w"] < w["w"] and r["h"] < w["h"] and w["w"] >= 0.9 * out_w and r["w"] >= 0.3 * w["w"] \
                        and abs(r["w"] / r["h"] - w["w"] / w["h"]) < 0.05:
                    cands.append(dict(eid=a["eid"], name=a["name"], marker=a["marker"],
                                      src=r["id"], src_size=[r["w"], r["h"]], src_fmt=r["fmt"],
                                      dst=w["id"], dst_size=[w["w"], w["h"]], dst_fmt=w["fmt"],
                                      shaders=a.get("shaders")))

    # Depth targets
    depth_use = {}
    for a in actions:
        if a["depth"]:
            d = depth_use.setdefault(a["depth"], dict(first=a["eid"], last=a["eid"], draws=0))
            d["last"] = a["eid"]; d["draws"] += 1
    depths = sorted(({**textures.get(k, {"id": k}), **v} for k, v in depth_use.items()),
                    key=lambda x: -x["draws"])

    # Save images for upscale candidates and main depth
    def save(tid, eid, tag):
        try:
            ctrl.SetFrameEvent(eid, True)
            ts = rd.TextureSave()
            ts.resourceId = tex_obj[tid].resourceId
            ts.destType = rd.FileType.PNG
            ts.alpha = rd.AlphaMapping.Discard
            ts.mip = 0
            ts.slice.sliceIndex = 0
            if "D" in textures[tid]["fmt"][:2] or "R32_TYPELESS" in textures[tid]["fmt"]:
                ts.comp.blackPoint = 0.0; ts.comp.whitePoint = 1.0
            fn = os.path.join(out_dir, "img", "%s_e%d_%s.png" % (tag, eid, tid.replace("::", "_").replace(" ", "")))
            ctrl.SaveTexture(ts, fn)
            return fn
        except Exception as e:
            log("save fail", tid, eid, e)
    seen = set()
    for c in cands[:12]:
        for tid, tag in ((c["src"], "upsrc"), (c["dst"], "updst")):
            if (tid, c["eid"]) not in seen:
                seen.add((tid, c["eid"]))
                save(tid, c["eid"], tag)
    for d in depths[:3]:
        if d.get("id") in textures:
            save(d["id"], d["last"], "depth")

    # Pass summary: consecutive actions grouped by render-target size
    passes = []
    for a in actions:
        size = None
        for t, u in a["usage"]:
            if u in ("ColorTarget", "CS_RWResource", "DepthStencilTarget"):
                size = (textures[t]["w"], textures[t]["h"]); break
        key = (a["marker"], size)
        if passes and passes[-1]["key"] == key:
            passes[-1]["n"] += 1; passes[-1]["last"] = a["eid"]
        else:
            passes.append(dict(key=key, first=a["eid"], last=a["eid"], n=1))

    data = dict(capture=cap_path, output_res=[out_w, out_h], api=str(ctrl.GetAPIProperties().pipelineType),
                n_actions=len(actions), upscale_candidates=cands, depth_targets=depths[:10],
                textures=sorted(textures.values(), key=lambda t: -(t["w"] * t["h"]))[:80],
                shaders=shader_cache, actions=actions, log=LOG)
    with open(os.path.join(out_dir, "frame.json"), "w") as f:
        json.dump(data, f, indent=1, default=str)
    with open(os.path.join(out_dir, "summary.txt"), "w") as f:
        f.write("capture %s\napi %s  output %dx%d  actions %d\n\n" % (cap_path, data["api"], out_w, out_h, len(actions)))
        f.write("== UPSCALE CANDIDATES\n")
        for c in cands:
            f.write("e%-6d %-40s %s %s -> %s %s  sh=%s  [%s]\n" % (c["eid"], c["name"][:40], c["src_size"], c["src_fmt"],
                    c["dst_size"], c["dst_fmt"], c["shaders"], c["marker"][-60:]))
        f.write("\n== DEPTH TARGETS\n")
        for d in depths[:10]:
            f.write("%s %sx%s %s draws=%d e%d..e%d\n" % (d.get("id"), d.get("w"), d.get("h"), d.get("fmt"), d["draws"], d["first"], d["last"]))
        f.write("\n== PASSES (marker, target size)\n")
        for p in passes:
            f.write("e%-6d..e%-6d n=%-4d %s %s\n" % (p["first"], p["last"], p["n"], p["key"][1], p["key"][0][-90:]))
        f.write("\n== LOG\n" + "\n".join(LOG))
    ctrl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    out = os.environ.get("EDFSR_OUT") or (os.path.splitext(os.environ.get("EDFSR_CAPTURE", "x"))[0] + "_analysis")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "error.txt"), "w") as f:
        f.write(traceback.format_exc() + "\n" + "\n".join(LOG))
os._exit(0)
