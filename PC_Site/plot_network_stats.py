import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

CSV_FILE    = "results.csv"
WINDOW_SIZE = 10000

df = pd.read_csv(CSV_FILE)

# ── Sliding-window datarate ──────────────────────────────────────────────────
# recv_time_us is the blocking time of recv() ≈ inter-packet interval.
# Datarate [MB/s] = sum(bytes in window) / sum(time in window [s])
bytes_arr = df["bytes"].to_numpy(dtype=float)
time_s    = df["recv_time_us"].to_numpy(dtype=float) / 1e6   # µs → s

kernel    = np.ones(WINDOW_SIZE)
win_bytes = np.convolve(bytes_arr, kernel, mode="valid")
win_time  = np.convolve(time_s,    kernel, mode="valid")
datarate  = win_bytes / win_time / 1e6   # bytes/s → MB/s
dr_steps  = df["step"].to_numpy()[WINDOW_SIZE - 1:]

# ── Plot ─────────────────────────────────────────────────────────────────────
fig, axes = plt.subplots(4, 1, figsize=(12, 11), sharex=False)
fig.suptitle("UDP Stream Statistics", fontsize=14)

# 1 – Bytes per packet
axes[0].plot(df["step"], df["bytes"], linewidth=0.7, color="steelblue")
axes[0].set_ylabel("Bytes")
axes[0].set_title("Bytes per packet")
axes[0].grid(True, alpha=0.4)

# 2 – Dropped packets over time
axes[1].plot(df["step"], df["dropped_packets"], linewidth=0.7, color="tomato")
axes[1].set_ylabel("Dropped packets")
axes[1].set_title("Dropped packets per step")
axes[1].grid(True, alpha=0.4)

# 3 – Histogram of dropped packets (exclude zeros)
drops_nonzero = df["dropped_packets"][df["dropped_packets"] > 0]
max_drop = int(drops_nonzero.max()) if len(drops_nonzero) > 0 else 1
bins = range(1, max_drop + 2)
axes[2].hist(drops_nonzero, bins=bins, color="orchid", edgecolor="white", align="left")
axes[2].set_ylabel("Count")
axes[2].set_xlabel("Dropped packets")
axes[2].set_title("Histogram of dropped packets")
axes[2].grid(True, alpha=0.4, axis="y")

# 4 – Sliding-window mean datarate
axes[3].plot(dr_steps, datarate, linewidth=0.7, color="seagreen")
axes[3].set_ylabel("MB/s")
axes[3].set_xlabel("Step")
axes[3].set_title(f"Mean datarate (sliding window = {WINDOW_SIZE} packets)")
axes[3].grid(True, alpha=0.4)

plt.tight_layout()
plt.savefig("network_stats.png", dpi=150)
plt.show()
print("Saved network_stats.png")
