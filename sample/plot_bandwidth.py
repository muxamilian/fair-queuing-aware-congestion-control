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

for i, y in enumerate(ys):
	print("sum:", np.sum(y) / 1_000_000 * 8)
	y = y / interval_len / 1_000_000 * 8
	plt.plot(x, y, alpha=0.67)

plt.xlabel("Time (s)")
plt.ylabel("Throughput (Mbit/s)")
plt.ylim(bottom=0)
plt.tight_layout()
plt.savefig("plots/bw.pdf", bbox_inches = 'tight', pad_inches = 0)
plt.show()