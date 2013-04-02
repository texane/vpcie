#!/usr/bin/env sh

# generate device command line
test -z $PCIEFW_NDEV && PCIEFW_NDEV=1;
pciefw_opts="";
for n in `seq 0 $(($PCIEFW_NDEV - 1))`; do
    lport=$((42424 + $n * 2 + 0));
    rport=$((42424 + $n * 2 + 1));
    pciefw_opts="$pciefw_opts -device pciefw,laddr=127.0.0.1,lport=$lport,raddr=127.0.0.1,rport=$rport" ;
done

# network options
# net_opts="-net nic,model=e1000 -net user -redir tcp:5556::22"
net_opts="-net nic,model=e1000 -net tap,ifname=tap0,script=no"

# install networking rules

sudo ifconfig tap0 down > /dev/null 2>&1 ;
sudo ifconfig tap0 192.168.0.2 netmask 255.255.255.0 ;

sudo sh -c 'echo 1 > /proc/sys/net/ipv4/ip_forward' ;

# machine port redirections
# a virtual machine has a range of 10000 contiguous TCP ports, starting from $base.
# $base is a non zero integer multiple of 10000. thus, there can be at most 5 virtual
# machines. this range is used to setup a redirection mirror such that the port
# $base + $i on the host redirects to the port $base + 5000 + $i on the virtual machine

# TODO: use a double for loop
sudo iptables -t nat -C PREROUTING -p tcp --dport 10000 -j DNAT --to-destination 192.168.0.1:15000 || \
    sudo iptables -t nat -A PREROUTING -p tcp --dport 10000 -j DNAT --to-destination 192.168.0.1:15000 ;
sudo iptables -t nat -C POSTROUTING -p tcp --dport 15000 -j MASQUERADE || \
    sudo iptables -t nat -A POSTROUTING -p tcp --dport 15000 -j MASQUERADE ;

# shutdown if required
# sudo /etc/qemu-ifdown tap0 ;
# route add -net 192.168.0.0 netmask 255.255.255.0 dev br0

# disk options
# disk_opts=$HOME/repo/vm/linux_2_6_20/main.img
# disk_opts=$HOME/repo/vm/aurel32/debian_squeeze_amd64_standard.qcow2
disk_opts=$HOME/repo/vm/linux_3_6_11_lfs/main.img

# memory options
mem_opts='-m 16G'
# mem_opts='-m 512M'
# mem_opts='-mem-path /hugepages -mem-prealloc -m 1G'

sudo sh -c "$HOME/repo/qemu/x86_64-softmmu/qemu-system-x86_64 $disk_opts $pciefw_opts $net_opts $mem_opts >/dev/null 2>&1 &" ;

# bring tap0 up after qemu brung it down
sleep 1; sudo ifconfig tap0 192.168.0.2 netmask 255.255.255.0 ;
