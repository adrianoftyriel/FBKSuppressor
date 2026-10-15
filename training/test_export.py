"""Stdlib-only check of the exporter's byte layout (no torch needed).

    python training/test_export.py
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import export_fbkm as ex


class FakeTensor:
    def __init__(self, shape, start):
        self.shape = shape
        n = 1
        for d in shape:
            n *= d
        self._v = [start + i for i in range(n)]

    def detach(self): return self
    def cpu(self): return self
    def flatten(self): return self
    def tolist(self): return self._v


def main():
    sd = {
        "in_.weight": FakeTensor((4, 64), 0), "in_.bias": FakeTensor((4,), 1),
        "core.weight_ih_l0": FakeTensor((12, 4), 2), "core.bias_hh_l0": FakeTensor((12,), 3),
        "smoother_out.weight": FakeTensor((65, 2), 4),
    }
    blob = ex.encode(sd)
    magic, version, count = struct.unpack_from("<III", blob, 0)
    assert (magic, version, count) == (0x4D4B4246, 1, 5), (magic, version, count)

    pos, names = 12, []
    for _ in range(count):
        (n,) = struct.unpack_from("<I", blob, pos); pos += 4
        names.append(blob[pos:pos + n].decode()); pos += n
        (nd,) = struct.unpack_from("<I", blob, pos); pos += 4
        dims = struct.unpack_from(f"<{nd}I", blob, pos); pos += 4 * nd
        count_f = 1
        for d in dims:
            count_f *= d
        pos += 4 * count_f
    assert pos == len(blob), "trailing or missing bytes"
    assert names == ["in.weight", "in.bias", "core.weight_ih", "core.bias_hh", "smoother.out.weight"], names

    try:
        ex.encode({"mystery.weight": FakeTensor((1,), 0)})
    except KeyError:
        pass
    else:
        raise AssertionError("unknown parameter must be rejected")
    print("export layout OK")


main()
