#!/usr/bin/env python3
"""Check that a built dsetup.dll exports exactly the original names/ordinals."""
import struct, sys

EXPECTED = {
    1: "DirectXDeviceDriverSetupA", 2: "DirectXDeviceDriverSetupW",
    3: "DirectXRegisterApplicationA", 4: "DirectXRegisterApplicationW",
    5: "DirectXSetupA", 6: "DirectXSetupIsJapan", 7: "DirectXSetupIsJapanNec",
    8: "DirectXSetupW", 9: "DirectXUnRegisterApplication",
    10: "DirectXSetupSetCallback", 11: "DirectXSetupGetVersion",
    12: "DirectXSetupCallback", 13: "DirectXSetupGetFileVersion",
    14: "DirectXLoadString", 15: "DirectXSetupIsEng", 16: "DirectXSetupShowEULA",
    17: "DirectXSetupGetEULAA", 18: "DirectXSetupGetEULAW",
}

def exports(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3c)[0]
    assert d[pe:pe+4] == b"PE\0\0", "not a PE file"
    machine, nsec, _, _, _, optsz, _ = struct.unpack_from("<HHIIIHH", d, pe + 4)
    assert machine == 0x14c, "not a 32-bit x86 DLL"
    opt = pe + 24
    exp_rva, _ = struct.unpack_from("<II", d, opt + 96)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, so + i*40 + 8)
        secs.append((va, max(vsz, rsz), rptr))
    def off(rva):
        for va, sz, rp in secs:
            if va <= rva < va + sz:
                return rva - va + rp
        raise ValueError(hex(rva))
    e = off(exp_rva)
    base, nfun, nnames, _, pnames, pords = struct.unpack_from("<IIIIII", d, e + 16)
    out = {}
    for i in range(nnames):
        nrva = struct.unpack_from("<I", d, off(pnames) + 4*i)[0]
        o = off(nrva)
        name = d[o:d.index(b"\0", o)].decode()
        ordi = struct.unpack_from("<H", d, off(pords) + 2*i)[0] + base
        out[ordi] = name
    return out

if __name__ == "__main__":
    got = exports(sys.argv[1])
    if got != EXPECTED:
        for k in sorted(set(got) | set(EXPECTED)):
            if got.get(k) != EXPECTED.get(k):
                print(f"ordinal {k}: expected {EXPECTED.get(k)!r}, got {got.get(k)!r}")
        sys.exit(1)
    print(f"OK: {len(got)} exports with matching ordinals")
