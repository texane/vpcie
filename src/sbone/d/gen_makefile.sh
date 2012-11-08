#!/usr/bin/env sh

PCIE_DIR=../../pcie

ghdl -a \
 $PCIE_DIR/pcie_pkg.vhdl \
 $PCIE_DIR/pcie_endpoint.vhdl \
 sbone_pkg.vhdl \
 sbone_rep_mux.vhdl \
 sbone_bar_addr_cmp.vhdl \
 sbone_reg_wo.vhdl \
 sbone_reg_wo_msi.vhdl \
 sbone_reg_rw.vhdl \
 rst_clk.vhdl \
 stimul.vhdl \
 main.vhdl ;

# add to GHDLFLAGS
ghdl --gen-makefile main > Makefile.tmp;
sed 's/GHDLFLAGS=/GHDLFLAGS=-Wl,main_ghdl_c.o -Wl,pcie_glue_c.o -Wl,pcie_c.o -Wl,pcie_net_c.o -Wl,-lpthread/' Makefile.tmp > Makefile;
