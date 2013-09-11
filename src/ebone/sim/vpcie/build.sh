#!/usr/bin/env bash

[ -z "$VPCIE_DIR" ] && export VPCIE_DIR=$HOME/segfs/repo/vpcie
[ -z "$PCIE_DIR" ] && export PCIE_DIR=$VPCIE_DIR/src/pcie
[ -z "$MAIN_DIR" ] && export MAIN_DIR=$VPCIE_DIR/src/main
[ -z "$THIS_DIR" ] && export THIS_DIR=`pwd`
[ -z "$RASHPA_DIR" ] && export RASHPA_DIR=$THIS_DIR/../../src

# compile C files by hand
# pcie
gcc -Wall -O2 -I$PCIE_DIR -Wno-strict-aliasing -c $PCIE_DIR/pcie.c -o pcie_c.o ;
gcc -Wall -O2 -I$PCIE_DIR -c $PCIE_DIR/pcie_net.c -o pcie_net_c.o ;
gcc -Wall -O2 -I$PCIE_DIR -c $PCIE_DIR/pcie_glue.c -o pcie_glue_c.o ;
# minimalistic ghdl main
gcc -Wall -O2 -c $MAIN_DIR/main_ghdl.c -o main_ghdl_c.o ;

# vhdl files
VHDL_FILES=''
VHDL_FILES+=" $RASHPA_DIR/ebs_core/src/ebs_core.vhd"
VHDL_FILES+=" $RASHPA_DIR/eb_common/src/eb_common_pkg.vhd"
VHDL_FILES+=" $RASHPA_DIR/eb_common/src/fifo_s.vhd"
VHDL_FILES+=" $PCIE_DIR/pcie_pkg.vhdl"
VHDL_FILES+=" $PCIE_DIR/pcie_endpoint.vhdl"
VHDL_FILES+=" $RASHPA_DIR/ebm0_pcie_vpcie/src/ebm0_pcie_vpcie_pkg.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebm0_pcie_vpcie/src/ebm0_pcie_vpcie.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebs_regs/src/ebs_regs_pkg.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebs_regs/src/ebs_regs.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebs_bram/src/ebs_bram_pkg.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebs_bram/src/ebs_bram.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebm_ebft/src/ebm_ebft_pkg.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebm_ebft/src/ebftm_ft.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebm_ebft/src/ebftm_mx.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebm_ebft/src/ebftm_regs.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebm_ebft/src/ebm_ebft.vhd"
VHDL_FILES+=" $RASHPA_DIR/dualport_bram.vhd"
VHDL_FILES+=" $RASHPA_DIR/rashpa/rashpa_pkg.vhd"
VHDL_FILES+=" $RASHPA_DIR/rashpa/rashpa_btd_fifo.vhd"
VHDL_FILES+=" $RASHPA_DIR/rashpa/rashpa_btu.vhd"
VHDL_FILES+=" $RASHPA_DIR/rashpa/rashpa_bgu.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebs_btd_fifo/src/ebs_btd_fifo_pkg.vhd"
VHDL_FILES+=" $RASHPA_DIR/ebs_btd_fifo/src/ebs_btd_fifo.vhd"
VHDL_FILES+=" $RASHPA_DIR/rashpa_top.vhd"
VHDL_FILES+=" $THIS_DIR/rst_clk.vhdl"
VHDL_FILES+=" $THIS_DIR/vbench_top.vhdl"

# generate makefile if required
if [ ! -e Makefile ]; then
 ghdl -i $VHDL_FILES ;
 # add to GHDLFLAGS
 ghdl --gen-makefile vbench_top > Makefile.tmp ;
 sed 's/GHDLFLAGS=/GHDLFLAGS=-Wl,main_ghdl_c.o -Wl,pcie_glue_c.o -Wl,pcie_c.o -Wl,pcie_net_c.o -Wl,-lpthread/' Makefile.tmp > Makefile ;
fi

# analyze
ghdl -a $VHDL_FILES ;

# elaborate
make ;
