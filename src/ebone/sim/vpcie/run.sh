#!/usr/bin/env bash

export PCIE_INET_LADDR=127.0.0.1
export PCIE_INET_RADDR=127.0.0.1

if [ $# -ge 1 ]; then
 rport=$((42424 + 2 * $1 + 0))
 lport=$((42424 + 2 * $1 + 1))
else
 rport=42424
 lport=42425
fi

export PCIE_INET_LPORT=$lport
export PCIE_INET_RPORT=$rport

export PCIE_BAR0_SIZE=0x100
export PCIE_BAR1_SIZE=0x10000
export PCIE_BAR2_SIZE=0x10000

export PCIE_VENDOR_ID=0x2a2a
export PCIE_DEVICE_ID=0x2b2b

./vbench_top --wave=$HOME/tmp/vbench_top.ghw
# ./vbench_top
