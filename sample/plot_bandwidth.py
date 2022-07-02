import numpy as np
import fileinput
import matplotlib.pyplot as plt
import sys

import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--from_time', type=float, default=0)
parser.add_argument('--to_time', type=float, default=sys.maxsize)
args = parser.parse_args()
sys.argv = sys.argv[:1] 

all_lines=[]
for line in fileinput.input():
	try:
		current_line = [float(item) for item in line.split(',')]
	except ValueError:
		continue
	if current_line[0] > args.to_time:
		break
	elif current_line[0] < args.from_time:
		continue
	all_lines.append(current_line)
interval_len = all_lines[1][0] - all_lines[0][0]
all_as_arrays = [np.array(item) for item in zip(*all_lines)] 
x = all_as_arrays[0]
ys = all_as_arrays[1:]

print("default size:", plt.rcParamsDefault["figure.figsize"])
plt.figure(figsize=(6.4, 4.8*0.67))

labels = ['total', 'subflow 1', 'subflow 2']
for i, y in enumerate(ys):
	print("sum:", np.sum(y) / 1_000_000 * 8)
	y = y / interval_len / 1_000_000 * 8
	# plt.plot(x, y, alpha=0.67, label=labels[i])


# plt.annotate("fair queuing", xy=(82.05, 22), xytext=(82.05, 10),
#             arrowprops=dict(arrowstyle="->"), horizontalalignment='center')

# plt.annotate("fair queuing", xy=(83.5, 22), xytext=(83.5, 10),
#             arrowprops=dict(arrowstyle="->"), horizontalalignment='center')

# plt.xlabel("Time (s)")
# plt.ylabel("Throughput (Mbit/s)")
# plt.ylim(bottom=0)
# plt.legend(loc="lower right")
# plt.tight_layout()
# plt.savefig("plots/bw.pdf", bbox_inches = 'tight', pad_inches = 0)
# plt.show()