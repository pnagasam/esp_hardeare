import numpy as np
import sys
import tqdm
import matplotlib.pyplot as plt
import matplotlib.animation as ani
import matplotlib.patches as mpatches
from scipy.signal import find_peaks
from pathlib import Path

# from acm_fig import *
from scipy.signal import butter, sosfiltfilt

# set_ieee()

data_dir = Path(".")

tx_mac = "1C:DB:D4:45:44:FC"

data = np.load(data_dir / "data.npz")
macs = data["mac"]

Hs = data["csi"][macs == tx_mac, :]
ts = data["time"][macs == tx_mac]

LOS_DIST = 0.25

min_vel = 0.01
f_center = 2.4e9
lam = 3e8 / f_center
fd_min = min_vel / lam

rssi_scale = -40


# Hs = np.vstack(Hs)
# ts = np.asarray(ts)


sample_med = np.median(np.diff(ts))

# Hs = normalize_csi_40(Hs, stitching="constant")
Hs_shift = np.zeros_like(Hs)

d_range = np.linspace(-30, 60, 1024)
ffreq = np.fft.fftfreq(128, d=1 / 40e6)
IDFT_MAT = np.exp((2.0j * np.pi * (d_range[:, None] / 3e8) * ffreq[None, :]))
sinc = np.abs(IDFT_MAT @ np.ones((128,)))
sinc /= np.max(sinc)
rise_thresh = 0.25

sinc_rise_comp = -d_range[np.argmax(np.abs(sinc) > rise_thresh)]
d0 = np.argmin(np.abs(d_range))
# win_len = int(1.0/sample_med)
win_len = 0.8
step_len = 0.2
env = np.hamming(win_len)


cir = (IDFT_MAT @ Hs.T).T


cir_shift = np.zeros_like(cir)


fs = 1.0 / sample_med  # sampling rate along axis 0
order = 8
sos = butter(order, fd_min, btype="high", fs=fs, output="sos")
cir_butter = sosfiltfilt(sos, cir_shift, axis=0)
# cir_filt = cir_shift - np.mean(cir_shift, axis=0)
cir_filt = cir_butter


# plt.figure()
# plt.plot(d_range,np.abs(cir_shift).T)
# plt.figure()
# plt.imshow(np.angle(cir_shift).T, cmap='twilight')


# ax.imshow((np.abs(spec)/np.max(np.abs(spec),axis=1)[:,None]).T,extent=[ts[0],ts[-1],d_range[0],d_range[-1]],origin='lower',aspect='auto')
def spec_abs(ax, spec):
    ax.imshow(
        (np.abs(spec)).T,
        extent=[ts[0], ts[-1], d_range[0], d_range[-1]],
        origin="lower",
        aspect="auto",
    )


def spec_ang(ax, spec):
    ax.imshow(
        np.angle(spec),
        cmap="twilight",
        extent=[ts[0], ts[-1], d_range[0], d_range[-1]],
        origin="lower",
        aspect="auto",
    )


# step_len = win_len // 4
# win_idcs = np.arange(0,cir_filt.shape[0] - win_len-step_len,step_len)
win_bins = np.arange(ts[0], ts[-1] - 0.01, step_len)
Nsteps = len(win_bins)
d_freq = np.linspace(-20, 20, 256)
nfreqs = len(d_freq)
img = np.zeros((Nsteps, cir_filt.shape[1]))
specs = np.zeros((Nsteps, nfreqs, cir_filt.shape[1]))
hwcs = []
hwds = []
for ii in tqdm.trange(Nsteps):
    # win = np.copy(cir_filt[win_idcs[ii]:win_idcs[ii]+win_len,:])
    # while id_win_e < len(ts) and ts[id_win_e] < win_bins[ii] + win_len:
    # id_win_e += 1
    in_idx = (ts >= win_bins[ii]) & (ts < win_bins[ii] + win_len)
    win = np.copy(cir_filt[in_idx, :])
    win_t = np.copy(ts[in_idx])
    if len(win_t) == 0:
        continue

    win_t -= win_t[0]
    dft_mat = np.exp(-2.0j * np.pi * d_freq[:, None] * win_t[None, :])

    img[ii, :] = np.linalg.norm(win, axis=0)
    # wf = np.fft.fft(win*env[:,None], axis=0,n=nfreqs)
    specs[ii, :, :] = np.abs(dft_mat @ win)
# vmax = 0.8*np.max(np.abs(specs))
vmax = 1.7
vmax_ts = 0.3

slice_dist = 10.0
slice_di = np.argmin(np.abs(d_range - slice_dist))


fig = plt.figure(figsize=(6, 10), layout="constrained")
ax = fig.add_subplot(3, 1, 1)
ax2 = fig.add_subplot(3, 1, 2)
ax3 = fig.add_subplot(3, 1, 3)


def animate(idx):
    print(f"{idx}/{Nsteps}")
    ax.clear()
    ax2.clear()

    ax3.clear()
    # ax5.clear()

    # c=cir[win_idcs[idx]:win_idcs[idx]+win_len,:]
    # cs=cir_shift[win_idcs[idx]:win_idcs[idx]+win_len,:]

    ax.imshow(
        specs[idx, :, :],
        extent=[d_range[0], d_range[-1], d_freq[0], d_freq[-1]],
        origin="lower",
        aspect="auto",
        vmin=0,
        vmax=vmax,
    )
    ax.axhline(0.0, color="orange")
    ax.axvline(slice_dist, color="green")

    # ax.set_ylim([-13.0,13.0])
    ax2.imshow(img.T, aspect="auto", origin="lower", vmin=0, vmax=vmax_ts)
    ax2.axvline(idx, color="r")

    ax3.imshow(
        specs[:, :, slice_di].T,
        origin="lower",
        aspect="auto",
        extent=[ts[0], ts[-1], d_freq[0], d_freq[-1]],
    )
    ax3.axvline(ts[idx], color="r")

    ax.set_xlabel("Range")
    ax.set_ylabel("Doppler")
    ax.set_title("Range-Doppler")

    ax2.set_xlabel("Time")
    ax2.set_ylabel("Range")
    ax2.set_title("Norm across window")

    ax3.set_xlabel("Time")
    ax3.set_ylabel("Doppler")
    ax3.set_title(f"Time-Doppler @ {slice_dist:.2f}m")

    # ax3.plot(np.angle(hwcs[idx]).T)
    # ax4.plot(np.angle(hwds[idx]).T)
    # cir_hwc = IDFT_MAT @ hwcs[idx].T
    # ax3.plot(np.abs(IDFT_MAT @ hwcs[idx].T))
    # ax4.plot(np.abs(IDFT_MAT @ hwds[idx].T))
    # ax5.plot(tw, np.abs(cir_hwc[d0,:]))
    # ax5.plot(cir_hwc[d0,:].real, cir_hwc[d0,:].imag,marker='.',linestyle='none')
    # ll = np.max(np.abs(cir_hwc[d0,:])*1.2)
    # ax5.set_xlim([-ll,ll])
    # ax5.set_ylim([-ll,ll])


# anim = ani.FuncAnimation(fig, animate, frames=range(00, Nsteps))
# anim.save("motion_uniform.mp4",fps=1/(step_len))
animate(10)


# plt.figure()
# spec_ang(plt.subplot(2,2,1), cir_filt)
# spec_abs(plt.subplot(2,2,2), cir_filt)

# spec_ang(plt.subplot(2,2,3), cir_butter)
# spec_abs(plt.subplot(2,2,4), cir_butter)

plt.figure()
plt.plot(np.sort(np.diff(ts)), np.linspace(0, 1, len(ts) - 1))
plt.xlim([0, np.median(np.diff(ts)) * 3])
plt.title("CSI Interval CDF")


plt.show()
