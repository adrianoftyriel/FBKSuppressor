"""Export an FbkNet state dict to the FBKM file NeuralSeparator::load() reads.

Only the standard library is needed to write the file; torch is needed only to
produce the state dict. Usage:

    python export_fbkm.py checkpoint.pt model.fbkm

Format (little-endian):
    u32 magic 'FBKM' (0x4D4B4246), u32 version (1), u32 tensorCount
    per tensor: u32 nameLen, name, u32 ndim, u32 dims[ndim], f32 data[]
"""
import struct
import sys

MAGIC = 0x4D4B4246
VERSION = 1

# PyTorch state-dict key -> FBKM tensor name.
_RENAMES = {
    "in_": "in",
    "film_trunk": "film.trunk",
    "film_gamma": "film.gamma",
    "film_beta": "film.beta",
    "core": "core",
    "head": "head",
    "smoother": "smoother",
    "smoother_out": "smoother.out",
}


def fbkm_name(key: str) -> str:
    module, _, param = key.rpartition(".")
    param = param.removesuffix("_l0")          # nn.GRU names its params *_l0
    return f"{_RENAMES[module]}.{param}"


def _flat(t):
    """(shape, flat list of floats) from a torch tensor or anything shaped like one."""
    return tuple(t.shape), [float(v) for v in t.detach().cpu().flatten().tolist()]


def encode(state_dict) -> bytes:
    items = []
    for key, tensor in state_dict.items():
        if key.rpartition(".")[0] not in _RENAMES:
            raise KeyError(f"unexpected parameter {key!r}; is this an FbkNet checkpoint?")
        items.append((fbkm_name(key), *_flat(tensor)))

    out = bytearray(struct.pack("<III", MAGIC, VERSION, len(items)))
    for name, shape, data in items:
        raw = name.encode("ascii")
        out += struct.pack("<I", len(raw)) + raw
        out += struct.pack("<I", len(shape)) + struct.pack(f"<{len(shape)}I", *shape)
        out += struct.pack(f"<{len(data)}f", *data)
    return bytes(out)


def main(argv) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    import torch                                # only needed here
    state = torch.load(argv[1], map_location="cpu")
    state = state.get("model", state) if isinstance(state, dict) else state
    blob = encode(state)
    with open(argv[2], "wb") as f:
        f.write(blob)
    print(f"wrote {argv[2]}: {len(blob)} bytes, {len(state)} tensors")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
