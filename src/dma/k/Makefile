#---------------------------------------------------------------------
# DRIVER source
PWD  := $(shell pwd)



#---------------------------------------------------------------------
# Needed because this Makefile will also be called by
# kernel source Makefile when compiling drivers
# (and we don't want the kernel to recompile anything else
# than the driver)
#
ifneq ($(KERNELRELEASE),)

obj-m := kdma.o
#CFLAGS_kdma.o := -DCONFIG_KDMA_XXX=YYY

else

#---------------------------------------------------------------------
#
driver:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	make min_clean

#---------------------------------------------------------------------
#
min_clean:
	rm -f kdma_mod.o
	rm -f .*cmd
	rm -rf .tmp*
	rm -f modules.order
	rm -f Module.symvers

#---------------------------------------------------------------------
#
clean: min_clean
	rm -f kdma.ko

#---------------------------------------------------------------------
# end of ifneq ($(KERNELRELEASE),)
endif

