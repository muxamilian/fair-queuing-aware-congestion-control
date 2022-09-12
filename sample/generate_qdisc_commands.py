import math

class Opts:
    pass

opt = Opts()
opt.delay = 100
opt.rate = 10
opt.buffer_size = 100
opt.qdisc = 'pfifo'
opt.interface = 'host10'

s = f"tc qdisc add dev {opt.interface} root handle 1: netem{f' delay {int(round(opt.delay/2))}ms'}", f"tc qdisc add dev {opt.interface} parent 1: handle 2: htb default 21", f"tc class add dev {opt.interface} parent 2: classid 2:21 htb rate {opt.rate}mbit", f"tc qdisc add dev {opt.interface} parent 2:21 handle 3: {opt.qdisc}{f' flow_limit {int(math.ceil(opt.buffer_size))}'}{f' limit {int(math.ceil(opt.buffer_size))}'}"

print(s)