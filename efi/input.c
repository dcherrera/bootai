/*
 * input.c — Keyboard input via UEFI Simple Text Input Protocol
 *           with 8042 PS/2 fallback for DUET compatibility.
 *
 * Uses gST->ConIn->ReadKeyStroke() first. If that fails repeatedly
 * (DUET's BiosKeyboardDxe doesn't work on all hardware), falls back
 * to direct 8042 PS/2 controller port I/O.
 */

#include "efi.h"

/* -----------------------------------------------------------------------
 * Direct 8042 PS/2 keyboard controller (fallback for DUET)
 * ----------------------------------------------------------------------- */

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

#define PS2_DATA    0x60
#define PS2_STATUS  0x64

/* Set 1 scancode → ASCII (unshifted). 0 = no mapping. */
static const char sc_to_ascii[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\r',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' '
};

/* Set 1 scancode → ASCII (shifted) */
static const char sc_to_ascii_shift[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\r',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' '
};

/* Scancode constants */
#define SC_LSHIFT_PRESS   0x2A
#define SC_RSHIFT_PRESS   0x36
#define SC_LSHIFT_RELEASE 0xAA
#define SC_RSHIFT_RELEASE 0xB6
#define SC_CAPSLOCK       0x3A
#define SC_BACKSPACE      0x0E
#define SC_ENTER          0x1C
#define SC_TAB            0x0F
#define SC_ESCAPE         0x01
#define SC_UP             0x48
#define SC_DOWN           0x50
#define SC_LEFT           0x4B
#define SC_RIGHT          0x4D
#define SC_HOME           0x47
#define SC_END            0x4F
#define SC_DELETE         0x53
#define SC_SPACE          0x39

static int ps2_shift = 0;
static int ps2_caps = 0;

/* Read one key from 8042. Returns 1 if key was filled, 0 if no key. */
static int ps2_read_key(EFI_INPUT_KEY *key) {
    if (!(inb(PS2_STATUS) & 1)) return 0; /* no data available */

    uint8_t sc = inb(PS2_DATA);

    /* Handle shift press/release */
    if (sc == SC_LSHIFT_PRESS || sc == SC_RSHIFT_PRESS) { ps2_shift = 1; return 0; }
    if (sc == SC_LSHIFT_RELEASE || sc == SC_RSHIFT_RELEASE) { ps2_shift = 0; return 0; }

    /* Handle CapsLock toggle */
    if (sc == SC_CAPSLOCK) { ps2_caps = !ps2_caps; return 0; }

    /* Ignore break codes (key release, bit 7 set) */
    if (sc & 0x80) return 0;

    /* Handle extended scancodes (0xE0 prefix) */
    if (sc == 0xE0) {
        /* Wait briefly for the actual scancode */
        for (int i = 0; i < 1000; i++) {
            if (inb(PS2_STATUS) & 1) break;
            /* small delay via empty loop */
            for (volatile int j = 0; j < 100; j++);
        }
        if (!(inb(PS2_STATUS) & 1)) return 0;
        sc = inb(PS2_DATA);
        if (sc & 0x80) return 0; /* break code */

        key->UnicodeChar = 0;
        switch (sc) {
        case 0x48: key->ScanCode = 0x01; return 1; /* Up */
        case 0x50: key->ScanCode = 0x02; return 1; /* Down */
        case 0x4D: key->ScanCode = 0x03; return 1; /* Right */
        case 0x4B: key->ScanCode = 0x04; return 1; /* Left */
        case 0x47: key->ScanCode = 0x05; return 1; /* Home */
        case 0x4F: key->ScanCode = 0x06; return 1; /* End */
        case 0x53: key->ScanCode = 0x08; return 1; /* Delete */
        default: return 0;
        }
    }

    /* Map scancode to ASCII */
    if (sc >= 128) return 0;

    key->ScanCode = 0;

    /* Special keys */
    if (sc == SC_BACKSPACE) { key->UnicodeChar = '\b'; return 1; }
    if (sc == SC_ENTER)     { key->UnicodeChar = '\r'; return 1; }
    if (sc == SC_TAB)       { key->UnicodeChar = '\t'; return 1; }
    if (sc == SC_ESCAPE)    { key->UnicodeChar = 27;   return 1; }

    int shifted = ps2_shift;
    char ch = shifted ? sc_to_ascii_shift[sc] : sc_to_ascii[sc];

    /* Apply CapsLock to letters */
    if (ps2_caps && ch >= 'a' && ch <= 'z') ch -= 32;
    else if (ps2_caps && ch >= 'A' && ch <= 'Z') ch += 32;

    if (ch == 0) return 0;

    key->UnicodeChar = (CHAR16)ch;
    return 1;
}

/* -----------------------------------------------------------------------
 * UEFI keyboard interface
 * ----------------------------------------------------------------------- */

/* 0 = not yet decided, 1 = UEFI works, 2 = use PS/2 fallback */
static int kb_mode = 0;

void efi_keyboard_init(void) {
    if (!gBS) return;

    /* Locate Simple Text Input Ex protocol to control toggle state */
    EFI_GUID ex_guid = { 0xDD9E7534, 0x7762, 0x4698,
                         {0x8C, 0x14, 0xF5, 0x85, 0x17, 0xA6, 0x25, 0xAA} };
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = 0;
    EFI_STATUS status = gBS->LocateProtocol(&ex_guid, 0, (void **)&ex);
    if (status != EFI_SUCCESS || !ex) return;

    /* Clear all toggle states (CapsLock, NumLock, ScrollLock) */
    EFI_KEY_TOGGLE_STATE state = EFI_TOGGLE_STATE_VALID; /* no lock bits = all off */
    ex->SetState(ex, &state);
}

EFI_STATUS efi_read_key(EFI_INPUT_KEY *key) {
    if (!key) return EFI_NOT_READY;

    /* If 8042 PS/2 mode was detected, use it for all key reads */
    if (kb_mode == 2) {
        return ps2_read_key(key) ? EFI_SUCCESS : EFI_NOT_READY;
    }

    if (!gST || !gST->ConIn) return EFI_NOT_READY;

    EFI_STATUS s = gST->ConIn->ReadKeyStroke(gST->ConIn, key);

    /* Also check 8042 if UEFI has nothing */
    if (s != EFI_SUCCESS && ps2_read_key(key)) {
        kb_mode = 2;
        return EFI_SUCCESS;
    }

    return s;
}

void efi_wait_key(void) {
    if (!gBS || !gST || !gST->ConIn) return;

    UINTN index;
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
}

/* UEFI scan codes for special keys */
#define SCAN_UP     0x01
#define SCAN_DOWN   0x02
#define SCAN_RIGHT  0x03
#define SCAN_LEFT   0x04
#define SCAN_HOME   0x05
#define SCAN_END    0x06
#define SCAN_DELETE 0x08

/* Redraw line from cursor position to end, then reposition cursor */
static void redraw_from(const char *buf, int len, int cursor) {
    /* Write chars from cursor to end */
    for (int i = cursor; i < len; i++) {
        CHAR16 ch[2] = { (CHAR16)(unsigned char)buf[i], 0 };
        efi_print(ch);
    }
    /* Overwrite any leftover character (after deletion) */
    efi_print_ascii(" ");
    /* Move cursor back to correct position */
    for (int i = 0; i < len - cursor + 1; i++)
        efi_print_ascii("\b");
}

/* Combined wait+read: tries UEFI ReadKeyStroke first.
 * If it fails 50 times (~500ms), switches permanently to 8042 PS/2. */
static EFI_STATUS read_key_auto(EFI_INPUT_KEY *key) {
    if (kb_mode == 2) goto ps2;

    /* Try UEFI */
    if (kb_mode == 0) {
        /* Auto-detect: try UEFI for a while, then fall back */
        int tries = 0;
        for (;;) {
            EFI_STATUS s = gST->ConIn->ReadKeyStroke(gST->ConIn, key);
            if (s == EFI_SUCCESS) { kb_mode = 1; return s; }

            /* Also check 8042 while waiting */
            if (ps2_read_key(key)) { kb_mode = 2; return EFI_SUCCESS; }

            tries++;
            if (gBS) gBS->Stall(10000); /* 10ms */
        }
    }

    /* kb_mode == 1: UEFI confirmed working */
    for (;;) {
        EFI_STATUS s = gST->ConIn->ReadKeyStroke(gST->ConIn, key);
        if (s == EFI_SUCCESS) return s;
        if (gBS) gBS->Stall(10000);
    }

ps2:
    /* kb_mode == 2: 8042 PS/2 mode */
    for (;;) {
        if (ps2_read_key(key)) return EFI_SUCCESS;
        if (gBS) gBS->Stall(10000);
    }
}

int efi_read_line(char *buf, int max_len) {
    if (!buf || max_len <= 0) return 0;

    int len = 0;     /* total characters in buffer */
    int cursor = 0;  /* cursor position within buffer */
    EFI_INPUT_KEY key;

    while (len < max_len - 1) {
        EFI_STATUS status = read_key_auto(&key);
        if (status != EFI_SUCCESS) continue;

        /* Handle scan codes (arrow keys, delete, home, end) */
        if (key.UnicodeChar == 0 && key.ScanCode != 0) {
            switch (key.ScanCode) {
            case SCAN_LEFT:
                if (cursor > 0) {
                    cursor--;
                    efi_print_ascii("\b");
                }
                break;
            case SCAN_RIGHT:
                if (cursor < len) {
                    CHAR16 ch[2] = { (CHAR16)(unsigned char)buf[cursor], 0 };
                    efi_print(ch);
                    cursor++;
                }
                break;
            case SCAN_HOME:
                while (cursor > 0) { cursor--; efi_print_ascii("\b"); }
                break;
            case SCAN_END:
                while (cursor < len) {
                    CHAR16 ch[2] = { (CHAR16)(unsigned char)buf[cursor], 0 };
                    efi_print(ch);
                    cursor++;
                }
                break;
            case SCAN_DELETE:
                /* macOS Terminal sends 0x7F for backspace, which OVMF
                 * maps to SCAN_DELETE. Treat as backspace. */
                if (cursor > 0) {
                    cursor--;
                    for (int i = cursor; i < len - 1; i++)
                        buf[i] = buf[i + 1];
                    len--;
                    efi_print_ascii("\b");
                    redraw_from(buf, len, cursor);
                }
                break;
            }
            continue;
        }

        /* Enter — submit the line */
        if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            efi_print_ascii("\r\n");
            break;
        }

        /* Backspace */
        if (key.UnicodeChar == '\b' || key.UnicodeChar == 127) {
            if (cursor > 0) {
                cursor--;
                for (int i = cursor; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                len--;
                efi_print_ascii("\b");
                redraw_from(buf, len, cursor);
            }
            continue;
        }

        /* Printable ASCII — insert at cursor position */
        if (key.UnicodeChar >= 0x20 && key.UnicodeChar < 0x7F) {
            if (len >= max_len - 1) continue;

            /* Shift chars right to make room */
            for (int i = len; i > cursor; i--)
                buf[i] = buf[i - 1];
            buf[cursor] = (char)key.UnicodeChar;
            len++;
            cursor++;

            /* Redraw from previous cursor position */
            redraw_from(buf, len, cursor - 1);
            /* Advance cursor on screen past the inserted char */
            CHAR16 echo[2] = { key.UnicodeChar, 0 };
            efi_print(echo);
        }
    }

    buf[len] = '\0';
    return len;
}
