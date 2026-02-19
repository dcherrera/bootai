CC = x86_64-elf-gcc
LD = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy

CFLAGS = -ffreestanding -mno-red-zone -mno-sse -fno-stack-protector \
         -fPIC -fshort-wchar \
         -Wall -Wextra -std=c11 \
         -I .

# Phase 1: EFI layer only (console + input + memory)
EFI_SRC   = efi/main.c efi/console.c efi/input.c efi/efi_memory.c efi/reloc.c efi/net.c

# Full build sources (future phases)
FS_NET_SRC = efi/fs.c
MODEL_SRC = model/qwen2.c model/rwkvx.c model/wkv7.c model/rope.c \
            model/kv_cache.c model/loader.c model/sampling.c
TOOLS_SRC = tools/tool_use.c tools/tools_fs.c tools/tools_net.c tools/tools_system.c
TOK_SRC   = tokenizer/bpe.c tokenizer/rwkv_tokenizer.c
TENSOR_SRC = foundry/tensor/tensor.c

ALL_SRC = $(EFI_SRC) $(FS_NET_SRC) $(MODEL_SRC) $(TOOLS_SRC) $(TOK_SRC) $(TENSOR_SRC)
ALL_OBJ = $(ALL_SRC:.c=.o)

PHASE1_OBJ = $(EFI_SRC:.c=.o)

# UEFI firmware for QEMU (built from EDK2 with NETWORK_IP4_ENABLE)
OVMF_CODE ?= ovmf-code.fd
OVMF_VARS ?= ovmf-vars.fd

# Targets
.PHONY: phase1 efi run clean usb

phase1: bootai.efi

# Link to ELF, then convert to PE/COFF EFI application
bootai.efi: $(PHASE1_OBJ)
	$(LD) -T efi/uefi.lds -nostdlib -shared -Bsymbolic -o bootai.so $^
	$(OBJCOPY) -j .text -j .data -j .bss -j .dynamic -j .dynsym -j .rel \
	           -j .rela -j .reloc \
	           -O pei-x86-64 --subsystem efi-app \
	           bootai.so $@

efi: all_bootai.efi

all_bootai.efi: $(ALL_OBJ)
	$(LD) -T efi/uefi.lds -nostdlib -shared -Bsymbolic -o bootai.so $^
	$(OBJCOPY) -j .text -j .data -j .bss -j .dynamic -j .dynsym -j .rel \
	           -j .rela -j .reloc \
	           -O pei-x86-64 --subsystem efi-app \
	           bootai.so $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Run in QEMU
run: bootai.efi
	mkdir -p esp/EFI/BOOT esp/rwkvos esp/workspace
	cp bootai.efi esp/EFI/BOOT/BOOTX64.EFI
	echo "FS0:\EFI\BOOT\BOOTX64.EFI" > esp/startup.nsh
	@if [ -f model.bin ]; then cp model.bin esp/rwkvos/model.bin; fi
	@if [ -f vocab.bin ]; then cp vocab.bin esp/rwkvos/vocab.bin; fi
	cp $(OVMF_VARS) esp/ovmf-vars-session.fd
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=esp/ovmf-vars-session.fd \
		-drive format=raw,file=fat:rw:esp \
		-m 8G \
		-display none \
		-serial stdio \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0 \
		-device virtio-rng-pci

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
	rm -f $(ALL_OBJ) $(PHASE1_OBJ) bootai.elf bootai.so bootai.efi
	rm -rf esp/
