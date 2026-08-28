obj-m := dsipoke.o

# Out-of-tree kbuild. KDIR must be a configured tree matching the running
# badge kernel (vendor 5.10: the badge's /lib/modules/$(uname -r)/build
# harvest, or pkgs/kernel/duos-vendor's .dev output) — vermagic must match
# or insmod refuses.
KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
