CC = x86_64-elf-gcc
LD = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy

# Base flags: freestanding UEFI application (never ExitBootServices, SSE available)
# -mno-sse removed: UEFI guarantees SSE, needed for float/double in tensor ops
# -mf16c removed: F16C uses VEX/AVX encoding, UEFI doesn't enable AVX (causes #UD)
CFLAGS = -O3 -ftree-vectorize -msse4.2 \
         -ffreestanding -fno-builtin -mno-red-zone -fno-stack-protector \
         -fPIC -fshort-wchar \
         -Wall -Wextra -std=c11 \
         -I .

ifdef PROD
CFLAGS += -DPROD
endif

ifdef DEBUG_REMOTE
CFLAGS += -DDEBUG_REMOTE
endif

# Phase 1: EFI layer only (console + input + memory)
EFI_SRC   = efi/main.c efi/console.c efi/input.c efi/efi_memory.c efi/reloc.c efi/install.c efi/net.c efi/net_diag.c efi/http_server.c efi/api.c efi/mgmt_api.c efi/drivers.c efi/wifi.c efi/mdns.c efi/ns_net.c efi/ns_arp.c efi/ns_ip.c efi/ns_udp.c efi/ns_dhcp.c efi/ns_tcp.c iwn/iwn_pci.c iwn/iwn_hw.c iwn/iwn_dma.c iwn/iwn_fw.c iwn/iwn_scan.c iwn/iwn_crypto.c iwn/iwn_assoc.c iwn/iwn_eapol.c iwn/iwn_data.c iwn/iwn_ccmp.c iwn/iwn_snp.c e1000e/e1000e_hw.c

# Phase 3: UEFI libc shim (malloc/printf via UEFI Boot Services)
LIBC_SRC  = efi/uefi_libc.c

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

ALL_SRC = $(EFI_SRC) $(LIBC_SRC) $(PHASE4_SRC) \
          $(RWKVX_SRC) $(TOOLS_SRC) $(RWKV_TOK)
ALL_OBJ = $(ALL_SRC:.c=.o)

PHASE1_OBJ = $(EFI_SRC:.c=.o)
PHASE3_OBJ = $(EFI_SRC:.c=.o) $(LIBC_SRC:.c=.o)
PHASE4_OBJ = $(EFI_SRC:.c=.o) $(LIBC_SRC:.c=.o) $(PHASE4_SRC:.c=.o)

# UEFI firmware for QEMU (built from EDK2 with NETWORK_IP4_ENABLE)
OVMF_CODE ?= ovmf-code.fd
OVMF_VARS ?= ovmf-vars.fd

# Targets
.PHONY: phase1 phase3 phase4 efi run run-prod clean usb img img-multi release test-iwn-crypto

# Model files (source paths)
MODEL_BTW ?= DownloadedModels/Qwen2.5-Coder-0.5B-Instruct/model.btw
MODEL_BTV ?= DownloadedModels/Qwen2.5-Coder-0.5B-Instruct/model.btv

phase1: PHASE3_OBJ = $(PHASE1_OBJ)
phase1: bootai.efi

phase3: bootai.efi

phase4: bootai-p4.efi

# Phase 3 build (EFI + libc)
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

# Create FAT32 disk image with EFI binary + model + vocab
# Size: model + vocab + 50MB headroom, rounded up to nearest MB
img: bootai-p4.efi
	@echo "Creating bootai.img..."
	@size=$$(( ($$(stat -f%z $(MODEL_BTW)) + $$(stat -f%z $(MODEL_BTV)) + 52428800) / 1048576 )); \
	dd if=/dev/zero of=bootai.img bs=1m count=$$size 2>/dev/null
	mformat -i bootai.img -F ::
	mmd -i bootai.img ::EFI ::EFI/BOOT ::rwkvos ::workspace ::drivers
	mcopy -i bootai.img bootai-p4.efi ::EFI/BOOT/BOOTX64.EFI
	mcopy -i bootai.img wifi.conf.template ::wifi.conf
	mcopy -i bootai.img $(MODEL_BTW) ::rwkvos/model.btw
	mcopy -i bootai.img $(MODEL_BTV) ::rwkvos/vocab.btv
	@echo "bootai.img ready ($$(ls -lh bootai.img | awk '{print $$5}'))"

# Multi-model disk image — multiple models in rwkvos/models/<name>/
# Usage: make img-multi
# Models are auto-discovered from MODELS variable or use defaults
MODELS_SMOL_BTW ?= ../models/base/SmolLM2-135M-Instruct/model.btw
MODELS_SMOL_BTV ?= ../models/base/SmolLM2-135M-Instruct/model.btv
MODELS_QWEN_BTW ?= DownloadedModels/Qwen2.5-Coder-3B-Instruct/model.btw
MODELS_QWEN_BTV ?= DownloadedModels/Qwen2.5-Coder-3B-Instruct/model.btv

img-multi: all_bootai.efi
	@echo "Creating multi-model bootai.img..."
	@size=0; \
	for f in $(MODELS_SMOL_BTW) $(MODELS_SMOL_BTV) $(MODELS_QWEN_BTW) $(MODELS_QWEN_BTV); do \
		if [ -f "$$f" ]; then size=$$(( $$size + $$(stat -f%z "$$f") )); fi; \
	done; \
	size=$$(( ($$size + 52428800) / 1048576 )); \
	dd if=/dev/zero of=bootai.img bs=1m count=$$size 2>/dev/null
	mformat -i bootai.img -F ::
	mmd -i bootai.img ::EFI ::EFI/BOOT ::rwkvos ::rwkvos/models ::workspace ::drivers
	mcopy -i bootai.img all_bootai.efi ::EFI/BOOT/BOOTX64.EFI
	mcopy -i bootai.img wifi.conf.template ::wifi.conf
	@if [ -f "$(MODELS_SMOL_BTW)" ]; then \
		echo "  Adding smollm-135m..."; \
		mmd -i bootai.img ::rwkvos/models/smollm-135m; \
		mcopy -i bootai.img $(MODELS_SMOL_BTW) ::rwkvos/models/smollm-135m/model.btw; \
		mcopy -i bootai.img $(MODELS_SMOL_BTV) ::rwkvos/models/smollm-135m/vocab.btv; \
	fi
	@if [ -f "$(MODELS_QWEN_BTW)" ]; then \
		echo "  Adding qwen25-coder-3b..."; \
		mmd -i bootai.img ::rwkvos/models/qwen25-coder-3b; \
		mcopy -i bootai.img $(MODELS_QWEN_BTW) ::rwkvos/models/qwen25-coder-3b/model.btw; \
		mcopy -i bootai.img $(MODELS_QWEN_BTV) ::rwkvos/models/qwen25-coder-3b/vocab.btv; \
	fi
	@echo "bootai.img ready ($$(ls -lh bootai.img | awk '{print $$5}'))"

# GPT release image — flashable with balenaEtcher or dd
# Creates a proper GPT disk with an EFI System Partition (ESP)
# Requires: brew install gptfdisk
release:
	$(MAKE) clean
	$(MAKE) PROD=1 efi
	@echo "Creating GPT release image..."
	@fat_bytes=$$(( $$(stat -f%z $(MODEL_BTW)) + $$(stat -f%z $(MODEL_BTV)) + 52428800 )); \
	fat_mb=$$(( (fat_bytes + 1048575) / 1048576 )); \
	fat_sectors=$$(( fat_mb * 2048 )); \
	total_sectors=$$(( fat_sectors + 2048 + 34 )); \
	dd if=/dev/zero of=bootai-release.img bs=512 count=$$total_sectors 2>/dev/null; \
	sgdisk --clear \
	       --new=1:2048:$$((2048 + fat_sectors - 1)) \
	       --typecode=1:EF00 \
	       --change-name=1:BOOTAI \
	       bootai-release.img; \
	esp_offset=$$(( 2048 * 512 )); \
	mformat -i bootai-release.img@@$$esp_offset -F -v BOOTAI ::; \
	mmd -i bootai-release.img@@$$esp_offset ::EFI ::EFI/BOOT ::rwkvos ::workspace ::drivers; \
	mcopy -i bootai-release.img@@$$esp_offset all_bootai.efi ::EFI/BOOT/BOOTX64.EFI; \
	mcopy -i bootai-release.img@@$$esp_offset wifi.conf.template ::wifi.conf; \
	mcopy -i bootai-release.img@@$$esp_offset $(MODEL_BTW) ::rwkvos/model.btw; \
	mcopy -i bootai-release.img@@$$esp_offset $(MODEL_BTV) ::rwkvos/vocab.btv; \
	echo "bootai-release.img ready ($$(ls -lh bootai-release.img | awk '{print $$5}'))"

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

# Run PROD build in QEMU (boots straight into chat, no menu)
run-prod:
	$(MAKE) clean
	$(MAKE) PROD=1 efi
	mcopy -o -i bootai.img all_bootai.efi ::EFI/BOOT/BOOTX64.EFI
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

# Run phase 3 (no model, networking only)
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

test-iwn-crypto: iwn/iwn_crypto.c iwn/test_crypto.c
	cc -std=c11 -Wall -Wextra -O2 -o test_crypto iwn/test_crypto.c iwn/iwn_crypto.c && ./test_crypto && rm -f test_crypto

clean:
	rm -f $(ALL_OBJ) $(PHASE1_OBJ) $(PHASE3_OBJ) $(PHASE4_OBJ) \
	      bootai.elf bootai.so bootai.efi bootai-p4.efi bootai-release.img
	rm -rf esp/
