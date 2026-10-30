import sys
import csv
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
from matplotlib.ticker import FuncFormatter

COLORS = ["tab:blue", "tab:orange", "tab:green", "tab:red"]
TRACK_Y = {"producer": 1.0, "consumer": 0.0}
BOX_HEIGHT = 0.65

COLUMNS = [
    "producer.yield_start", "event.producer.yield_end",
    "producer.resume_start", "event.producer.resume_end",
    "producer.work_start", "event.producer.work_end",
    "producer.notify_start", "event.producer.notify_end",
    "consumer.yield_start", "event.consumer.yield_end",
    "consumer.resume_start", "event.consumer.resume_end",
    "consumer.work_start", "event.consumer.work_end",
    "consumer.notify_start", "event.consumer.notify_end"
]

min_timestamp = ""
intervals = []
with open(sys.argv[1]) as f:
    reader = csv.reader(f)

    for row_number, row in enumerate(reader, start=1):
        if not row or all(not value.strip() for value in row):
            continue

        if len(row) != 16:
            raise ValueError(
                f"Row {row_number}: expected 16 columns, "
                f"got {len(row)}"
            )

        values = [int(value.strip()) for value in row]

        for track_index, track in enumerate(("producer", "consumer")):
            offset = track_index * 8

            for i in range(4):
                start = values[offset + 2 * i]
                end = values[offset + 2 * i + 1]

                if start == 0 or end == 0:
                    continue

                if end < start:
                    raise ValueError(
                        f"Invalid interval in row {row_number}, "
                        f"{track}, interval {i + 1}"
                    )

                intervals.append({
                    "track": track,
                    "number": i,
                    "start": start,
                    "end": end,
                    "row": row_number,
                })

if not intervals:
    raise ValueError("No intervals found in the CSV.")

# Subtract the origin using integer arithmetic first.
t0 = min(item["start"] for item in intervals)

for item in intervals:
    item["x"] = (item["start"] - t0)
    item["width"] = (
        item["end"] - item["start"]
    )

# Plot
fig, ax = plt.subplots(figsize=(14, 4))

for item in intervals:
    y = TRACK_Y[item["track"]]

    ax.add_patch(Rectangle(
        (item["x"], y),
        item["width"],
        BOX_HEIGHT,
        facecolor=COLORS[item["number"]],
        edgecolor="black",
        linewidth=0.6,
        alpha=0.85,
    ))

ax.set_yticks([
    TRACK_Y["producer"] + BOX_HEIGHT / 2,
    TRACK_Y["consumer"] + BOX_HEIGHT / 2,
])
ax.set_yticklabels(["Producer", "Consumer"])
ax.set_ylim(-0.2, 1.9)

xmin = min(item["x"] for item in intervals)
xmax = max(item["x"] + item["width"] for item in intervals)
margin = max((xmax - xmin) * 0.02, 1e-9)
ax.set_xlim(xmin - margin, xmax + margin)

ax.set_xlabel("Elapsed time (seconds)")
ax.xaxis.set_major_formatter(
    FuncFormatter(lambda x, _: f"{x:.6g}")
)
ax.grid(axis="x", linestyle="--", alpha=0.4)
ax.set_axisbelow(True)
ax.set_title("Timeline")

# Legend: colors identify interval positions within each track.
handles = [
    Rectangle((0, 0), 1, 1, facecolor=COLORS[i])
    for i in range(4)
]
ax.legend(handles, [f"Interval {i}" for i in range(1, 5)],
          loc="upper right")

plt.tight_layout()
plt.show()
