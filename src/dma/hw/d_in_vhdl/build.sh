#!/usr/bin/env sh

# pcie
PCIE_DIR=../../../pcie
gcc -Wall -O2 -I$PCIE_DIR -Wno-strict-aliasing -c $PCIE_DIR/pcie.c -o pcie_c.o ;
gcc -Wall -O2 -I$PCIE_DIR -c $PCIE_DIR/pcie_net.c -o pcie_net_c.o ;
gcc -Wall -O2 -I$PCIE_DIR -c $PCIE_DIR/pcie_glue.c -o pcie_glue_c.o ;

# minimalistic ghdl main
MAIN_DIR=../../../main
gcc -Wall -O2 -c $MAIN_DIR/main_ghdl.c -o main_ghdl_c.o ;

# generate makefile if required
[ -e Makefile ] || ./gen_makefile.sh ;

# compile vhdl file and elaborate
make ;
