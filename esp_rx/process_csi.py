import struct
import numpy as np
import matplotlib.pyplot as plt

# Must match csi_bin_hdr_t in main.c (packed, little-endian)
HDR_FMT  = '<2sHqbBBBBB6sH'
HDR_SIZE = struct.calcsize(HDR_FMT)   # 26 bytes
MAGIC    = b'\xaa\x55'

sig_mode_tgt = 1
cwb_tgt      = 1

times  = []
macs   = []
rssis  = []
csis   = []

n_accept = 0
n_reject = 0
n_bad    = 0

with open("csi.bin", "rb") as f:
    data = f.read()

pos = 0
while pos < len(data) - HDR_SIZE:
    idx = data.find(MAGIC, pos)
    if idx == -1:
        break
    pos = idx

    if pos + HDR_SIZE > len(data):
        break

    magic, seq, ts, rssi, rate, sig_mode, cwb, mcs, channel, mac_b, data_len = \
        struct.unpack_from(HDR_FMT, data, pos)

    pos += HDR_SIZE

    if data_len == 0 or data_len > 384 or pos + data_len > len(data):
        n_bad += 1
        pos -= (HDR_SIZE - 1)   # back up and keep scanning
        continue

    csi_raw = np.frombuffer(data[pos:pos + data_len], dtype=np.int8).copy()
    pos += data_len

    mac = ':'.join(f'{b:02X}' for b in mac_b)

    if sig_mode != sig_mode_tgt or cwb != cwb_tgt:
        n_reject += 1
        continue

    n_accept += 1
    csi_complex = csi_raw[1::2] + 1j * csi_raw[0::2]
    csi = csi_complex[-128:]
    times.append(ts)
    macs.append(mac)
    rssis.append(rssi)
    csis.append(csi)

print(f"Accept {n_accept}  Reject {n_reject}  Bad frames {n_bad}")

times  = np.asarray(times) * 1e-6
macs   = np.asarray(macs)
csis   = np.vstack(csis)
rssis  = np.asarray(rssis)

np.savez("data.npz", time=times, mac=macs, csi=csis, rssi=rssis)

dt = np.diff(times)
plt.figure()
plt.plot(np.sort(dt), np.linspace(0, 1, len(dt)))
plt.xlabel("inter-packet interval (s)")
plt.ylabel("CDF")
plt.title(f"N={n_accept} packets")
plt.show()
