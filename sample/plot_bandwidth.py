import numpy as np
import fileinput
import matplotlib.pyplot as plt
all_lines=[]
for line in fileinput.input():
	all_lines.append([float(item) for item in line.split(',')])
interval_len = all_lines[1][0] - all_lines[0][0]
x, y = [np.array(item) for item in zip(*all_lines)] 
y = y / interval_len / 1_000_000 * 8
plt.plot(x, y)
plt.show()
