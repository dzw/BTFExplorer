#!/usr/bin/env python3
"""Extract the main icon group (all resolutions) from a PE file into a .ico.

Uses Win32 APIs (LoadLibraryEx as datafile + FindResource). Avoids
EnumResourceNames callbacks (unreliable across CPython versions). Works for
both classic BMP icons and PNG-compressed icons because the raw RT_ICON bytes
are copied verbatim into the .ico file.
"""
import ctypes
import os
import sys

from ctypes import wintypes

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

HMODULE = wintypes.HMODULE
LPCWSTR = wintypes.LPCWSTR
DWORD = wintypes.DWORD

LOAD_LIBRARY_AS_DATAFILE = 0x00000002
RT_GROUP_ICON = LPCWSTR(14)
RT_ICON = LPCWSTR(3)

kernel32.LoadLibraryExW.argtypes = [LPCWSTR, wintypes.HANDLE, DWORD]
kernel32.LoadLibraryExW.restype = HMODULE
kernel32.FindResourceW.argtypes = [HMODULE, LPCWSTR, LPCWSTR]
kernel32.FindResourceW.restype = wintypes.HANDLE
kernel32.LoadResource.argtypes = [HMODULE, wintypes.HANDLE]
kernel32.LoadResource.restype = wintypes.HGLOBAL
kernel32.LockResource.argtypes = [wintypes.HGLOBAL]
kernel32.LockResource.restype = wintypes.LPVOID
kernel32.SizeofResource.argtypes = [HMODULE, wintypes.HANDLE]
kernel32.SizeofResource.restype = DWORD


def find_res(hmod, res_type, res_id):
    """Return (ptr, size) of a resource, or None if it does not exist."""
    res = kernel32.FindResourceW(hmod, LPCWSTR(res_id), res_type)
    if not res:
        return None
    hglobal = kernel32.LoadResource(hmod, res)
    if not hglobal:
        return None
    ptr = kernel32.LockResource(hglobal)
    size = kernel32.SizeofResource(hmod, res)
    return ptr, size


def main():
    if len(sys.argv) >= 3:
        exe_path = sys.argv[1]
        out_path = sys.argv[2]
    else:
        exe_path = r"D:\ndisk\Soft\Q-Dir\Q-Dir_x64.exe"
        out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "src", "Resources", "app.ico")

    hmod = kernel32.LoadLibraryExW(exe_path, 0, LOAD_LIBRARY_AS_DATAFILE)
    if not hmod:
        raise SystemExit("LoadLibraryExW failed: %d" % ctypes.get_last_error())

    # Locate the first existing RT_GROUP_ICON id (usually 1).
    group_id = None
    for cand in range(1, 256):
        if find_res(hmod, RT_GROUP_ICON, cand) is not None:
            group_id = cand
            break
    if group_id is None:
        raise SystemExit("No RT_GROUP_ICON found in %s" % exe_path)

    grp_ptr, grp_size = find_res(hmod, RT_GROUP_ICON, group_id)
    grp_bytes = ctypes.string_at(grp_ptr, grp_size)

    count = int.from_bytes(grp_bytes[4:6], "little")
    entries = []
    for i in range(count):
        base = 6 + i * 14
        width = grp_bytes[base]
        height = grp_bytes[base + 1]
        color_count = grp_bytes[base + 2]
        reserved = grp_bytes[base + 3]
        planes = int.from_bytes(grp_bytes[base + 4:base + 6], "little")
        bit_count = int.from_bytes(grp_bytes[base + 6:base + 8], "little")
        bytes_in_res = int.from_bytes(grp_bytes[base + 8:base + 12], "little")
        n_id = int.from_bytes(grp_bytes[base + 12:base + 14], "little")
        entries.append((width, height, color_count, reserved,
                        planes, bit_count, bytes_in_res, n_id))

    images = []
    for e in entries:
        res = find_res(hmod, RT_ICON, e[7])
        if res is None:
            continue
        images.append((e, ctypes.string_at(res[0], res[1])))

    if not images:
        raise SystemExit("No RT_ICON images found under group %d" % group_id)

    out_dir = os.path.dirname(os.path.abspath(out_path))
    os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(b"\x00\x00")              # reserved
        f.write(b"\x01\x00")              # type = icon
        f.write(len(images).to_bytes(2, "little"))
        offset = 6 + len(images) * 16
        for e, data in images:
            f.write(bytes([e[0], e[1], e[2], e[3]]))   # w/h/color/reserved
            f.write(e[4].to_bytes(2, "little"))        # planes
            f.write(e[5].to_bytes(2, "little"))        # bit count
            f.write(len(data).to_bytes(4, "little"))   # bytes in res
            f.write(offset.to_bytes(4, "little"))      # image offset
            offset += len(data)
        for _, data in images:
            f.write(data)

    print("Wrote %d image(s) to %s" % (len(images), os.path.abspath(out_path)))


if __name__ == "__main__":
    main()
