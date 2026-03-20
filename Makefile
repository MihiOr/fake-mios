ARCH = x86_64
USB_DRIVE = E:
USB_DRIVE_LETTER = $(subst :,,$(USB_DRIVE))
USB_LABEL = MIOS

EFI_INC = /usr/include/efi
EFI_ARCH_INC = /usr/include/efi/$(ARCH)
EFI_CRT = /usr/lib/crt0-efi-$(ARCH).o
EFI_LDS = /usr/lib/elf_$(ARCH)_efi.lds
EFI_LIBDIR = /usr/lib
OVMF_CODE_SECBOOT = /usr/share/OVMF/OVMF_CODE_4M.secboot.fd
OVMF_VARS = OVMF_VARS_4M.ms.fd
RELEASE_DIR = dist
RELEASE_ZIP = $(RELEASE_DIR)/fake-mios-image.zip

SIGN_KEY = keys/mios.key
SIGN_CERT = keys/mios.crt
SIGN_CERT_DER = keys/mios.der
SBAT_FILE = keys/sbat.csv

CFLAGS = -O3 -I$(EFI_INC) -I$(EFI_ARCH_INC) -fpic -fshort-wchar -mno-red-zone -ffreestanding -fno-stack-protector -Wall -Wextra
LDFLAGS = -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic -L$(EFI_LIBDIR)

SRC = src/main.c
OBJ = main.o
SO = main.so
EFI = grubx64.efi

.PHONY: all copy run clean build usb release check-secureboot-files

all: $(EFI)

$(OBJ): $(SRC)
	gcc $(CFLAGS) -c $(SRC) -o $(OBJ)

$(SO): $(OBJ)
	ld $(LDFLAGS) $(EFI_CRT) $(OBJ) -o $(SO) -lgnuefi -lefi

$(EFI): $(SO)
	objcopy \
		-j .text \
		-j .sdata \
		-j .data \
		-j .dynamic \
		-j .dynsym \
		-j .rel \
		-j .rela \
		-j .reloc \
		--target=efi-app-$(ARCH) $(SO) $(EFI)

check-secureboot-files:
	@for required in "$(SIGN_KEY)" "$(SIGN_CERT)" "$(SIGN_CERT_DER)" "$(SBAT_FILE)"; do \
		if [ ! -f "$$required" ]; then \
			echo "Error: missing Secure Boot file '$$required'."; \
			echo "Generate local signing material in keys/ as described in README.md before running this target."; \
			exit 1; \
		fi; \
	done

copy: check-secureboot-files $(SO)
	objcopy \
		-j .text \
		-j .sdata \
		-j .data \
		-j .dynamic \
		-j .dynsym \
		-j .rel \
		-j .rela \
		-j .reloc \
		--target=efi-app-$(ARCH) $(SO) $(EFI)
	objcopy --set-section-alignment .sbat=512 --add-section .sbat=$(SBAT_FILE) --adjust-section-vma .sbat+0x10000000 $(EFI) $(EFI)
	sbsign --key $(SIGN_KEY) --cert $(SIGN_CERT) --output $(EFI) $(EFI)
	mkdir -p image/boot_keys
	cp -r $(SIGN_CERT_DER) image/boot_keys

	cp $(EFI) image/EFI/BOOT/$(EFI)
	mkdir -p image/EFI/BOOT/assets
	cp -r src/assets/* image/EFI/BOOT/assets/


run:
	@if [ ! -f "$(OVMF_CODE_SECBOOT)" ]; then \
		echo "Error: missing Secure Boot OVMF code at $(OVMF_CODE_SECBOOT)."; \
		echo "Install OVMF or override OVMF_CODE_SECBOOT to point to your local firmware."; \
		exit 1; \
	fi
	qemu-system-x86_64 \
		-machine q35 \
		-m 512 \
		-cpu qemu64 \
		-drive if=pflash,format=raw,unit=0,file=$(OVMF_CODE_SECBOOT),readonly=on \
		-drive if=pflash,format=raw,unit=1,file=$(PWD)/$(OVMF_VARS) \
		-drive format=raw,file=fat:rw:$(PWD)/image \
		-net none \
		-vga std \
		-serial stdio \
		-boot menu=on

clean:
	rm -f *.o *.so $(EFI) image/EFI/BOOT/$(EFI)
	rm -rf image/boot_keys
	rm -rf image/EFI/BOOT/assets

build:
	make clean
	make
	make copy
	make run

usb:
	@usb_vol_output="$$(cmd.exe /C vol $(USB_DRIVE) 2>&1 | tr -d '\r')"; \
	if [ $$? -ne 0 ]; then \
		echo "Error: USB drive $(USB_DRIVE) nije dostupan."; \
		exit 1; \
	fi; \
	echo "$$usb_vol_output" | grep -F "Volume in drive $(USB_DRIVE_LETTER) is $(USB_LABEL)" >/dev/null || { \
		echo "Error: USB drive $(USB_DRIVE) mora postojati i imati label '$(USB_LABEL)'."; \
		exit 1; \
	}
	make clean
	make
	make copy
	sync
	while mountpoint -q /mnt/usb; do sudo umount /mnt/usb; done
	sudo mkdir -p /mnt/usb
	sudo mount -t drvfs E: /mnt/usb
	rm -r -f /mnt/usb/*
	cp -r image/* /mnt/usb
	sync
	while mountpoint -q /mnt/usb; do sudo umount /mnt/usb; done

release: 
	make clean
	make
	make copy
	mkdir -p $(RELEASE_DIR)
	rm -f $(RELEASE_ZIP)
	zip -r $(RELEASE_ZIP) image -x "image/NvVars"
