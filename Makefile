IMAGEFILE = os.img

KERNELDIR = kernel
IMAGEDIR = image

KERNEL = $(KERNELDIR)/kernel.elf

.PHONY: run $(KERNEL) clean
	
$(IMAGEFILE): $(KERNEL)
	@echo Generating Hard Disk Image...
	@cp $(KERNEL) $(IMAGEDIR)/kernel.elf && ./genimg
	
$(KERNEL): 
	@echo Building kernel...
	@$(MAKE) -C $(KERNELDIR)
	
run: $(IMAGEFILE)
	@echo Testing image in QEMU...
	@qemu-system-x86_64 $(IMAGEFILE) -bios /usr/share/ovmf/OVMF.fd -m 8192 -no-reboot -no-shutdown -enable-kvm -smp 4
	
clean:
	@$(MAKE) -C $(KERNELDIR) clean
