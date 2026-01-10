# Makefile for kernel module

obj-m := jerry_rt_module.o

jerry_rt_module-objs := rt_main.o trace_ring_buffer.o

MODULE_NAME := jerry_rt_module

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) INSTALL_MOD_PATH=$(INSTALL_MOD_PATH) modules_install
