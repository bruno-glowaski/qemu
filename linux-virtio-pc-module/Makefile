obj-m += producer.o

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) MO=$(PWD)/build modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) MO=$(PWD)/build modules_install
