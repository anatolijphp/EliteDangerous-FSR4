# Summarise reactive_scan.py output: HDR target writers in order, with state.
# Usage: python reactive_report.py <analysis_dir>/reactive
import json, sys, os
d = sys.argv[1]
J = json.load(open(os.path.join(d, "scan.json")))
rows = J["rows"]
TONEMAP = "2c77020ebfb2210b"
out = []
def P(*a): out.append(" ".join(str(x) for x in a))
P("HDR-format full-res textures:")
for k, v in J["hdr"].items():
    us = v["usage"]
    P(" ", v["desc"], v["flags"], "uses=%d" % len(us), "first", us[:1], "last", us[-1:])
tm = [r for r in rows if r.get("ps") and r["ps"]["sha"] == TONEMAP]
P("tonemap draws:", [(r["eid"], r["ps_srv"]) for r in tm])
for r in rows:
    if r.get("cs") and r["cs"]["sha"] == "2f7d47465d50053c":
        P("EASU fnv", r["cs"]["fnv"])
# HDR = the textures read by tonemap that are in J["hdr"]
hdrids = set()
for r in tm:
    for s in r["ps_srv"]:
        rid = s.split("=")[1]
        for k in J["hdr"]:
            if k.split("::")[-1] == rid: hdrids.add(k)
P("HDR ids read by tonemap:", hdrids)
P("")
P("eid  | VS sha/fnv | PS sha/fnv | rts | dsv ro | depth en/wr/fn | blend0 | srv | marker")
for r in rows:
    if not r["draw"]:
        if r["copy"] and any(o in hdrids for o in r["outputs"]): P("COPY/CLEAR", r["eid"], r["name"], r["outputs"])
        continue
    if not any(rt["res"] in hdrids for rt in r["rts"]):
        continue
    dp = r["depth"]
    P("%5d %s/%s %s/%s rts=%s dsv=%s ro=%d dep=%d/%d/%s st=%d bl=%s %s wm=%d cull=%s n=%d i=%d srv=%s | %s" % (
        r["eid"], r["vs"]["sha"], r["vs"]["fnv"], r["ps"]["sha"] if r["ps"] else None, r["ps"]["fnv"] if r["ps"] else None,
        [rt["desc"] for rt in r["rts"]], (r["dsv"] or "-").split("::")[-1], r["dsv_ro_depth"],
        dp["en"], dp["wr"], dp["fn"], dp["st"], int(r["blend0"]["en"]), r["blend0"]["c"], r["blend0"]["wm"], r["cull"],
        r["nidx"], r["ninst"], ",".join(r["ps_srv"]), r["marker"][-50:]))
open(os.path.join(d, "hdr_writers.txt"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
