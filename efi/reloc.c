/*
 * reloc.c — Dummy .reloc section for UEFI PE image
 *
 * UEFI firmware expects a .reloc section in PE images.
 * Since we use RIP-relative addressing (x86_64), no actual
 * relocations are needed, but the section must exist.
 */

__asm__(
    ".section .reloc, \"a\"\n"
    "dummy_reloc:\n"
    ".long 0\n"    /* VirtualAddress (page RVA) */
    ".long 10\n"   /* SizeOfBlock (header + 2 bytes padding) */
    ".word 0\n"    /* Type=0 (IMAGE_REL_BASED_ABSOLUTE = padding) */
);
