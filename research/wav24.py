import struct, numpy as np
def read_wav(path):
    """PCM 16/24/32-bit or float32 WAV -> (fs, float64 array [n, ch])"""
    d = open(path, "rb").read()
    assert d[:4] == b"RIFF" and d[8:12] == b"WAVE"
    i, fmt = 12, None
    while i < len(d) - 8:
        cid, sz = d[i:i + 4], struct.unpack("<I", d[i + 4:i + 8])[0]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", d[i + 8:i + 24])
        if cid == b"data":
            raw = d[i + 8:i + 8 + sz]
            break
        i += 8 + sz + (sz & 1)
    tag, ch, fs, _, _, bits = fmt
    if tag == 3:
        x = np.frombuffer(raw, "<f4").astype(np.float64)
    elif bits == 24:
        b = np.frombuffer(raw[:len(raw) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v / float(1 << 23)
    else:
        x = np.frombuffer(raw, {16: "<i2", 32: "<i4"}[bits]).astype(np.float64) / float(1 << (bits - 1))
    return fs, x.reshape(-1, ch)
