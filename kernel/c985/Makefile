obj-m += c985.o

c985-y := c985_main.o c985_irq.o c985_dma.o c985_fw.o c985_mbox.o c985_v4l2.o c985_debugfs.o c985_nuc100.o cpr.o

ccflags-y := -I$(src)/../include

all:
	LLVM=1 make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	LLVM=1 make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean