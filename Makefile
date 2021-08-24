MODULENAME := vpoll
obj-m += $(MODULENAME).o
# name cannot be same (try)
$(MODULENAME)-y += module.o

KERNELDIR ?= /lib/modules/`uname -r`/build
PWD       := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	gcc -Wall -o user user.c

check: all
	sudo rmmod vpoll || echo
	sudo insmod vpoll.ko
	./user
	sudo rmmod vpoll

indent:
	clang-format -i trace_time.h
	clang-format -i module.c

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	$(RM) user
