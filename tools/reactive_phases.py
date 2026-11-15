# Run-length grouped view of HDR (tonemap input) writers + all non-RT usages of the HDR texture.
# Usage: python reactive_phases.py <analysis_dir>/reactive
import json, sys, os
d = sys.argv[1]
J = json.load(open(os.path.join(d, "scan.json")))
rows = J["rows"]; by = {r["eid"]: r for r in rows}
TONEMAP = "2c77020ebfb2210b"
tm = [r for r in rows if r.get("ps") and r["ps"]["sha"] == TONEMAP][0]
hid = [k for k in J["hdr"] if k.split("::")[-1] in [s.split("=")[1] for s in tm["ps_srv"]]][0]
out = []
def P(*a): out.append(" ".join(str(x) for x in a))
P("HDR", J["hdr"][hid]["desc"], "tonemap eid", tm["eid"])
P("== non-ColorTarget usage of HDR:")
for e, u in J["hdr"][hid]["usage"]:
    if u != "ColorTarget":
        r = by.get(e, {})
        P("  e%d %s %s %s" % (e, u, r.get("name", "")[:60], (r.get("ps") or r.get("cs") or {}).get("sha", "")))
P("== HDR colour writers (grouped)")
def key(r):
    dp = r["depth"]
    return (r["vs"]["sha"], (r["ps"] or {}).get("sha"), tuple(x["desc"].split()[0] for x in r["rts"]), r["dsv"],
            dp["en"], dp["wr"], dp["fn"], r["blend0"]["en"], r["blend0"]["c"], r["blend0"]["wm"])
grp = []
for r in rows:
    if not r["draw"] or not any(x["res"] == hid for x in r["rts"]): continue
    k = key(r)
    if grp and grp[-1]["k"] == k: grp[-1]["e"].append(r["eid"]); grp[-1]["n"] += r["nidx"] * max(1, r["ninst"])
    else: grp.append(dict(k=k, e=[r["eid"]], n=r["nidx"] * max(1, r["ninst"]), r=r))
for g in grp:
    k = g["k"]; r = g["r"]
    P("e%-5d..%-5d x%-3d VS %s %s | PS %s %s | rt=%s dsv=%s dep=%d/%d/%s bl=%d %s wm=%d idx*inst=%d" % (
        g["e"][0], g["e"][-1], len(g["e"]), k[0], r["vs"]["fnv"], k[1], (r["ps"] or {}).get("fnv"),
        "+".join(k[2]), (k[3] or "-").split("::")[-1], k[4], k[5], k[6], k[7], k[8], k[9], g["n"]))
open(os.path.join(d, "hdr_phases.txt"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
