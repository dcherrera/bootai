/*
 * console.c — Text output via UEFI Simple Text Output Protocol
 *
 * Uses gST->ConOut->OutputString() for all text display.
 * Also mirrors output to COM1 serial port (0x3F8) for headless debugging.
 * Functions: efi_print(), efi_print_ascii(), efi_print_line(), efi_clear_screen()
 */

#include "efi.h"

/* ---- Log buffer: tee all console output, flush to file on demand ---- */
#define LOG_BUF_SIZE (128 * 1024)  /* 128 KB ring buffer */
static char log_buf[LOG_BUF_SIZE];
static UINTN log_pos = 0;
static int log_active = 0;

void efi_log_flush(void) {
    if (log_pos == 0) return;
    efi_write_file("bootai.log", log_buf, log_pos);
}

/* x86 port I/O for serial debug output */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1 + 1, 0x00);  /* Disable interrupts */
    outb(COM1 + 3, 0x80);  /* Enable DLAB */
    outb(COM1 + 0, 0x01);  /* Baud rate 115200 (divisor 1) */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  /* 8N1 */
    outb(COM1 + 2, 0xC7);  /* Enable FIFO */
    outb(COM1 + 4, 0x0B);  /* IRQs enabled, RTS/DSR set */
}

static void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s) {
    while (*s) {
        serial_putc(*s);
        s++;
    }
}

static int serial_inited = 0;

void efi_print(const CHAR16 *str) {
    if (gST && gST->ConOut && str) {
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)str);
    }
}

void efi_print_ascii(const char *str) {
    if (!str) return;

    /* Tee to log buffer */
    if (!log_active && efi_fs_root()) log_active = 1;
    if (log_active) {
        const char *p = str;
        while (*p && log_pos < LOG_BUF_SIZE - 1)
            log_buf[log_pos++] = *p++;
    }

    /* Init serial on first use */
    if (!serial_inited) {
        serial_init();
        serial_inited = 1;
    }

    /* ConOut is mirrored to serial by OVMF — no separate serial_puts needed */

    /* Convert ASCII to UCS-2 in a stack buffer, flush in chunks */
    CHAR16 buf[128];
    int i = 0;

    while (*str) {
        buf[i++] = (CHAR16)(unsigned char)*str++;

        if (i >= 127) {
            buf[i] = 0;
            efi_print(buf);
            i = 0;
        }
    }

    if (i > 0) {
        buf[i] = 0;
        efi_print(buf);
    }
}

void efi_print_line(const char *str) {
    efi_print_ascii(str);
    efi_print_ascii("\r\n");
}

void efi_clear_screen(void) {
    if (gST && gST->ConOut) {
        gST->ConOut->ClearScreen(gST->ConOut);
    }
}
