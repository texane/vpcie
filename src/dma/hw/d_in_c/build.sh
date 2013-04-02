#!/usr/bin/env sh

PCIE_DIR=../../pcie

gcc -Wall -Wstrict-aliasing=0 -O2 \
-I. -I$PCIE_DIR \
-o main_dma \
main_dma.c \
$PCIE_DIR/pcie.c $PCIE_DIR/pcie_net.c
