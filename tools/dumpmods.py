import sys; sys.path.insert(0, r"D:\tools\pylibs")
from minidump.minidumpfile import MinidumpFile
for m in MinidumpFile.parse(sys.argv[1]).modules.modules:
    if "EOS" in m.name or "overlay" in m.name.lower(): print(m.name)
