import numpy as np
import fileinput
import matplotlib.pyplot as plt
import json
all_lines=[]
for line in fileinput.input():
    if "latest_rtt" not in line:
        continue
    line = line[:-2]
    try:
        parsed = json.loads(line)
        t = float(parsed[0])
        latest_rtt = float(parsed[4]['latest_rtt'])
        all_lines.append((t, latest_rtt))
    except Exception as e:
        print("line", line)
        raise e

x, y = [np.array(item) for item in zip(*all_lines)] 
x = x / 1_000_000
y = y / 1000.
plt.plot(x, y)
plt.show()
