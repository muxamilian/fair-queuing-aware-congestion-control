import mininet
import mininet.topo
import mininet.net
import mininet.node
import mininet.link
import mininet.util
import mininet.cli
import mininet.clean
import time
import os
import math
import subprocess

mininet.clean.cleanup()
subprocess.run('killall picoquic_sample'.split(' '))
subprocess.run('killall iperf3'.split(' '))

class MyTopo(mininet.topo.Topo):
    "Simple topology example."

    def __init__(self):
        "Create custom topo."

        # Initialize topology
        mininet.topo.Topo.__init__(self)

        # Add hosts and switches
        leftHost = self.addHost('h1')
        rightHost = self.addHost('h2')
        switch = self.addSwitch('s1')

        # Add links
        self.addLink(leftHost, switch)
        # self.addLink(leftHost, switch)
        self.addLink(switch, rightHost)

topo = MyTopo()
net = mininet.net.Mininet(topo=topo, link=mininet.link.TCLink, ipBase='192.168.0.0/24')
net.start()

time.sleep(2)

mininet.util.dumpNodeConnections(net.hosts)

net.pingAll()

h1: mininet.node.Host = net.get('h1')
h2: mininet.node.Host = net.get('h2')
s1: mininet.node.Host = net.get('s1')

os.chdir(os.path.dirname(__file__))

class Opts:
    pass

iperf = False
max_time = 10
congestion_control = "tonopah"

def generate_tc_commands(if_name, with_delay=False):
    opt = Opts()
    opt.delay = 50
    opt.rate = 20
    bdp = (opt.delay/1000 * opt.rate*1000000)/(1500*8)
    if with_delay:
        print("bdp", bdp)
    opt.buffer_size = None
    # opt.buffer_size = int(1. * math.ceil(bdp))
    opt.buffer_size = 10
    # opt.qdisc = 'fq'
    opt.qdisc = 'pfifo'
    opt.interface = if_name

    if with_delay:
        if opt.qdisc != 'fq':
            qdisc_string = f"{opt.qdisc}"
            if opt.buffer_size is not None: 
                 qdisc_string += f" limit {int(math.ceil(opt.buffer_size/2))}"
        else:
            qdisc_string = f"{opt.qdisc} nopacing"
            if opt.buffer_size is not None: 
                 qdisc_string += f" flow_limit {int(math.ceil(opt.buffer_size/2))}"
    else:
        qdisc_string = opt.qdisc

    strings = (
        f"tc qdisc del dev {opt.interface} root", 
        f"tc qdisc add dev {opt.interface} root handle 1: netem{f' delay {int(opt.delay/2) if with_delay else 0}ms'}", 
        f"tc qdisc add dev {opt.interface} parent 1: handle 2: htb default 21", 
        f"tc class add dev {opt.interface} parent 2: classid 2:21 htb rate {opt.rate if with_delay else 1000}mbit", 
        f"tc qdisc add dev {opt.interface} parent 2:21 handle 3: {qdisc_string}"
    )
    print("dev:", if_name, "with_delay:", with_delay, "commands:", strings)
    return strings

print([h1.cmd(item) for item in generate_tc_commands('h1-eth0', with_delay=True)])
# print([h1.cmd(item) for item in generate_tc_commands('h1-eth1')])
print([h2.cmd(item) for item in generate_tc_commands('h2-eth0')])
print([s1.cmd(item) for item in generate_tc_commands('s1-eth1', with_delay=True)])
print([s1.cmd(item) for item in generate_tc_commands('s1-eth2')])
# print([s1.cmd(item) for item in generate_tc_commands('s1-eth3')])
# print(h1.cmd("ip addr add 192.168.0.3/24 brd 192.168.0.255 dev h1-eth1"))

offloading_options = "gso off tso off gro off"
print(h1.cmd("ethtool -K h1-eth0 " + offloading_options))
print(h2.cmd("ethtool -K h2-eth0 " + offloading_options))
print(s1.cmd("ethtool -K s1-eth1 " + offloading_options))
print(s1.cmd("ethtool -K s1-eth2 " + offloading_options))

# mininet.cli.CLI(net)
# net.stop()
# quit()

debug = {"stdout": None, "stderr": None}
os.environ["MAX_TIME"] = str(max_time)
os.environ["CONGESTION_CONTROL"] = congestion_control

server_tcpdump_popen = h2.popen(f'tcpdump -s 100 -i h2-eth0 -w server.pcap (tcp || udp) and ip'.split(' '), **debug)
client_tcpdump_popen = h1.popen(f'tcpdump -s 100 -i h1-eth0 -w client.pcap (tcp || udp) and ip'.split(' '), **debug)

# server_popen = h2.popen('iperf3 -s -1'.split(' '), **debug)
# time.sleep(1)
# client_popen = h1.popen(f'iperf3 -c {h2.IP()}'.split(' '), **debug)
server_popen = h2.popen(f'../picoquic_sample server 4433 ./ca-cert.pem ./server-key.pem ./server_files'.split(' '), **debug)
if iperf:
    iperf_server_popen = h1.popen(f'iperf3 -s'.split(' '), **debug)
time.sleep(1)
if iperf:
    iperf_client_popen = h2.popen(f'iperf3 -c {h1.IP()} --congestion reno -tinf'.split(' '), **debug)
# client_popen = h1.popen(f'../picoquic_sample client {h2.IP()} 4433 ./ 100M.bin'.split(' '), env={'END_TIME': "10"}, **debug)
client_popen = h1.popen(f'../picoquic_sample client {h2.IP()} 4433 ./ 100M.bin'.split(' '), **debug)

client_popen.communicate()

if iperf:
    iperf_client_popen.terminate()
    iperf_server_popen.terminate()
    out, err = iperf_client_popen.communicate()
    if out:
        print("iperf client out", out.decode("utf-8"))
    if err:
        print("iperf client err", err.decode("utf-8"))
    out, err = iperf_server_popen.communicate()
    if out:
        print("iperf server out", out.decode("utf-8"))
    if err:
        print("iperf server err", err.decode("utf-8"))

# client_popen.terminate()

out, err = client_popen.communicate()
if out:
    print("client out", out.decode("utf-8"))
if err:
    print("client err", err.decode("utf-8"))

time.sleep(5)

server_tcpdump_popen.terminate()
out, err = server_tcpdump_popen.communicate()
if out:
    print("server_tcpdump out", out.decode("utf-8"))
if err:
    print("server_tcpdump err", err.decode("utf-8"))

client_tcpdump_popen.terminate()
out, err = client_tcpdump_popen.communicate()
if out:
    print("client_tcpdump out", out.decode("utf-8"))
if err:
    print("client_tcpdump err", err.decode("utf-8"))

server_popen.terminate()
out, err = server_popen.communicate()
if out:
    print("server out", out.decode("utf-8"))
if err:
    print("server err", err.decode("utf-8"))

mininet.cli.CLI(net)
net.stop()

