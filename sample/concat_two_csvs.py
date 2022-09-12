import numpy as np
import sys

file_1 = sys.argv[1]
file_2 = sys.argv[2]

def read_lines(f):
    all_lines = []
    for line in f.readlines():
        current_line = [float(item) for item in line.split(',')]
        all_lines.append(current_line)
    return all_lines

with open(file_1) as f:
    all_lines_1 = read_lines(f)

with open(file_2) as f:
    all_lines_2 = read_lines(f)

arr1 = np.array(all_lines_1, dtype=np.float64)
arr2 = np.array(all_lines_2, dtype=np.float64)
min_len = min(arr1.shape[0], arr2.shape[0])
arr1 = arr1[:min_len, :]
arr2 = arr2[:min_len, :]
print("arr1", arr1.shape, "arr2", arr2.shape)

new_arr = np.concatenate([arr1, arr2[:,-1:]], axis=-1)
np.savetxt("out.csv", new_arr, delimiter=",")
