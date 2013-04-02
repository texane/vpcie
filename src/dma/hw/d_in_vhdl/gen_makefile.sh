#!/usr/bin/env sh

PCIE_DIR=../../pcie
SBONE_DIR=../../sbone

ghdl -a \
 $PCIE_DIR/pcie_pkg.vhdl \
 $PCIE_DIR/pcie_endpoint.vhdl \
 dma.vhdl \
 rst_clk.vhdl \
 stimul.vhdl \
 main.vhdl ;

# add to GHDLFLAGS
ghdl --gen-makefile main > Makefile.tmp;
sed 's/GHDLFLAGS=/GHDLFLAGS=-Wl,main_ghdl_c.o -Wl,pcie_glue_c.o -Wl,pcie_c.o -Wl,pcie_net_c.o -Wl,-lpthread/' Makefile.tmp > Makefile;
