/* Linker script for UEFI applications */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

ENTRY(efi_main)

SECTIONS
{
    /* Start after PE headers (SectionAlignment = 0x1000) */
    . = 0x1000;

    .text : {
        _text = .;
        *(.text .text.*)
        _text_end = .;
    }

    . = ALIGN(0x1000);

    .data : {
        _data = .;
        *(.data .data.*)
        *(.rodata .rodata.*)
        *(.sdata .sdata.*)
        *(.got .got.*)
        *(.got.plt)
        _data_end = .;
    }

    . = ALIGN(0x1000);

    .bss : {
        _bss = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    . = ALIGN(0x1000);

    /* Dummy .reloc section — UEFI PE loader requires this */
    .reloc : {
        *(.reloc)
    }

    . = ALIGN(0x1000);

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gnu*)
        *(.debug*)
        *(.rel .rel.*)
        *(.rela .rela.*)
    }
}
