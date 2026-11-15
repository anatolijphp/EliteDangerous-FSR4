import sys; sys.path.insert(0, r"D:\tools\pylibs")
from minidump.minidumpfile import MinidumpFile
mf = MinidumpFile.parse(sys.argv[1])
mods = sorted(mf.modules.modules, key=lambda m: m.baseaddress)
def where(a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.size:
            return "%s+0x%x" % (m.name.split("\\")[-1], a - m.baseaddress)
    return "?"
if mf.exception:
    for e in mf.exception.exception_records:
        r = e.ExceptionRecord
        print("code", r.ExceptionCode, "addr", hex(r.ExceptionAddress), where(r.ExceptionAddress), "info", [hex(x) for x in r.ExceptionInformation[:2]])
        tid = e.ThreadId
        print("thread", tid)
# scan crashing thread stack for return addresses in known modules
rd = mf.get_reader()
th = [t for t in mf.threads.threads if t.ThreadId == tid][0]
sp = th.Stack.StartOfMemoryRange; size = th.Stack.MemoryLocation.DataSize
data = rd.read(sp, size)
import struct
hits = []
for i in range(0, size, 8):
    v = struct.unpack_from("<Q", data, i)[0]
    w = where(v)
    if w != "?" and not w.startswith(("ntdll", "KERNELBASE")):
        hits.append(w)
print("stack:", "\n ".join(hits[:400]))
print("modules:", ", ".join(m.name.split("\\")[-1] for m in mods if not m.name.lower().startswith(r"c:\windows")))
