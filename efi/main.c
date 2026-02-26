/*
 * main.c — BootAI UEFI entry point
 *
 * Boot menu → interactive command REPL with networking.
 * Stays in UEFI boot services mode (never calls ExitBootServices).
 */

#include "efi.h"
#include "foundry/tensor/tensor.h"
#include "model/model.h"
#include "iwn/iwn.h"

/* Printf (provided by uefi_libc.c) */
int uefi_printf(const char *fmt, ...);

/* Globals */
EFI_SYSTEM_TABLE  *gST = 0;
EFI_BOOT_SERVICES *gBS = 0;
EFI_HANDLE         gImageHandle = 0;

/* Protocol GUIDs */
EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x0964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

EFI_GUID gEfiGraphicsOutputProtocolGuid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};

/* Simple string comparison helpers (freestanding — no libc) */
static int str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void show_boot_menu(void) {
    efi_print_ascii("\r\n");
    efi_print_ascii("  BootAI v0.1\r\n");
    efi_print_ascii("  ==================\r\n");
    efi_print_ascii("\r\n");
    efi_print_ascii("  [1] Run Live\r\n");
    efi_print_ascii("  [2] Install to Hard Drive\r\n");
    efi_print_ascii("  [3] Settings\r\n");
    efi_print_ascii("\r\n");
    efi_print_ascii("  Select: ");
}

/* ----------------------------------------------------------------- */
/* Tensor self-test — verifies foundry tensor ops on bare metal      */
/* ----------------------------------------------------------------- */
static void tensor_self_test(void) {
    efi_print_ascii("  Running tensor self-test...\r\n");
    int pass;

    /* Disable autograd — we're doing inference, not training */
    bool prev_grad = tm_no_grad_begin();

    /* Test 1: matmul — [2,3] @ [3,2] = [2,2] with all 3.0 */
    {
        int64_t shape_a[] = {2, 3};
        int64_t shape_b[] = {3, 2};
        tm_tensor *a = tm_ones(shape_a, 2, TM_F32);
        tm_tensor *b = tm_ones(shape_b, 2, TM_F32);
        tm_tensor *c = tm_matmul(a, b);

        float *data = tm_data_f32(c);
        pass = (c && c->shape[0] == 2 && c->shape[1] == 2 &&
                data[0] == 3.0f && data[1] == 3.0f &&
                data[2] == 3.0f && data[3] == 3.0f);
        efi_print_ascii(pass ? "  PASS: matmul\r\n" : "  FAIL: matmul\r\n");

        tm_free(a);
        tm_free(b);
        tm_free(c);
    }

    /* Test 2: softmax — use tm_softmax now */
    {
        int64_t shape[] = {1, 4};
        tm_tensor *t = tm_ones(shape, 2, TM_F32);
        tm_tensor *s = tm_softmax(t, -1);

        float *data = tm_data_f32(s);
        /* softmax of [1,1,1,1] = [0.25, 0.25, 0.25, 0.25] */
        float total = data[0] + data[1] + data[2] + data[3];
        pass = (total > 0.999f && total < 1.001f &&
                data[0] > 0.24f && data[0] < 0.26f);
        efi_print_ascii(pass ? "  PASS: softmax\r\n" : "  FAIL: softmax\r\n");

        tm_free(t);
        tm_free(s);
    }

    /* Test 3: rms_norm — normalized output */
    {
        int64_t shape[] = {1, 4};
        tm_tensor *t = tm_full(shape, 2, TM_F32, 2.0);
        tm_tensor *w = tm_ones(shape, 2, TM_F32);
        tm_tensor *n = tm_rms_norm(t, w, 1e-6f);

        if (!n) { efi_print_ascii("  FAIL: rms_norm returned NULL\r\n"); tm_free(t); tm_free(w); goto done; }

        float *data = tm_data_f32(n);
        /* RMS of [2,2,2,2] = 2.0, so normalized = 2/2 * 1 = 1.0 */
        pass = (data[0] > 0.99f && data[0] < 1.01f);
        efi_print_ascii(pass ? "  PASS: rms_norm\r\n" : "  FAIL: rms_norm\r\n");

        tm_free(t);
        tm_free(w);
        tm_free(n);
    }

    /* Test 4: silu — silu(x) = x * sigmoid(x) */
    {
        int64_t shape[] = {4};
        tm_tensor *t = tm_ones(shape, 1, TM_F32);
        if (!t) { efi_print_ascii("  FAIL: silu: tm_ones NULL\r\n"); goto done; }

        tm_tensor *s = tm_silu(t);
        if (!s) { efi_print_ascii("  FAIL: silu: tm_silu NULL\r\n"); tm_free(t); goto done; }

        float *data = tm_data_f32(s);
        /* silu(1.0) = 1.0 * sigmoid(1.0) = 1.0 * 0.7311 ≈ 0.7311 */
        pass = (data[0] > 0.73f && data[0] < 0.74f);
        efi_print_ascii(pass ? "  PASS: silu\r\n" : "  FAIL: silu\r\n");

        tm_free(t);
        tm_free(s);
    }

done:
    tm_no_grad_end(prev_grad);
    efi_print_ascii("  Tensor self-test complete.\r\n");
}

/* ---- Integer-to-ASCII helper (freestanding) ---- */
static void int_to_str(char *buf, int val) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    char tmp[12]; int ti = 0;
    while (val > 0) { tmp[ti++] = '0' + val % 10; val /= 10; }
    int si = 0;
    if (neg) buf[si++] = '-';
    while (ti > 0) buf[si++] = tmp[--ti];
    buf[si] = '\0';
}

/* ---- Model + Tokenizer (loaded once, persists across infer calls) ---- */
/* Non-static so api.c can access them for HTTP API inference */
ba_model     *g_model = 0;
ba_tokenizer *g_tok   = 0;

#define MODEL_PATH "\\rwkvos\\model.btw"
#define VOCAB_PATH "\\rwkvos\\vocab.btv"

static int ensure_model_loaded(void) {
    if (g_model && g_tok) return 1;

    /* Initialize filesystem */
    if (efi_fs_init() != EFI_SUCCESS) {
        efi_print_ascii("  ERROR: filesystem init failed\r\n");
        return 0;
    }

    if (!g_model) {
        efi_print_ascii("  Loading model from " MODEL_PATH "...\r\n");
        g_model = ba_model_load(MODEL_PATH);
        if (!g_model) {
            efi_print_ascii("  ERROR: failed to load model\r\n");
            return 0;
        }

        ba_model_config *cfg = &g_model->config;
        char num[12];
        efi_print_ascii("  Model loaded: ");
        int_to_str(num, cfg->n_layers); efi_print_ascii(num); efi_print_ascii(" layers, ");
        int_to_str(num, cfg->n_embd); efi_print_ascii(num); efi_print_ascii(" hidden, ");
        int_to_str(num, cfg->vocab_size); efi_print_ascii(num); efi_print_ascii(" vocab\r\n");

        /* Print size */
        uint64_t mb = g_model->raw_size / (1024 * 1024);
        int_to_str(num, (int)mb);
        efi_print_ascii("  Weights: "); efi_print_ascii(num); efi_print_ascii(" MB\r\n");
    }

    if (!g_tok) {
        efi_print_ascii("  Loading vocab from " VOCAB_PATH "...\r\n");
        g_tok = ba_tokenizer_load(VOCAB_PATH);
        if (!g_tok) {
            efi_print_ascii("  ERROR: failed to load vocab\r\n");
            return 0;
        }
        efi_print_ascii("  Vocab loaded.\r\n");
    }

    return 1;
}

/* ---- Helper: append string to buffer ---- */
static int buf_append_str(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max) buf[pos++] = *s++;
    return pos;
}

static int buf_append_n(char *buf, int pos, int max, const char *s, int n) {
    for (int i = 0; i < n && pos < max; i++) buf[pos++] = s[i];
    return pos;
}

/* ---- Chat command: multi-turn conversation ---- */

static void command_chat(void) {
    if (!ensure_model_loaded()) return;

    ba_sample_config cfg = ba_default_config();
    cfg.temperature = 0.7f;
    cfg.max_tokens = 512;

    int eos_id = ba_tokenizer_eos(g_tok);
    int im_end_id = ba_tokenizer_im_end(g_tok);
    int vocab = ba_model_vocab_size(g_model);

    /* Reset KV cache for new conversation */
    ba_model_reset(g_model);

    char prompt[1024];
    char chat_buf[2048];
    int tokens[2048];
    char decode_buf[64];
    char num[12];
    int first_turn = 1;
    float *logits = 0;

    efi_print_ascii("\r\n  Chat (empty line to exit)\r\n\r\n");

    for (;;) {
        efi_print_ascii("> ");
        int len = efi_read_line(prompt, sizeof(prompt));
        if (len <= 0) break;

        /* Build token sequence for this turn */
        int ci = 0;
        if (first_turn) {
            /* First turn: system prompt + user message */
            ci = buf_append_str(chat_buf, ci, 2000,
                "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n<|im_start|>user\n");
        } else {
            /* Continue: close previous assistant, start new user turn.
             * The <|im_end|> we sampled was never fed to the model,
             * so we include it here to properly close the assistant response. */
            ci = buf_append_str(chat_buf, ci, 2000,
                "<|im_end|>\n<|im_start|>user\n");
        }
        ci = buf_append_n(chat_buf, ci, 2000, prompt, len);
        ci = buf_append_str(chat_buf, ci, 2000,
            "<|im_end|>\n<|im_start|>assistant\n");
        chat_buf[ci] = '\0';

        int n_prompt = ba_tokenizer_encode(g_tok, chat_buf, tokens, 2048);
        if (n_prompt <= 0) {
            efi_print_ascii("  ERROR: tokenization failed\r\n");
            continue;
        }

        /* Prefill turn tokens (KV cache accumulates across turns) */
        efi_print_ascii("  Thinking");
        for (int i = 0; i < n_prompt; i++) {
            logits = ba_model_forward(g_model, &tokens[i], 1);
            if ((i + 1) % 5 == 0) efi_print_ascii(".");
        }
        efi_print_ascii("\r\n\r\n");

        /* Generate response */
        int n_gen = 0;
        for (int i = 0; i < cfg.max_tokens; i++) {
            int next = ba_model_sample(logits, vocab, &cfg);
            if (next == eos_id || next == im_end_id) break;

            ba_tokenizer_decode(g_tok, &next, 1, decode_buf, sizeof(decode_buf));
            for (int j = 0; decode_buf[j]; j++) {
                if (decode_buf[j] == '\n') {
                    efi_print_ascii("\r\n");
                } else {
                    CHAR16 ch[2] = { (CHAR16)(unsigned char)decode_buf[j], 0 };
                    efi_print(ch);
                }
            }
            n_gen++;
            logits = ba_model_forward(g_model, &next, 1);
        }

        efi_print_ascii("\r\n\r\n");
        first_turn = 0;
    }
}

#define HTTP_RESPONSE_MAX 8192
#define DISPLAY_MAX       2048

static void command_repl(void) {
    char line[512];
    char response[HTTP_RESPONSE_MAX];

    efi_print_ascii("\r\n");
    efi_print_ascii("  Commands: chat, serve [port], ls [path], cat <file>, fetch <url>,\r\n");
    efi_print_ascii("            wifi [scan|connect|disconnect|save], mdns, iwn, netinfo,\r\n");
    efi_print_ascii("            netdiag, tensor, exit\r\n");
    efi_print_ascii("\r\n");

    for (;;) {
        efi_log_flush();  /* persist log to bootai.log after each command */
        efi_print_ascii("> ");

        int len = efi_read_line(line, sizeof(line));

        if (len <= 0) continue;

        /* exit */
        if (str_eq(line, "exit")) {
            break;
        }

        /* chat — multi-turn conversation */
        if (str_eq(line, "chat") || str_eq(line, "infer")) {
            command_chat();
            continue;
        }

        /* ls [path] — list directory */
        if (str_eq(line, "ls") || str_starts_with(line, "ls ")) {
            if (efi_fs_init() != EFI_SUCCESS) {
                efi_print_ascii("  Filesystem not available\r\n");
                continue;
            }
            const char *path = line[2] == ' ' ? line + 3 : "";
            while (*path == ' ') path++;
            efi_list_dir(path);
            continue;
        }

        /* cat <file> — read and display file */
        if (str_starts_with(line, "cat ")) {
            const char *path = line + 4;
            while (*path == ' ') path++;
            if (*path == '\0') {
                efi_print_ascii("  Usage: cat <file>\r\n");
                continue;
            }
            if (efi_fs_init() != EFI_SUCCESS) {
                efi_print_ascii("  Filesystem not available\r\n");
                continue;
            }
            void *buf = 0;
            UINTN fsize = 0;
            if (efi_read_file(path, &buf, &fsize) == EFI_SUCCESS && buf) {
                /* Display up to 2KB */
                UINTN show = fsize > 2048 ? 2048 : fsize;
                char *p = (char *)buf;
                for (UINTN i = 0; i < show; i++) {
                    if (p[i] == '\n') {
                        efi_print_ascii("\r\n");
                    } else if (p[i] >= 0x20 && p[i] < 0x7F) {
                        CHAR16 ch[2] = { (CHAR16)(unsigned char)p[i], 0 };
                        efi_print(ch);
                    }
                }
                efi_print_ascii("\r\n");
                if (fsize > 2048) efi_print_ascii("  (truncated to 2KB)\r\n");
                gBS->FreePool(buf);
            } else {
                efi_print_ascii("  File not found\r\n");
            }
            continue;
        }

        /* netinfo */
        if (str_eq(line, "netinfo")) {
            if (efi_net_available()) {
                efi_print_ascii("  Network: ready\r\n");
            } else {
                efi_print_ascii("  Network: unavailable\r\n");
            }
            continue;
        }

        /* serve [port] — start HTTP server */
        if (str_eq(line, "serve") || str_starts_with(line, "serve ")) {
            if (!efi_net_available()) {
                efi_print_ascii("  Network not available\r\n");
                continue;
            }
            if (!ensure_model_loaded()) continue;

            uint16_t port = 8080;
            if (line[5] == ' ') {
                const char *p = line + 6;
                while (*p == ' ') p++;
                if (*p >= '1' && *p <= '9') {
                    int v = 0;
                    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                    if (v > 0 && v < 65536) port = (uint16_t)v;
                }
            }
            if (ba_mdns_init() == EFI_SUCCESS) ba_mdns_announce();
            ba_http_serve(port);
            ba_mdns_stop();
            continue;
        }

        /* fetch <url> */
        if (str_starts_with(line, "fetch ")) {
            const char *url = line + 6;
            /* Skip leading spaces */
            while (*url == ' ') url++;

            if (*url == '\0') {
                efi_print_ascii("  Usage: fetch <url>\r\n");
                continue;
            }

            if (!efi_net_available()) {
                efi_print_ascii("  Network not available\r\n");
                continue;
            }

            efi_print_ascii("  Fetching: ");
            efi_print_ascii(url);
            efi_print_ascii("\r\n");

            UINTN out_size = 0;
            EFI_STATUS status = efi_http_get(url, response, sizeof(response), &out_size);

            if (status == EFI_SUCCESS && out_size > 0) {
                efi_print_ascii("\r\n--- Response (");
                /* Print size */
                char size_str[12];
                int si = 0;
                UINTN sv = out_size;
                if (sv == 0) {
                    size_str[si++] = '0';
                } else {
                    char tmp[12];
                    int ti = 0;
                    while (sv > 0) {
                        tmp[ti++] = '0' + (int)(sv % 10);
                        sv /= 10;
                    }
                    while (ti > 0) size_str[si++] = tmp[--ti];
                }
                size_str[si] = '\0';
                efi_print_ascii(size_str);
                efi_print_ascii(" bytes) ---\r\n");

                /* Truncate for display */
                if (out_size > DISPLAY_MAX) {
                    response[DISPLAY_MAX] = '\0';
                    out_size = DISPLAY_MAX;
                }

                /* Print response, converting \n to \r\n */
                for (UINTN i = 0; i < out_size; i++) {
                    if (response[i] == '\n') {
                        efi_print_ascii("\r\n");
                    } else if (response[i] == '\r') {
                        /* skip — we handle \n above */
                    } else if (response[i] >= 0x20 && response[i] < 0x7F) {
                        CHAR16 ch[2] = { (CHAR16)(unsigned char)response[i], 0 };
                        efi_print(ch);
                    }
                }

                efi_print_ascii("\r\n--- End ---\r\n");

                if (out_size == DISPLAY_MAX) {
                    efi_print_ascii("  (truncated to 2KB for display)\r\n");
                }
            } else {
                efi_print_ascii("  Fetch failed or empty response\r\n");
            }
            continue;
        }

        /* wifi — WiFi management commands */
        if (str_eq(line, "wifi")) {
            efi_wifi_status();
            continue;
        }
        if (str_eq(line, "wifi scan")) {
            efi_wifi_scan();
            continue;
        }
        if (str_eq(line, "wifi disconnect")) {
            efi_wifi_disconnect();
            continue;
        }
        if (str_starts_with(line, "wifi save")) {
            const char *ssid = line + 9;
            while (*ssid == ' ') ssid++;
            if (*ssid == '\0') {
                efi_print_ascii("  Usage: wifi save <SSID> [password]\r\n");
                continue;
            }
            /* Check if password is provided after SSID (space-separated) */
            const char *pw = ssid;
            while (*pw && *pw != ' ') pw++;
            if (*pw == ' ') {
                /* Split SSID and password */
                char save_ssid[64];
                int slen = (int)(pw - ssid);
                if (slen > 63) slen = 63;
                for (int i = 0; i < slen; i++) save_ssid[i] = ssid[i];
                save_ssid[slen] = '\0';
                pw++;
                while (*pw == ' ') pw++;
                efi_wifi_save(save_ssid, pw);
            } else {
                efi_wifi_save(ssid, "");
            }
            continue;
        }
        if (str_starts_with(line, "wifi connect ")) {
            const char *ssid = line + 13;
            while (*ssid == ' ') ssid++;
            if (*ssid == '\0') {
                efi_print_ascii("  Usage: wifi connect <SSID>\r\n");
                continue;
            }
            /* Prompt for password */
            efi_print_ascii("  Password: ");
            char password[128];
            int plen = efi_read_line(password, sizeof(password));
            if (plen < 0) plen = 0;
            password[plen] = '\0';

            EFI_STATUS ws = efi_wifi_connect(ssid, password);
            if (ws == EFI_SUCCESS) {
                efi_print_ascii("  Save to wifi.conf? (y/n) ");
                char yn[8];
                efi_read_line(yn, sizeof(yn));
                if (yn[0] == 'y' || yn[0] == 'Y') {
                    efi_wifi_save(ssid, password);
                }
            }
            continue;
        }

        /* mdns — manual mDNS test */
        if (str_eq(line, "mdns")) {
            if (!efi_net_available()) {
                efi_print_ascii("  Network not available\r\n");
                continue;
            }
            if (ba_mdns_init() == EFI_SUCCESS) {
                ba_mdns_announce();
                efi_print_ascii("  mDNS running. Press any key to stop.\r\n");
                for (;;) {
                    ba_mdns_poll();
                    gBS->Stall(100000);
                    EFI_INPUT_KEY k;
                    if (efi_read_key(&k) == EFI_SUCCESS) break;
                }
                ba_mdns_stop();
            }
            continue;
        }

        /* iwn — PCI probe for Intel WiFi */
        if (str_eq(line, "iwn")) {
            iwn_probe();
            continue;
        }

        /* iwn scan — scan for WiFi networks (Phase 3) */
        if (str_eq(line, "iwn scan")) {
            iwn_scan_cmd();
            continue;
        }

        /* iwn connect <SSID> — connect to a WiFi network (Phase 4b) */
        if (str_starts_with(line, "iwn connect ")) {
            const char *ssid = line + 12;
            while (*ssid == ' ') ssid++;
            if (*ssid) {
                iwn_connect_cmd(ssid);
            } else {
                efi_print_ascii("  Usage: iwn connect <SSID>\r\n");
            }
            continue;
        }

        /* iwn pass <password> — set WPA2 password (Phase 4b) */
        if (str_starts_with(line, "iwn pass ")) {
            const char *pw = line + 9;
            while (*pw == ' ') pw++;
            if (*pw) {
                iwn_set_password(pw);
            } else {
                efi_print_ascii("  Usage: iwn pass <password>\r\n");
            }
            continue;
        }

        /* iwn ping — ARP ping to test data path (Phase 4d) */
        if (str_eq(line, "iwn ping")) {
            iwn_ping_cmd();
            continue;
        }

        /* netdiag — enumerate network protocols */
        if (str_eq(line, "netdiag")) {
            efi_net_diag();
            continue;
        }

        /* tensor — run tensor self-test */
        if (str_eq(line, "tensor")) {
            tensor_self_test();
            continue;
        }

        /* Default: echo back */
        efi_print_ascii("  ");
        efi_print_line(line);
    }
}

/*
 * Switch to a larger stack and call fn(). UEFI default stack is ~128KB
 * which overflows during tensor operations. We allocate 1MB via AllocatePages.
 */
static void run_on_new_stack(void (*fn)(void), void *stack_top) {
    register void *old_rsp __asm__("r15");
    __asm__ volatile (
        "mov %%rsp, %[old]\n\t"     /* save old stack pointer */
        "mov %[top], %%rsp\n\t"     /* switch to new stack */
        "call *%[func]\n\t"         /* call the function */
        "mov %[old], %%rsp\n\t"     /* restore old stack */
        : [old] "=&r"(old_rsp)
        : [top] "r"(stack_top), [func] "r"(fn)
        : "memory", "cc",
          "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11"
    );
}

static void __attribute__((used)) app_main(void);

/* EFI application entry point — must use MS x64 calling convention */
EFI_STATUS __attribute__((ms_abi)) efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImageHandle = ImageHandle;

    /* Clear screen */
    efi_clear_screen();

    /* Initialize memory */
    efi_memory_init();

    /* Allocate 2MB stack for tensor operations */
    #define APP_STACK_PAGES 512  /* 512 * 4KB = 2MB */
    uint64_t stack_addr = 0;
    EFI_STATUS st = gBS->AllocatePages(0, EfiLoaderData, APP_STACK_PAGES, &stack_addr);
    if (st != EFI_SUCCESS) {
        efi_print_ascii("  FATAL: cannot allocate stack\r\n");
        return EFI_SUCCESS;
    }

    /* Stack grows down — top is base + size, 16-byte aligned.
     * x86_64 ABI: RSP must be 16-byte aligned BEFORE call.
     * The 'call' instruction pushes 8 bytes, so RSP inside the callee
     * will be at 16n+8, which is correct per ABI. */
    void *stack_top = (void *)((stack_addr + APP_STACK_PAGES * 4096ULL) & ~0xFULL);
    run_on_new_stack(app_main, stack_top);

    return EFI_SUCCESS;
}

static void __attribute__((used)) app_main(void) {
    /* Initialize filesystem */
    efi_print_ascii("  Initializing filesystem...\r\n");
    if (efi_fs_init() == EFI_SUCCESS) {
        efi_print_ascii("  Filesystem: ready\r\n");
    } else {
        efi_print_ascii("  Filesystem: unavailable\r\n");
    }

    /* Load third-party UEFI drivers from \drivers\ */
    efi_load_drivers();

    /* Initialize networking (wired Ethernet + DHCP) */
    efi_print_ascii("  Initializing network...\r\n");
    EFI_STATUS net_status = efi_net_init();
    if (net_status == EFI_SUCCESS && efi_net_available()) {
        efi_print_ascii("  Network: ready\r\n");
    } else {
        efi_print_ascii("  Network: unavailable\r\n");
    }

    /* Auto-connect WiFi if adapter present and wifi.conf exists */
    efi_wifi_auto_connect();

    /* Run tensor self-test at boot */
    tensor_self_test();

    /* Main loop: boot menu → action → back to menu */
    for (;;) {
        show_boot_menu();

        /* Wait for a menu selection key */
        EFI_INPUT_KEY key;
        for (;;) {
            efi_wait_key();
            EFI_STATUS status = efi_read_key(&key);
            if (status == EFI_SUCCESS && key.UnicodeChar >= '1' && key.UnicodeChar <= '3') {
                /* Echo the selection */
                CHAR16 echo[2] = { key.UnicodeChar, 0 };
                efi_print(echo);
                efi_print_ascii("\r\n");
                break;
            }
        }

        switch (key.UnicodeChar) {
        case '1':
            /* Run Live — command REPL with networking */
            command_repl();
            break;
        case '2':
            efi_print_ascii("\r\n  Install not yet implemented.\r\n");
            efi_print_ascii("  Press any key to return to menu...\r\n");
            efi_wait_key();
            /* Drain the key */
            efi_read_key(&key);
            break;
        case '3':
            efi_print_ascii("\r\n  Settings not yet implemented.\r\n");
            efi_print_ascii("  Press any key to return to menu...\r\n");
            efi_wait_key();
            efi_read_key(&key);
            break;
        }

        efi_clear_screen();
    }
}
