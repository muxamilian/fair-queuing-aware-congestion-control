import numpy as np
import fileinput
import matplotlib.pyplot as plt
import json
import os
import sys

def add_plot(input_source):
    all_lines=[]
    for line in input_source:
        if "latest_rtt" not in line:
            continue
        line = line[:-2]
        try:
            parsed = json.loads(line)
            t = float(parsed[0])
            latest_rtt = float(parsed[4]['latest_rtt'])
            if latest_rtt > 0:
                all_lines.append((t, latest_rtt))
        except Exception as e:
            print("line", line)
            raise e

    x, y = [np.array(item) for item in zip(*all_lines)] 
    x = x / 1_000_000
    y = y / 1000.
    print("mean rtt:", np.mean(y))
    if 'DONT_PLOT' not in os.environ:
        plt.plot(x, y)

if len(sys.argv) <= 1:
    input_source = fileinput.input()
    add_plot(input_source)
else: 
    with open(sys.argv[1]) as f:
        input_source_1 = f.readlines()
        add_plot(input_source_1)
    with open(sys.argv[2]) as f:
        input_source_2 = f.readlines()
        add_plot(input_source_2)

if 'DONT_PLOT' not in os.environ:
    plt.xlabel("Time (s)")
    plt.ylabel("Delay (ms)")
    plt.ylim(bottom=0)
    plt.show()
