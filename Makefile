KERNEL_HEADERS=$(HOME)/src/linux-socfpga/
obj-m+=altera_fifo.o

.PHONY: all
all: upload

.PHONY: upload
upload: modules
	rsync -a --no-g --no-o altera_fifo.ko root@de10:packet-fifo/

.PHONY: modules
modules:
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C $(KERNEL_HEADERS) M=$(PWD) modules

.PHONY: clean
clean:
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C $(KERNEL_HEADERS) M=$(PWD) clean
