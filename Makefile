CC = x86_64-elf-gcc
LD = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy

# Base flags: freestanding UEFI application (never ExitBootServices, SSE available)
# -mno-sse removed: UEFI guarantees SSE, needed for float/double in tensor ops
# -mf16c removed: F16C uses VEX/AVX encoding, UEFI doesn't enable AVX (causes #UD)
CFLAGS = -O2 -msse4.2 \
         -ffreestanding -fno-builtin -mno-red-zone -fno-stack-protector \
         -fPIC -fshort-wchar \
         -Wall -Wextra -std=c11 \
         -I .

# Shim headers: declaration-only libc for bare-metal foundry (no __builtin_* wrappers)
SHIM_CFLAGS = -I shim

# Phase 1: EFI layer only (console + input + memory)
EFI_SRC   = efi/main.c efi/console.c efi/input.c efi/efi_memory.c efi/reloc.c efi/net.c efi/http_server.c efi/api.c

# Phase 3: UEFI libc shim (malloc/printf via UEFI Boot Services)
LIBC_SRC  = efi/uefi_libc.c

# Foundry tensor library sources
FOUNDRY_SRC = foundry/tensor/tensor.c foundry/quantization/quantization.c

# Phase 4: Filesystem + Model + Tokenizer
FS_SRC    = efi/fs.c
MODEL_SRC = model/qwen2.c model/loader.c model/sampling.c \
            model/rope.c model/kv_cache.c
TOK_SRC   = tokenizer/bpe.c

# Phase 4 combined
PHASE4_SRC = $(FS_SRC) $(MODEL_SRC) $(TOK_SRC)

# Full build sources (future phases)
RWKVX_SRC  = model/rwkvx.c model/wkv7.c
TOOLS_SRC  = tools/tool_use.c tools/tools_fs.c tools/tools_net.c tools/tools_system.c
RWKV_TOK   = tokenizer/rwkv_tokenizer.c

ALL_SRC = $(EFI_SRC) $(LIBC_SRC) $(FOUNDRY_SRC) $(PHASE4_SRC) \
          $(RWKVX_SRC) $(TOOLS_SRC) $(RWKV_TOK)
ALL_OBJ = $(ALL_SRC:.c=.o)

PHASE1_OBJ = $(EFI_SRC:.c=.o)
PHASE3_OBJ = $(EFI_SRC:.c=.o) $(LIBC_SRC:.c=.o) $(FOUNDRY_SRC:.c=.o)
PHASE4_OBJ = $(EFI_SRC:.c=.o) $(LIBC_SRC:.c=.o) $(FOUNDRY_SRC:.c=.o) $(PHASE4_SRC:.c=.o)

# UEFI firmware for QEMU (built from EDK2 with NETWORK_IP4_ENABLE)
OVMF_CODE ?= ovmf-code.fd
OVMF_VARS ?= ovmf-vars.fd

# Targets
.PHONY: phase1 phase3 phase4 efi run clean usb img

# Model files (source paths)
MODEL_BTW ?= ../models/base/SmolLM2-135M-Instruct/model.btw
MODEL_BTV ?= ../models/base/SmolLM2-135M-Instruct/model.btv

phase1: PHASE3_OBJ = $(PHASE1_OBJ)
phase1: bootai.efi

phase3: bootai.efi

phase4: bootai-p4.efi

# Phase 3 build (EFI + libc + foundry tensor)
bootai.efi: $(PHASE3_OBJ)
	$(LD) -T efi/uefi.lds -nostdlib -shared -Bsymbolic -o bootai.so $^
	$(OBJCOPY) -j .text -j .data -j .bss -j .dynamic -j .dynsym -j .rel \
	           -j .rela -j .reloc \
	           -O pei-x86-64 --subsystem efi-app \
	           bootai.so $@

# Phase 4 build (+ filesystem + model + tokenizer)
bootai-p4.efi: $(PHASE4_OBJ)
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

# Default rule for EFI sources
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# uefi_libc.c uses only compiler-provided headers + efi.h — no JACLibc
efi/uefi_libc.o: efi/uefi_libc.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Foundry sources: force-include the shim to redirect malloc/printf, plus JACLibc
foundry/tensor/tensor.o: foundry/tensor/tensor.c
	$(CC) $(CFLAGS) $(SHIM_CFLAGS) -include efi/uefi_libc.h \
	    -I foundry/tensor -Wno-unused-parameter -Wno-sign-compare \
	    -c -o $@ $<

foundry/quantization/quantization.o: foundry/quantization/quantization.c
	$(CC) $(CFLAGS) $(SHIM_CFLAGS) -include efi/uefi_libc.h \
	    -I foundry/tensor -I foundry/quantization -Wno-unused-parameter -Wno-sign-compare \
	    -c -o $@ $<

# Create FAT32 disk image with EFI binary + model + vocab
# Size: model + vocab + 50MB headroom, rounded up to nearest MB
img: bootai-p4.efi
	@echo "Creating bootai.img..."
	@size=$$(( ($$(stat -f%z $(MODEL_BTW)) + $$(stat -f%z $(MODEL_BTV)) + 52428800) / 1048576 )); \
	dd if=/dev/zero of=bootai.img bs=1m count=$$size 2>/dev/null
	mformat -i bootai.img -F ::
	mmd -i bootai.img ::EFI ::EFI/BOOT ::rwkvos ::workspace
	mcopy -i bootai.img bootai-p4.efi ::EFI/BOOT/BOOTX64.EFI
	mcopy -i bootai.img $(MODEL_BTW) ::rwkvos/model.btw
	mcopy -i bootai.img $(MODEL_BTV) ::rwkvos/vocab.btv
	@echo "bootai.img ready ($$(ls -lh bootai.img | awk '{print $$5}'))"

# Run in QEMU (phase 4)
# Uses bootai.img (FAT32 disk image built with mtools)
run: bootai-p4.efi
	mcopy -o -i bootai.img bootai-p4.efi ::EFI/BOOT/BOOTX64.EFI
	mkdir -p esp
	cp $(OVMF_VARS) esp/ovmf-vars-session.fd
	qemu-system-x86_64 \
		-accel tcg,thread=multi,tb-size=2048 \
		-cpu max \
		-smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=esp/ovmf-vars-session.fd \
		-drive file=bootai.img,format=raw,if=virtio \
		-m 8G \
		-display none \
		-serial stdio \
		-netdev user,id=net0,hostfwd=tcp::8080-:8080 \
		-device virtio-net-pci,netdev=net0 \
		-device virtio-rng-pci

# Run phase 3 (no model, networking + tensor only)
run3: bootai.efi
	mkdir -p esp/EFI/BOOT esp/rwkvos esp/workspace
	cp bootai.efi esp/EFI/BOOT/BOOTX64.EFI
	echo "FS0:\EFI\BOOT\BOOTX64.EFI" > esp/startup.nsh
	cp $(OVMF_VARS) esp/ovmf-vars-session.fd
	qemu-system-x86_64 \
		-accel tcg,thread=multi,tb-size=2048 \
		-cpu max \
		-smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=esp/ovmf-vars-session.fd \
		-drive file=bootai.img,format=raw,if=virtio \
		-m 8G \
		-display none \
		-serial stdio \
		-netdev user,id=net0,hostfwd=tcp::8080-:8080 \
		-device virtio-net-pci,netdev=net0 \
		-device virtio-rng-pci

# Create bootable USB/SD card — writes the same image QEMU uses
usb: bootai-p4.efi
ifndef DISK
	$(error Set DISK= to target device, e.g. make usb DISK=/dev/disk4)
endif
	mcopy -o -i bootai.img bootai-p4.efi ::EFI/BOOT/BOOTX64.EFI
	diskutil unmountDisk $(DISK)
	dd if=bootai.img of=$(DISK) bs=1m
	diskutil eject $(DISK)
	@echo "Ready. Boot from it on any x86_64 machine."

# Export model weights (run on host with Python + transformers)
# IMPORTANT: MODEL must match the .btw weights on disk (currently SmolLM2-135M)
MODEL ?= HuggingFaceTB/SmolLM2-135M-Instruct
export-model:
	python3 tools/export_weights.py \
		--model $(MODEL) \
		--quant q8 \
		--output esp/rwkvos/model.btw

export-vocab:
	python3 tools/export_weights.py \
		--model $(MODEL) \
		--vocab-only \
		--output esp/rwkvos/model.btw

clean:
	rm -f $(ALL_OBJ) $(PHASE1_OBJ) $(PHASE3_OBJ) $(PHASE4_OBJ) \
	      bootai.elf bootai.so bootai.efi bootai-p4.efi
	rm -rf esp/
