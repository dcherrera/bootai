CC = x86_64-elf-gcc
LD = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy

CFLAGS = -ffreestanding -mno-red-zone -mno-sse -fno-stack-protector \
         -Wall -Wextra -std=c99 \
         -I JACLibc/include -I foundry -I .

LDFLAGS = -nostdlib -shared -Wl,-dll -Wl,--subsystem,10

# Source files
EFI_SRC   = efi/main.c efi/console.c efi/input.c efi/fs.c efi/net.c
MODEL_SRC = model/qwen2.c model/rwkvx.c model/wkv7.c model/rope.c \
            model/kv_cache.c model/loader.c model/sampling.c
TOOLS_SRC = tools/tool_use.c tools/tools_fs.c tools/tools_net.c tools/tools_system.c
TOK_SRC   = tokenizer/bpe.c tokenizer/rwkv_tokenizer.c
TENSOR_SRC = foundry/tensor/tensor.c

ALL_SRC = $(EFI_SRC) $(MODEL_SRC) $(TOOLS_SRC) $(TOK_SRC) $(TENSOR_SRC)
ALL_OBJ = $(ALL_SRC:.c=.o)

# UEFI firmware for QEMU
OVMF ?= /opt/homebrew/share/qemu/edk2-x86_64-code.fd

# Targets
.PHONY: efi run clean usb

efi: bootai.efi

bootai.efi: $(ALL_OBJ)
	$(CC) $(LDFLAGS) -o bootai.so $^
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .reloc --target=efi-app-x86_64 \
	           bootai.so $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Run in QEMU
run: bootai.efi
	mkdir -p esp/EFI/BOOT esp/rwkvos esp/workspace
	cp bootai.efi esp/EFI/BOOT/BOOTX64.EFI
	@if [ -f model.bin ]; then cp model.bin esp/rwkvos/model.bin; fi
	@if [ -f vocab.bin ]; then cp vocab.bin esp/rwkvos/vocab.bin; fi
	qemu-system-x86_64 \
		-bios $(OVMF) \
		-drive format=raw,file=fat:rw:esp \
		-m 8G \
		-serial stdio \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0

# Create bootable USB
usb: bootai.efi
ifndef DISK
	$(error Set DISK= to target device, e.g. make usb DISK=/dev/disk4)
endif
	diskutil eraseDisk FAT32 BOOTAI MBRFormat $(DISK)
	mkdir -p /Volumes/BOOTAI/EFI/BOOT
	mkdir -p /Volumes/BOOTAI/rwkvos
	mkdir -p /Volumes/BOOTAI/workspace
	cp bootai.efi /Volumes/BOOTAI/EFI/BOOT/BOOTX64.EFI
	@if [ -f model.bin ]; then cp model.bin /Volumes/BOOTAI/rwkvos/model.bin; fi
	@if [ -f vocab.bin ]; then cp vocab.bin /Volumes/BOOTAI/rwkvos/vocab.bin; fi
	diskutil eject $(DISK)
	@echo "USB ready. Boot from it on any x86_64 machine."

# Export model weights
model.bin:
	python3 tools/export_weights.py \
		--model Qwen/Qwen2.5-Coder-7B-Instruct \
		--quant q4_k_m \
		--output model.bin

clean:
	rm -f $(ALL_OBJ) bootai.so bootai.efi
	rm -rf esp/
