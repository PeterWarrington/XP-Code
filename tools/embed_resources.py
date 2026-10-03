"""Embed the application icon, version information and manifest into a linked executable.

Tiny C Compiler cannot compile .rc files, and XP's UpdateResource API corrupts TCC-linked images,
so this script writes the resource section itself: it builds an IMAGE_RESOURCE_DIRECTORY tree,
appends it to the PE file as a new ".rsrc" section and points the resource data directory at it.
  RT_ICON / RT_GROUP_ICON  1  res\\icon.ico
  RT_VERSION              1  built from APP_VERSION in src\\xpcode.h
  RT_MANIFEST             1  Common Controls 6 (XP visual styles for dialogs and message boxes)

    python tools\\embed_resources.py build\\xpcode.exe
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RT_ICON, RT_GROUP_ICON, RT_VERSION, RT_MANIFEST = 3, 14, 16, 24
LANG_EN_US = 0x0409

MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="{v}.0" processorArchitecture="x86" name="XPCode.XPCode" type="win32"/>
  <description>XP Code</description>
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
                        processorArchitecture="x86" publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
</assembly>
"""


def app_version():
    with open(os.path.join(ROOT, "src", "xpcode.h")) as f:
        m = re.search(r'#define APP_VERSION "(\d+)\.(\d+)\.(\d+)"', f.read())
    return tuple(int(x) for x in m.groups())


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def node(key, value=b"", value_len=0, text=False, children=()):
    """One VS_VERSIONINFO-style block: header, key, value, then 32-bit aligned children."""
    data = pad4(struct.pack("<HHH", 0, value_len, 1 if text else 0) + (key + "\0").encode("utf-16-le"))
    data += value
    for c in children:
        data = pad4(data) + c
    return struct.pack("<H", len(data)) + data[2:]


def version_resource(ver):
    major, minor, patch = ver
    dotted = "%d.%d.%d" % ver
    fixed = struct.pack("<13I", 0xFEEF04BD, 0x00010000, (major << 16) | minor, patch << 16,
                        (major << 16) | minor, patch << 16, 0x3F, 0, 0x00040004, 1, 0, 0, 0)
    strings = [("CompanyName", "Peter Warrington (lilpete.me)"), ("FileDescription", "XP Code"),
               ("FileVersion", dotted), ("InternalName", "xpcode"),
               ("LegalCopyright", "Copyright (c) 2026 Peter Warrington (lilpete.me). MIT License."), ("OriginalFilename", "xpcode.exe"),
               ("ProductName", "XP Code"), ("ProductVersion", dotted)]
    table = node("040904B0", children=[
        node(k, (v + "\0").encode("utf-16-le"), len(v) + 1, text=True) for k, v in strings])
    return node("VS_VERSION_INFO", fixed, len(fixed), children=[
        node("StringFileInfo", text=True, children=[table]),
        node("VarFileInfo", children=[node("Translation", struct.pack("<HH", 0x0409, 0x04B0), 4)])])


def icon_resources(path):
    with open(path, "rb") as f:
        data = f.read()
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1:
        raise SystemExit("not an .ico file: " + path)
    images, group = [], struct.pack("<HHH", 0, 1, count)
    for i in range(count):
        w, h, colors, res, planes, bits, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        images.append((i + 1, data[offset:offset + size]))
        group += struct.pack("<BBBBHHIH", w, h, colors, res, planes, bits, size, i + 1)
    return images, group


def align(n, a):
    return (n + a - 1) // a * a


def resource_section(resources, base_rva):
    """resources: {type_id: {name_id: bytes}} -> raw .rsrc section bytes for virtual address base_rva."""
    types = sorted(resources)
    # layout: root dir, one dir per type, one dir per (type, name), then data entries, then data
    def dir_size(n):
        return 16 + 8 * n
    off = dir_size(len(types))
    type_off, name_off = {}, {}
    for t in types:
        type_off[t] = off
        off += dir_size(len(resources[t]))
    for t in types:
        for n in sorted(resources[t]):
            name_off[t, n] = off
            off += dir_size(1)
    entry_off = {}
    for t in types:
        for n in sorted(resources[t]):
            entry_off[t, n] = off
            off += 16
    data_off = {}
    for t in types:
        for n in sorted(resources[t]):
            off = align(off, 8)
            data_off[t, n] = off
            off += len(resources[t][n])
    out = bytearray(align(off, 8))

    def put_dir(at, entries):
        struct.pack_into("<IIHHHH", out, at, 0, 0, 0, 0, 0, len(entries))
        for i, (ident, target) in enumerate(entries):
            struct.pack_into("<II", out, at + 16 + 8 * i, ident, target)

    put_dir(0, [(t, 0x80000000 | type_off[t]) for t in types])
    for t in types:
        put_dir(type_off[t], [(n, 0x80000000 | name_off[t, n]) for n in sorted(resources[t])])
        for n in sorted(resources[t]):
            put_dir(name_off[t, n], [(LANG_EN_US, entry_off[t, n])])
            data = resources[t][n]
            struct.pack_into("<IIII", out, entry_off[t, n], base_rva + data_off[t, n], len(data), 0, 0)
            out[data_off[t, n]:data_off[t, n] + len(data)] = data
    return bytes(out)


def add_resource_section(exe, resources):
    with open(exe, "rb") as f:
        pe = bytearray(f.read())
    nt = struct.unpack_from("<I", pe, 0x3C)[0]
    if pe[nt:nt + 4] != b"PE\0\0":
        raise SystemExit("not a PE file: " + exe)
    fh = nt + 4
    nsect, opt_size = struct.unpack_from("<H", pe, fh + 2)[0], struct.unpack_from("<H", pe, fh + 16)[0]
    opt = fh + 20
    if struct.unpack_from("<H", pe, opt)[0] != 0x10B:
        raise SystemExit("only 32-bit PE images are supported")
    sect_align, file_align = struct.unpack_from("<II", pe, opt + 32)
    size_of_headers = struct.unpack_from("<I", pe, opt + 60)[0]
    sections = opt + opt_size
    names = [bytes(pe[sections + 40 * i:sections + 40 * i + 8]).rstrip(b"\0") for i in range(nsect)]
    if b".rsrc" in names:
        raise SystemExit("image already has a .rsrc section (link a fresh one)")
    new_hdr = sections + 40 * nsect
    if new_hdr + 40 > size_of_headers or any(pe[new_hdr:new_hdr + 40]):
        raise SystemExit("no room in the section table for .rsrc")
    last_va_end = 0
    for i in range(nsect):
        vsize, va, raw_size = struct.unpack_from("<III", pe, sections + 40 * i + 8)
        last_va_end = max(last_va_end, va + max(vsize, raw_size))
    va = align(last_va_end, sect_align)
    data = resource_section(resources, va)
    raw_ptr = align(len(pe), file_align)
    raw_size = align(len(data), file_align)
    pe += b"\0" * (raw_ptr - len(pe)) + data + b"\0" * (raw_size - len(data))
    struct.pack_into("<8sIIIIIIHHI", pe, new_hdr, b".rsrc", len(data), va, raw_size, raw_ptr, 0, 0, 0, 0, 0x40000040)
    struct.pack_into("<H", pe, fh + 2, nsect + 1)
    struct.pack_into("<I", pe, opt + 8, struct.unpack_from("<I", pe, opt + 8)[0] + raw_size)   # SizeOfInitializedData
    struct.pack_into("<I", pe, opt + 56, align(va + len(data), sect_align))                    # SizeOfImage
    struct.pack_into("<II", pe, opt + 96 + 8 * 2, va, len(data))                               # resource directory
    with open(exe, "wb") as f:
        f.write(pe)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    exe = os.path.abspath(sys.argv[1])
    images, group = icon_resources(os.path.join(ROOT, "res", "icon.ico"))
    ver = app_version()
    resources = {
        RT_ICON: dict(images),
        RT_GROUP_ICON: {1: group},
        RT_VERSION: {1: version_resource(ver)},
        RT_MANIFEST: {1: MANIFEST.format(v="%d.%d.%d" % ver).encode("utf-8")},
    }
    add_resource_section(exe, resources)
    print("embedded icon (%d images), version %d.%d.%d and manifest into %s" % ((len(images),) + ver + (exe,)))


if __name__ == "__main__":
    main()
