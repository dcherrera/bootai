/*
 * main.c — BootAI UEFI entry point
 *
 * Boot menu → interactive command REPL with networking.
 * Stays in UEFI boot services mode (never calls ExitBootServices).
 */

#include "efi.h"
#include "foundry/tensor/tensor.h"
#include "model/model.h"

/* Printf (provided by uefi_libc.c) */
int uefi_printf(const char *fmt, ...);

/* Globals */
EFI_SYSTEM_TABLE  *gST = 0;
EFI_BOOT_SERVICES *gBS = 0;

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
static ba_model     *g_model = 0;
static ba_tokenizer *g_tok   = 0;

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

/* ---- Inference command ---- */

static void command_infer(void) {
    if (!ensure_model_loaded()) return;

    efi_print_ascii("\r\n  Enter prompt (empty line to cancel):\r\n");
    efi_print_ascii("  > ");

    char prompt[1024];
    int len = efi_read_line(prompt, sizeof(prompt));
    if (len <= 0) return;

    /* Wrap in chat template for instruct models:
     * <|im_start|>system\nYou are a helpful assistant.<|im_end|>\n
     * <|im_start|>user\n{prompt}<|im_end|>\n<|im_start|>assistant\n */
    char chat_buf[2048];
    int ci = 0;
    const char *pre = "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n<|im_start|>user\n";
    while (*pre && ci < 2000) chat_buf[ci++] = *pre++;
    for (int p = 0; p < len && ci < 2000; p++) chat_buf[ci++] = prompt[p];
    const char *post = "<|im_end|>\n<|im_start|>assistant\n";
    while (*post && ci < 2000) chat_buf[ci++] = *post++;
    chat_buf[ci] = '\0';

    int tokens[2048];
    int n_prompt = ba_tokenizer_encode(g_tok, chat_buf, tokens, 2048);
    if (n_prompt <= 0) {
        efi_print_ascii("  ERROR: tokenization failed\r\n");
        return;
    }

    char num[12];
    int_to_str(num, n_prompt);
    efi_print_ascii("  Prompt: "); efi_print_ascii(num); efi_print_ascii(" tokens [");
    for (int t = 0; t < n_prompt && t < 10; t++) {
        if (t > 0) efi_print_ascii(", ");
        int_to_str(num, tokens[t]); efi_print_ascii(num);
    }
    efi_print_ascii("]\r\n");
    /* Reset model state */
    ba_model_reset(g_model);

    ba_sample_config cfg = ba_default_config();
    cfg.temperature = 0.0f;  /* greedy for determinism */
    cfg.max_tokens = 256;

    int eos_id = ba_tokenizer_eos(g_tok);
    int im_end_id = ba_tokenizer_im_end(g_tok);
    int vocab = ba_model_vocab_size(g_model);

    /* Prefill: process prompt tokens one at a time with progress */
    efi_print_ascii("  Prefilling");
    float *logits = 0;
    for (int i = 0; i < n_prompt; i++) {
        logits = ba_model_forward(g_model, &tokens[i], 1);
        if ((i + 1) % 5 == 0) efi_print_ascii(".");
    }
    efi_print_ascii(" done\r\n\r\n");

    /* Generate tokens */
    int n_gen = 0;
    char decode_buf[64];

    for (int i = 0; i < cfg.max_tokens; i++) {
        /* Debug: show top-5 logits for first 5 tokens */
        if (i < 5) {
            int top_ids[5] = {0,0,0,0,0};
            float top_vals[5] = {-1e30f,-1e30f,-1e30f,-1e30f,-1e30f};
            for (int v = 0; v < vocab; v++) {
                if (logits[v] > top_vals[4]) {
                    top_vals[4] = logits[v]; top_ids[4] = v;
                    /* bubble up */
                    for (int s = 4; s > 0 && top_vals[s] > top_vals[s-1]; s--) {
                        float tv = top_vals[s]; top_vals[s] = top_vals[s-1]; top_vals[s-1] = tv;
                        int ti = top_ids[s]; top_ids[s] = top_ids[s-1]; top_ids[s-1] = ti;
                    }
                }
            }
            uefi_printf("  [step %d] top5:", i);
            for (int t = 0; t < 5; t++) {
                char tbuf[32];
                ba_tokenizer_decode(g_tok, &top_ids[t], 1, tbuf, sizeof(tbuf));
                uefi_printf(" %d(%s)=%.2f", top_ids[t], tbuf, (double)top_vals[t]);
            }
            uefi_printf("\r\n");
        }

        int next = ba_model_sample(logits, vocab, &cfg);

        /* Check for EOS */
        if (next == eos_id || next == im_end_id) break;

        /* Decode and display token */
        ba_tokenizer_decode(g_tok, &next, 1, decode_buf, sizeof(decode_buf));

        /* Print, converting \n to \r\n for UEFI console */
        for (int j = 0; decode_buf[j]; j++) {
            if (decode_buf[j] == '\n') {
                efi_print_ascii("\r\n");
            } else {
                CHAR16 ch[2] = { (CHAR16)(unsigned char)decode_buf[j], 0 };
                efi_print(ch);
            }
        }

        n_gen++;

        /* Forward pass for next token */
        logits = ba_model_forward(g_model, &next, 1);
    }

    efi_print_ascii("\r\n\r\n  [");
    int_to_str(num, n_gen);
    efi_print_ascii(num);
    efi_print_ascii(" tokens generated]\r\n");
}

#define HTTP_RESPONSE_MAX 8192
#define DISPLAY_MAX       2048

static void command_repl(void) {
    char line[512];
    char response[HTTP_RESPONSE_MAX];

    efi_print_ascii("\r\n");
    efi_print_ascii("  Commands: infer, ls [path], cat <file>, fetch <url>, netinfo, tensor, exit\r\n");
    efi_print_ascii("\r\n");

    for (;;) {
        efi_print_ascii("> ");

        int len = efi_read_line(line, sizeof(line));

        if (len <= 0) continue;

        /* exit */
        if (str_eq(line, "exit")) {
            break;
        }

        /* infer — run inference */
        if (str_eq(line, "infer")) {
            command_infer();
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
    (void)ImageHandle;

    gST = SystemTable;
    gBS = SystemTable->BootServices;

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

    /* Initialize networking */
    efi_print_ascii("  Initializing network...\r\n");
    EFI_STATUS net_status = efi_net_init();
    if (net_status == EFI_SUCCESS && efi_net_available()) {
        efi_print_ascii("  Network: ready\r\n");
    } else {
        efi_print_ascii("  Network: unavailable\r\n");
    }

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
