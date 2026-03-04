/*
 * main.c — BootAI UEFI entry point
 *
 * Boot menu → interactive command REPL with networking.
 * Stays in UEFI boot services mode (never calls ExitBootServices).
 */

#include "efi.h"
#include "ns_net.h"
#include "model/model.h"
#include "iwn/iwn.h"
#include "e1000e/e1000e.h"

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

EFI_GUID gEfiBlockIoProtocolGuid = {
    0x964e5b21, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

EFI_GUID gEfiGraphicsOutputProtocolGuid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};

EFI_GUID gEfiMpServicesProtocolGuid = {
    0x3fdda605, 0xa76e, 0x4f46,
    {0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08}
};

/* EFI_MP_SERVICES_PROTOCOL — lightweight typedef for probing */
typedef void (EFIAPI *EFI_AP_PROCEDURE)(void *Buffer);

typedef struct {
    uint64_t  ProcessorId;
    uint32_t  StatusFlag;
    struct { uint32_t Package; uint32_t Core; uint32_t Thread; } Location;
} EFI_PROCESSOR_INFORMATION;

typedef struct _EFI_MP_SERVICES_PROTOCOL {
    EFI_STATUS (EFIAPI *GetNumberOfProcessors)(
        struct _EFI_MP_SERVICES_PROTOCOL *This,
        UINTN *NumberOfProcessors,
        UINTN *NumberOfEnabledProcessors);
    EFI_STATUS (EFIAPI *GetProcessorInfo)(
        struct _EFI_MP_SERVICES_PROTOCOL *This,
        UINTN ProcessorNumber,
        EFI_PROCESSOR_INFORMATION *ProcessorInfoBuffer);
    EFI_STATUS (EFIAPI *StartupAllAPs)(
        struct _EFI_MP_SERVICES_PROTOCOL *This,
        EFI_AP_PROCEDURE Procedure,
        BOOLEAN SingleThread,
        EFI_EVENT WaitEvent,
        UINTN TimeoutInMicroSeconds,
        void *ProcedureArgument,
        UINTN **FailedCpuList);
    EFI_STATUS (EFIAPI *StartupThisAP)(
        struct _EFI_MP_SERVICES_PROTOCOL *This,
        EFI_AP_PROCEDURE Procedure,
        UINTN ProcessorNumber,
        EFI_EVENT WaitEvent,
        UINTN TimeoutInMicroseconds,
        void *ProcedureArgument,
        BOOLEAN *Finished);
    EFI_STATUS (EFIAPI *SwitchBSP)(
        struct _EFI_MP_SERVICES_PROTOCOL *This,
        UINTN ProcessorNumber,
        BOOLEAN EnableOldBSP);
    EFI_STATUS (EFIAPI *EnableDisableAP)(
        struct _EFI_MP_SERVICES_PROTOCOL *This,
        UINTN ProcessorNumber,
        BOOLEAN EnableAP,
        uint32_t *HealthFlag);
    EFI_STATUS (EFIAPI *WhoAmI)(
        struct _EFI_MP_SERVICES_PROTOCOL *This,
        UINTN *ProcessorNumber);
} EFI_MP_SERVICES_PROTOCOL;

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
    efi_print_ascii("  [2] Settings\r\n");
    efi_print_ascii("\r\n");
    efi_print_ascii("  Select: ");
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

/* ---- uint64 to ASCII helper (freestanding) ---- */
static void u64_to_str(char *buf, uint64_t val) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[21]; int ti = 0;
    while (val > 0) { tmp[ti++] = '0' + (int)(val % 10); val /= 10; }
    int si = 0;
    while (ti > 0) buf[si++] = tmp[--ti];
    buf[si] = '\0';
}

/* ---- Model + Tokenizer (loaded once, persists across infer calls) ---- */
/* Non-static so api.c can access them for HTTP API inference */
ba_model     *g_model = 0;
ba_tokenizer *g_tok   = 0;

#define MODEL_PATH "\\rwkvos\\model.btw"
#define VOCAB_PATH "\\rwkvos\\vocab.btv"
#define MODELS_DIR "\\rwkvos\\models"

static int ensure_model_loaded(void) {
    if (g_model && g_tok) return 1;

    /* Initialize filesystem */
    if (efi_fs_init() != EFI_SUCCESS) {
        efi_print_ascii("  ERROR: filesystem init failed\r\n");
        return 0;
    }

    if (!g_model) {
        /* Try raw data partition first (HD install stores model there) */
        void *raw_data = 0;
        UINTN raw_size = 0;
        if (ba_raw_read_entry("model.btw", &raw_data, &raw_size) == EFI_SUCCESS) {
            g_model = ba_model_load_buffer(raw_data, raw_size);
        }

        /* Fall back to filesystem (USB boot has model as file) */
        if (!g_model) {
            efi_print_ascii("  Loading model from " MODEL_PATH "...\r\n");
            g_model = ba_model_load(MODEL_PATH);
        }

        /* Try first multi-model dir if flat path also failed */
        if (!g_model) {
            efi_print_ascii("  Trying " MODELS_DIR "\\smollm-135m\\model.btw...\r\n");
            g_model = ba_model_load(MODELS_DIR "\\smollm-135m\\model.btw");
        }

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
        /* Try legacy flat path first, then scan multi-model dirs for vocab */
        efi_print_ascii("  Loading vocab from " VOCAB_PATH "...\r\n");
        g_tok = ba_tokenizer_load(VOCAB_PATH);
        if (!g_tok) {
            /* Try each model directory for a matching vocab */
            static const char *vocab_paths[] = {
                MODELS_DIR "\\smollm-135m\\vocab.btv",
                MODELS_DIR "\\qwen25-coder-3b\\vocab.btv",
                0
            };
            for (int i = 0; vocab_paths[i]; i++) {
                efi_print_ascii("  Trying ");
                efi_print_ascii(vocab_paths[i]);
                efi_print_ascii("...\r\n");
                g_tok = ba_tokenizer_load(vocab_paths[i]);
                if (g_tok) break;
            }
        }
        if (!g_tok) {
            efi_print_ascii("  ERROR: failed to load vocab\r\n");
            return 0;
        }
        efi_print_ascii("  Vocab loaded.\r\n");
    }

    return 1;
}

/* ---- Multi-model support ---- */

/* Small UCS-2 helper (ascii_to_ucs2 is static in fs.c) */
static void to_ucs2(const char *src, CHAR16 *dst, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = (src[i] == '/') ? (CHAR16)'\\' : (CHAR16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

/* String copy, returns bytes written (excluding NUL) */
static int str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* List available models in \rwkvos\models\ */
static void list_models(void) {
    EFI_FILE_PROTOCOL *root = efi_fs_root();
    if (!root) {
        efi_print_ascii("  Filesystem not available\r\n");
        return;
    }

    CHAR16 wpath[64];
    to_ucs2(MODELS_DIR, wpath, 64);

    EFI_FILE_PROTOCOL *dir = 0;
    EFI_STATUS st = root->Open(root, &dir, wpath, EFI_FILE_MODE_READ, 0);
    if (st != EFI_SUCCESS) {
        efi_print_ascii("  No models directory found (");
        efi_print_ascii(MODELS_DIR);
        efi_print_ascii(")\r\n");
        return;
    }

    efi_print_ascii("  Available models:\r\n");
    int count = 0;
    uint8_t buf[512];
    for (;;) {
        UINTN buf_size = sizeof(buf);
        st = dir->Read(dir, &buf_size, buf);
        if (st != EFI_SUCCESS || buf_size == 0) break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        if (!(info->Attribute & EFI_FILE_DIRECTORY)) continue;

        /* Convert filename to ASCII */
        char name[64];
        int i = 0;
        while (info->FileName[i] && i < 63) {
            name[i] = (char)(info->FileName[i] & 0xFF);
            i++;
        }
        name[i] = '\0';

        /* Skip . and .. */
        if (name[0] == '.') continue;

        efi_print_ascii("    ");
        efi_print_ascii(name);

        /* Show size of model.btw if present */
        char probe_path[128];
        int pi = str_copy(probe_path, MODELS_DIR "\\", 128);
        pi += str_copy(probe_path + pi, name, 128 - pi);
        str_copy(probe_path + pi, "\\model.btw", 128 - pi);

        CHAR16 wprobe[128];
        to_ucs2(probe_path, wprobe, 128);
        EFI_FILE_PROTOCOL *probe = 0;
        if (root->Open(root, &probe, wprobe, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS) {
            EFI_GUID fi_guid = EFI_FILE_INFO_GUID;
            uint8_t ibuf[256];
            UINTN isz = sizeof(ibuf);
            if (probe->GetInfo(probe, &fi_guid, &isz, ibuf) == EFI_SUCCESS) {
                EFI_FILE_INFO *fi = (EFI_FILE_INFO *)ibuf;
                char num[24];
                u64_to_str(num, fi->FileSize / (1024 * 1024));
                efi_print_ascii(" (");
                efi_print_ascii(num);
                efi_print_ascii(" MB)");
            }
            probe->Close(probe);
        }

        efi_print_ascii("\r\n");
        count++;
    }

    dir->Close(dir);

    if (count == 0) {
        efi_print_ascii("    (none)\r\n");
    }

    efi_print_ascii("\r\n  Usage: /load <name>\r\n");
}

/* Load a named model from \rwkvos\models\<name>\ */
static int load_model(const char *name) {
    if (!name || name[0] == '\0') {
        efi_print_ascii("  Usage: /load <name>\r\n");
        return 0;
    }

    if (efi_fs_init() != EFI_SUCCESS) {
        efi_print_ascii("  Filesystem not available\r\n");
        return 0;
    }

    /* Build paths */
    char btw_path[128], btv_path[128];
    int pi;

    pi = str_copy(btw_path, MODELS_DIR "\\", 128);
    pi += str_copy(btw_path + pi, name, 128 - pi);
    str_copy(btw_path + pi, "\\model.btw", 128 - pi);

    pi = str_copy(btv_path, MODELS_DIR "\\", 128);
    pi += str_copy(btv_path + pi, name, 128 - pi);
    str_copy(btv_path + pi, "\\vocab.btv", 128 - pi);

    /* Free current model and tokenizer */
    if (g_model) {
        ba_model_free(g_model);
        g_model = 0;
    }
    if (g_tok) {
        ba_tokenizer_free(g_tok);
        g_tok = 0;
    }

    /* Load new model */
    efi_print_ascii("  Loading model: ");
    efi_print_ascii(name);
    efi_print_ascii("...\r\n");

    g_model = ba_model_load(btw_path);
    if (!g_model) {
        efi_print_ascii("  ERROR: failed to load ");
        efi_print_ascii(btw_path);
        efi_print_ascii("\r\n");
        return 0;
    }

    ba_model_config *cfg = &g_model->config;
    char num[24];
    efi_print_ascii("  Model loaded: ");
    int_to_str(num, cfg->n_layers); efi_print_ascii(num); efi_print_ascii(" layers, ");
    int_to_str(num, cfg->n_embd); efi_print_ascii(num); efi_print_ascii(" hidden, ");
    int_to_str(num, cfg->vocab_size); efi_print_ascii(num); efi_print_ascii(" vocab");
    if (g_model->quant == 2) efi_print_ascii(" (Q8)");
    efi_print_ascii("\r\n");

    u64_to_str(num, g_model->raw_size / (1024 * 1024));
    efi_print_ascii("  Weights: "); efi_print_ascii(num); efi_print_ascii(" MB\r\n");

    /* Load tokenizer */
    g_tok = ba_tokenizer_load(btv_path);
    if (!g_tok) {
        efi_print_ascii("  ERROR: failed to load ");
        efi_print_ascii(btv_path);
        efi_print_ascii("\r\n");
        ba_model_free(g_model);
        g_model = 0;
        return 0;
    }
    efi_print_ascii("  Vocab loaded.\r\n");

    return 1;
}

/* Handle chat slash commands. Returns 1 if handled, 0 if not a command. */
static int handle_slash_command(const char *line) {
    if (line[0] != '/') return 0;

    if (str_eq(line, "/models")) {
        list_models();
        return 1;
    }

    if (str_starts_with(line, "/load ")) {
        const char *name = line + 6;
        while (*name == ' ') name++;
        if (*name == '\0') {
            efi_print_ascii("  Usage: /load <name>\r\n");
            return 1;
        }
        load_model(name);
        return 1;
    }

    if (str_eq(line, "/help")) {
        efi_print_ascii("  /models        List available models\r\n");
        efi_print_ascii("  /load <name>   Load a model by name\r\n");
        efi_print_ascii("  /help          Show this help\r\n");
        return 1;
    }

    return 0;
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
    if (!ensure_model_loaded()) {
        efi_print_ascii("  No model loaded. Use /models and /load <name>\r\n");
    }

    ba_sample_config cfg = ba_default_config();
    cfg.temperature = 0.7f;
    cfg.max_tokens = 512;

    int eos_id = 0, im_end_id = 0, vocab = 0;
    if (g_model && g_tok) {
        eos_id = ba_tokenizer_eos(g_tok);
        im_end_id = ba_tokenizer_im_end(g_tok);
        vocab = ba_model_vocab_size(g_model);
        ba_model_reset(g_model);
    }

    char prompt[1024];
    char chat_buf[2048];
    int tokens[2048];
    char decode_buf[64];
    char num[12];
    int first_turn = 1;
    float *logits = 0;

    efi_print_ascii("\r\n  Chat (empty line to exit, /help for commands)\r\n\r\n");

    for (;;) {
        efi_print_ascii("> ");
        int len = efi_read_line(prompt, sizeof(prompt));
        if (len <= 0) break;

        /* Handle slash commands (/models, /load, /help) */
        if (prompt[0] == '/') {
            if (handle_slash_command(prompt)) {
                /* After /load, refresh model-dependent state */
                if (g_model && g_tok) {
                    eos_id = ba_tokenizer_eos(g_tok);
                    im_end_id = ba_tokenizer_im_end(g_tok);
                    vocab = ba_model_vocab_size(g_model);
                    ba_model_reset(g_model);
                    first_turn = 1;
                    logits = 0;
                }
                continue;
            }
        }

        /* Need a loaded model for inference */
        if (!g_model || !g_tok) {
            efi_print_ascii("  No model loaded. Use /models and /load <name>\r\n");
            continue;
        }

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
        int recent_tokens[64];
        int n_recent = 0;
        for (int i = 0; i < cfg.max_tokens; i++) {
            int next = ba_model_sample(logits, vocab, &cfg, recent_tokens, n_recent);
            if (next == eos_id || next == im_end_id) break;

            /* Track token for repeat penalty */
            if (n_recent < 64)
                recent_tokens[n_recent++] = next;
            else
                recent_tokens[i % 64] = next;

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

/* Flag: set to 1 when executing a command from the management API.
 * Interactive/blocking commands check this and refuse to run. */
int g_api_context = 0;

/* Flag: set to 1 when reboot is requested via API (after response is sent) */
int g_reboot_pending = 0;

void efi_reboot(void) {
    efi_print_ascii("  Rebooting...\r\n");
    efi_log_flush();
    gST->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, 0);
    /* Should not return — halt if it does */
    for (;;) __asm__ volatile("hlt");
}

/*
 * ba_exec_cmd — Execute a single REPL command.
 * Returns 0 on success, -1 for "exit", -2 for blocked commands (interactive).
 */
int ba_exec_cmd(const char *line) {
    static char response[HTTP_RESPONSE_MAX];

    /* exit */
    if (str_eq(line, "exit")) {
        if (g_api_context) {
            efi_print_ascii("  exit acknowledged\r\n");
            return 0;  /* Don't actually exit from API */
        }
        return -1;
    }

    /* chat — multi-turn conversation (interactive — blocked from API) */
    if (str_eq(line, "chat") || str_eq(line, "infer")) {
        if (g_api_context) return -2;
        command_chat();
        return 0;
    }

    /* models — list available models */
    if (str_eq(line, "models")) {
        list_models();
        return 0;
    }

    /* load <name> — load a named model */
    if (str_starts_with(line, "load ")) {
        const char *name = line + 5;
        while (*name == ' ') name++;
        load_model(name);
        return 0;
    }

    /* ls [path] — list directory */
    if (str_eq(line, "ls") || str_starts_with(line, "ls ")) {
        if (efi_fs_init() != EFI_SUCCESS) {
            efi_print_ascii("  Filesystem not available\r\n");
            return 0;
        }
        const char *path = line[2] == ' ' ? line + 3 : "";
        while (*path == ' ') path++;
        efi_list_dir(path);
        return 0;
    }

    /* cat <file> — read and display file */
    if (str_starts_with(line, "cat ")) {
        const char *path = line + 4;
        while (*path == ' ') path++;
        if (*path == '\0') {
            efi_print_ascii("  Usage: cat <file>\r\n");
            return 0;
        }
        if (efi_fs_init() != EFI_SUCCESS) {
            efi_print_ascii("  Filesystem not available\r\n");
            return 0;
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
        return 0;
    }

    /* netinfo */
    if (str_eq(line, "netinfo")) {
        if (efi_net_available()) {
            efi_print_ascii("  Network: ready\r\n");
        } else {
            efi_print_ascii("  Network: unavailable\r\n");
        }
        return 0;
    }

    /* serve [port] — start HTTP server (interactive — blocked from API) */
    if (str_eq(line, "serve") || str_starts_with(line, "serve ")) {
        if (g_api_context) return -2;
        if (!efi_net_available()) {
            efi_print_ascii("  Network not available\r\n");
            return 0;
        }
        if (!ensure_model_loaded()) return 0;

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
        return 0;
    }

    /* fetch <url> */
    if (str_starts_with(line, "fetch ")) {
        const char *url = line + 6;
        /* Skip leading spaces */
        while (*url == ' ') url++;

        if (*url == '\0') {
            efi_print_ascii("  Usage: fetch <url>\r\n");
            return 0;
        }

        if (!efi_net_available()) {
            efi_print_ascii("  Network not available\r\n");
            return 0;
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
        return 0;
    }

    /* wifi — WiFi management commands */
    if (str_eq(line, "wifi")) {
        efi_wifi_status();
        return 0;
    }
    if (str_eq(line, "wifi scan")) {
        efi_wifi_scan();
        return 0;
    }
    if (str_eq(line, "wifi disconnect")) {
        efi_wifi_disconnect();
        return 0;
    }
    if (str_starts_with(line, "wifi save")) {
        const char *ssid = line + 9;
        while (*ssid == ' ') ssid++;
        if (*ssid == '\0') {
            efi_print_ascii("  Usage: wifi save <SSID> [password]\r\n");
            return 0;
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
        return 0;
    }
    if (str_starts_with(line, "wifi connect ")) {
        /* Interactive — blocked from API (prompts for password) */
        if (g_api_context) return -2;
        const char *ssid = line + 13;
        while (*ssid == ' ') ssid++;
        if (*ssid == '\0') {
            efi_print_ascii("  Usage: wifi connect <SSID>\r\n");
            return 0;
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
        return 0;
    }

    /* mdns — manual mDNS test (interactive — blocked from API) */
    if (str_eq(line, "mdns")) {
        if (g_api_context) return -2;
        if (!efi_net_available()) {
            efi_print_ascii("  Network not available\r\n");
            return 0;
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
        return 0;
    }

    /* e1000e poll — live frame capture (interactive — blocked from API) */
    if (str_eq(line, "e1000e poll")) {
        if (g_api_context) return -2;
        e1000e_poll_cmd();
        return 0;
    }

    /* e1000e arp — send gratuitous ARP broadcast */
    if (str_eq(line, "e1000e arp")) {
        e1000e_arp_cmd();
        return 0;
    }

    /* e1000e — PCI probe for Intel Ethernet */
    if (str_eq(line, "e1000e")) {
        e1000e_probe();
        return 0;
    }

    /* iwn — PCI probe for Intel WiFi */
    if (str_eq(line, "iwn")) {
        iwn_probe();
        return 0;
    }

    /* iwn scan — scan for WiFi networks (Phase 3) */
    if (str_eq(line, "iwn scan")) {
        iwn_scan_cmd();
        return 0;
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
        return 0;
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
        return 0;
    }

    /* iwn ping — ARP ping to test data path (Phase 4d) */
    if (str_eq(line, "iwn ping")) {
        iwn_ping_cmd();
        return 0;
    }

    /* meminfo — memory usage summary */
    if (str_eq(line, "meminfo")) {
        UINTN map_size = 0, map_key = 0, desc_size = 0;
        uint32_t desc_ver = 0;

        /* First call: get required buffer size */
        gBS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
        map_size += 2 * desc_size;

        void *map_buf = 0;
        if (gBS->AllocatePool(EfiLoaderData, map_size, &map_buf) != EFI_SUCCESS) {
            efi_print_ascii("  Failed to allocate buffer\r\n");
            return 0;
        }

        if (gBS->GetMemoryMap(&map_size, map_buf, &map_key, &desc_size, &desc_ver) != EFI_SUCCESS) {
            efi_print_ascii("  GetMemoryMap failed\r\n");
            gBS->FreePool(map_buf);
            return 0;
        }

        /* Sum pages by memory type */
        uint64_t pages[EfiMaxMemoryType];
        for (int i = 0; i < EfiMaxMemoryType; i++) pages[i] = 0;

        uint8_t *p = (uint8_t *)map_buf;
        uint8_t *end = p + map_size;
        while (p < end) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)p;
            if (d->Type < EfiMaxMemoryType)
                pages[d->Type] += d->NumberOfPages;
            p += desc_size;
        }
        gBS->FreePool(map_buf);

        char num[24];
        uint64_t total_mb = 0;
        for (int i = 0; i < EfiMaxMemoryType; i++)
            total_mb += (pages[i] * 4096) / (1024 * 1024);

        /* BootAI usage = LoaderCode + LoaderData */
        uint64_t bootai_kb = ((pages[EfiLoaderCode] + pages[EfiLoaderData]) * 4096) / 1024;

        /* Firmware usage = everything except Free, LoaderCode, LoaderData */
        uint64_t fw_mb = 0;
        for (int i = 0; i < EfiMaxMemoryType; i++) {
            if (i == EfiConventionalMemory || i == EfiLoaderCode || i == EfiLoaderData)
                continue;
            fw_mb += (pages[i] * 4096) / (1024 * 1024);
        }

        uint64_t free_mb = (pages[EfiConventionalMemory] * 4096) / (1024 * 1024);
        uint64_t pool_free_mb = ba_mem_pool_free() / (1024 * 1024);
        uint64_t pool_used_mb = ba_mem_pool_used() / (1024 * 1024);
        uint64_t usable_mb = free_mb + pool_free_mb;

        /* Clean user-facing output */
        efi_print_ascii("\r\n");
        efi_print_ascii("  Installed RAM:  ");
        u64_to_str(num, total_mb); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        efi_print_ascii("  Firmware:       ");
        u64_to_str(num, fw_mb); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        efi_print_ascii("  BootAI:         ");
        u64_to_str(num, bootai_kb); efi_print_ascii(num);
        efi_print_ascii(" KB\r\n");

        if (pool_used_mb > 0) {
            efi_print_ascii("  Model/Data:     ");
            u64_to_str(num, pool_used_mb); efi_print_ascii(num);
            efi_print_ascii(" MB\r\n");
        }

        efi_print_ascii("  ----\r\n");
        efi_print_ascii("  Available:      ");
        u64_to_str(num, usable_mb); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        if (ba_mem_pool_regions() > 0) {
            efi_print_ascii("    (includes ");
            u64_to_str(num, ba_mem_pool_free() / (1024 * 1024));
            efi_print_ascii(num);
            efi_print_ascii(" MB reclaimed from ");
            u64_to_str(num, ba_mem_pool_regions());
            efi_print_ascii(num);
            efi_print_ascii(" firmware region(s))\r\n");
        }

        efi_print_ascii("\r\n");
        return 0;
    }

    /* memmap — full memory map dump */
    if (str_eq(line, "memmap")) {
        UINTN map_size = 0, map_key = 0, desc_size = 0;
        uint32_t desc_ver = 0;
        gBS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
        map_size += 2 * desc_size;

        void *map_buf = 0;
        if (gBS->AllocatePool(EfiLoaderData, map_size, &map_buf) != EFI_SUCCESS) {
            efi_print_ascii("  Failed to allocate buffer\r\n");
            return 0;
        }
        if (gBS->GetMemoryMap(&map_size, map_buf, &map_key, &desc_size, &desc_ver) != EFI_SUCCESS) {
            efi_print_ascii("  GetMemoryMap failed\r\n");
            gBS->FreePool(map_buf);
            return 0;
        }

        static const char *type_names[] = {
            "Reserved  ", "LoaderCode", "LoaderData",
            "BS Code   ", "BS Data   ", "RT Code   ", "RT Data   ",
            "Free      ", "Unusable  ", "ACPI Recl ", "ACPI NVS  ",
            "MMIO      ", "MMIO Port ", "PAL Code  ", "Persistent"
        };

        /* Summary counters */
        uint64_t sum_free = 0, sum_bs_code = 0, sum_bs_data = 0;
        uint64_t sum_loader = 0, sum_rt = 0, sum_other = 0;
        int n_regions = 0;

        efi_print_ascii("\r\n  Type        PhysAddr         Size         Pages\r\n");
        efi_print_ascii("  -----------------------------------------------\r\n");

        uint8_t *p = (uint8_t *)map_buf;
        uint8_t *end = p + map_size;
        char num[24];
        char hex[20];

        while (p < end) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)p;
            uint64_t bytes = d->NumberOfPages * 4096;
            n_regions++;

            /* Accumulate summaries */
            switch (d->Type) {
            case EfiConventionalMemory: sum_free += bytes; break;
            case EfiBootServicesCode:   sum_bs_code += bytes; break;
            case EfiBootServicesData:   sum_bs_data += bytes; break;
            case EfiLoaderCode: case EfiLoaderData: sum_loader += bytes; break;
            case EfiRuntimeServicesCode: case EfiRuntimeServicesData: sum_rt += bytes; break;
            default: sum_other += bytes; break;
            }

            /* Only print regions >= 1 MB to keep output readable */
            if (bytes >= 1024 * 1024) {
                efi_print_ascii("  ");
                if (d->Type < 15)
                    efi_print_ascii(type_names[d->Type]);
                else {
                    efi_print_ascii("Type ");
                    u64_to_str(num, d->Type); efi_print_ascii(num);
                    efi_print_ascii("    ");
                }

                /* Print physical address as hex */
                efi_print_ascii("  0x");
                uint64_t addr = d->PhysicalStart;
                for (int i = 0; i < 12; i++) {
                    int nib = (int)((addr >> (44 - i * 4)) & 0xF);
                    hex[i] = nib < 10 ? '0' + nib : 'A' + nib - 10;
                }
                hex[12] = 0;
                efi_print_ascii(hex);

                /* Print size in MB */
                efi_print_ascii("  ");
                u64_to_str(num, bytes / (1024 * 1024));
                /* Right-align to 6 chars */
                int slen = 0; while (num[slen]) slen++;
                for (int i = 0; i < 6 - slen; i++) efi_print_ascii(" ");
                efi_print_ascii(num);
                efi_print_ascii(" MB\r\n");
            }
            p += desc_size;
        }
        gBS->FreePool(map_buf);

        efi_print_ascii("  -----------------------------------------------\r\n");
        efi_print_ascii("  Summary:\r\n");

        efi_print_ascii("    Free (Conventional):  ");
        u64_to_str(num, sum_free / (1024 * 1024)); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        efi_print_ascii("    BS Code:              ");
        u64_to_str(num, sum_bs_code / (1024 * 1024)); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        efi_print_ascii("    BS Data:              ");
        u64_to_str(num, sum_bs_data / (1024 * 1024)); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        efi_print_ascii("    Loader (BootAI):      ");
        u64_to_str(num, sum_loader / (1024 * 1024)); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        efi_print_ascii("    Runtime Services:     ");
        u64_to_str(num, sum_rt / (1024 * 1024)); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        efi_print_ascii("    Other (ACPI/MMIO/..): ");
        u64_to_str(num, sum_other / (1024 * 1024)); efi_print_ascii(num);
        efi_print_ascii(" MB\r\n");

        efi_print_ascii("    Total regions:        ");
        u64_to_str(num, n_regions); efi_print_ascii(num);
        efi_print_ascii("\r\n");

        /* AllocatePages test: how much can we actually allocate? */
        efi_print_ascii("\r\n  AllocatePages test (EfiLoaderData):\r\n");
        uint64_t total_allocable = 0;
        int alloc_count = 0;
        uint64_t test_addrs[64];
        UINTN test_pages[64];
        /* Try progressively smaller blocks */
        UINTN try_sizes[] = { 262144, 131072, 65536, 32768, 16384, 8192, 4096, 2048, 1024, 256, 0 };
        for (int s = 0; try_sizes[s] > 0 && alloc_count < 64; s++) {
            while (alloc_count < 64) {
                uint64_t phys = 0;
                EFI_STATUS st = gBS->AllocatePages(0, EfiLoaderData,
                                                    try_sizes[s], &phys);
                if (st != EFI_SUCCESS) break;
                test_addrs[alloc_count] = phys;
                test_pages[alloc_count] = try_sizes[s];
                total_allocable += try_sizes[s] * 4096;
                alloc_count++;
            }
        }
        /* Free everything */
        for (int i = 0; i < alloc_count; i++)
            gBS->FreePages(test_addrs[i], test_pages[i]);

        efi_print_ascii("    Allocable: ");
        u64_to_str(num, total_allocable / (1024 * 1024));
        efi_print_ascii(num);
        efi_print_ascii(" MB (");
        u64_to_str(num, alloc_count);
        efi_print_ascii(num);
        efi_print_ascii(" blocks)\r\n\r\n");

        return 0;
    }

    /* netdiag — enumerate network protocols */
    if (str_eq(line, "netdiag")) {
        efi_net_diag();
        return 0;
    }

    /* mpinfo — multiprocessor diagnostics */
    if (str_eq(line, "mpinfo")) {
        efi_print_ascii("  Multiprocessor info:\r\n");
        EFI_MP_SERVICES_PROTOCOL *mp = 0;
        EFI_STATUS mps = gBS->LocateProtocol(&gEfiMpServicesProtocolGuid, 0, (void **)&mp);
        if (mps != EFI_SUCCESS || !mp) {
            efi_print_ascii("  EFI_MP_SERVICES_PROTOCOL: not available\r\n");
            efi_print_ascii("  Fallback: raw SIPI required for multi-core\r\n");
            return 0;
        }
        efi_print_ascii("  EFI_MP_SERVICES_PROTOCOL: found\r\n");

        UINTN n_procs = 0, n_enabled = 0;
        mps = mp->GetNumberOfProcessors(mp, &n_procs, &n_enabled);
        if (mps == EFI_SUCCESS) {
            char num[12]; int ni;
            efi_print_ascii("  Processors: ");
            ni = 0; { UINTN v = n_procs; if (v == 0) { num[ni++] = '0'; } else { char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; } } num[ni] = '\0';
            efi_print_ascii(num);
            efi_print_ascii(" total, ");
            ni = 0; { UINTN v = n_enabled; if (v == 0) { num[ni++] = '0'; } else { char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; } } num[ni] = '\0';
            efi_print_ascii(num);
            efi_print_ascii(" enabled\r\n");

            /* List each processor */
            for (UINTN p = 0; p < n_procs && p < 16; p++) {
                EFI_PROCESSOR_INFORMATION info;
                if (mp->GetProcessorInfo(mp, p, &info) == EFI_SUCCESS) {
                    efi_print_ascii("    CPU ");
                    ni = 0; { UINTN v = p; if (v == 0) { num[ni++] = '0'; } else { char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; } } num[ni] = '\0';
                    efi_print_ascii(num);
                    efi_print_ascii(": APIC ");
                    ni = 0; { uint64_t v = info.ProcessorId; if (v == 0) { num[ni++] = '0'; } else { char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; } } num[ni] = '\0';
                    efi_print_ascii(num);
                    efi_print_ascii(info.StatusFlag & 1 ? " [BSP]" : " [AP]");
                    efi_print_ascii(info.StatusFlag & 2 ? " enabled" : " disabled");
                    efi_print_ascii(info.StatusFlag & 4 ? " healthy" : " unhealthy");
                    efi_print_ascii(" pkg=");
                    ni = 0; { uint32_t v = info.Location.Package; if (v == 0) { num[ni++] = '0'; } else { char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; } } num[ni] = '\0';
                    efi_print_ascii(num);
                    efi_print_ascii(" core=");
                    ni = 0; { uint32_t v = info.Location.Core; if (v == 0) { num[ni++] = '0'; } else { char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; } } num[ni] = '\0';
                    efi_print_ascii(num);
                    efi_print_ascii(" thread=");
                    ni = 0; { uint32_t v = info.Location.Thread; if (v == 0) { num[ni++] = '0'; } else { char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; } } num[ni] = '\0';
                    efi_print_ascii(num);
                    efi_print_ascii("\r\n");
                }
            }
        } else {
            efi_print_ascii("  GetNumberOfProcessors failed\r\n");
        }
        return 0;
    }

    /* netstat — network stack diagnostics */
    if (str_eq(line, "netstat")) {
        efi_print_ascii("  Network stack diagnostics:\r\n");
        /* e1000e TX/RX counters */
        if (e1000e_snp_is_installed()) {
            efi_print_ascii("  e1000e TX: ok=");
            char num[12]; int ni;
            uint32_t v = e1000e_tx_ok_count();
            if (v == 0) { efi_print_ascii("0"); }
            else { ni = 0; char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; num[ni] = '\0'; efi_print_ascii(num); }
            efi_print_ascii(", fail=");
            v = e1000e_tx_fail_count();
            if (v == 0) { efi_print_ascii("0"); }
            else { ni = 0; char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; num[ni] = '\0'; efi_print_ascii(num); }
            efi_print_ascii(", RX frames=");
            v = (uint32_t)ns_poll_rx_count();
            if (v == 0) { efi_print_ascii("0"); }
            else { ni = 0; char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; num[ni] = '\0'; efi_print_ascii(num); }
            efi_print_ascii("\r\n");
        }
        /* ARP table */
        ns_arp_dump();
        /* TCP state */
        ns_tcp_diag();
        return 0;
    }

    /* install — install to hard drive */
    if (str_eq(line, "install")) {
        ba_install_to_hd();
        return 0;
    }

    /* hdinfo — inspect hard drive contents */
    if (str_eq(line, "hdinfo")) {
        ba_hdinfo();
        return 0;
    }

    /* reboot — reset system */
    if (str_eq(line, "reboot")) {
        if (g_api_context) {
            efi_print_ascii("  Rebooting...\r\n");
            g_reboot_pending = 1;
            return 0;
        }
        efi_reboot();
        return 0;  /* unreachable */
    }

    /* Default: echo back */
    efi_print_ascii("  ");
    efi_print_line(line);
    return 0;
}

static void command_repl(void) {
    char line[512];

    efi_print_ascii("\r\n");
    efi_print_ascii("  Commands: chat, models, load <name>, serve [port],\r\n");
    efi_print_ascii("            ls [path], cat <file>, fetch <url>,\r\n");
    efi_print_ascii("            wifi [scan|connect|disconnect|save], mdns, iwn, netinfo,\r\n");
    efi_print_ascii("            e1000e [poll|arp], netdiag, meminfo, memmap, mpinfo, tensor, install,\r\n");
    efi_print_ascii("            hdinfo, reboot, exit\r\n");
    efi_print_ascii("\r\n");

    for (;;) {
        efi_log_flush();  /* persist log to bootai.log after each command */
        efi_print_ascii("> ");

        int len = efi_read_line(line, sizeof(line));

        if (len <= 0) continue;

        if (ba_exec_cmd(line) == -1) break;  /* "exit" */
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

    /* Immediate debug output — proves the binary loaded and is running */
    {
        CHAR16 msg[] = { 'B','o','o','t','A','I',' ','O','K','\r','\n', 0 };
        SystemTable->ConOut->OutputString(SystemTable->ConOut, msg);
    }

    /* Clear screen */
    efi_clear_screen();

    /* Clear CapsLock/NumLock toggle state (Dell BIOS starts with CapsLock on) */
    efi_keyboard_init();

    /* Initialize memory — reclaim mistyped BS Code/Data regions */
    ba_mem_init();

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

    /* Report reclaimed memory */
    if (ba_mem_pool_regions() > 0) {
        char num[24];
        efi_print_ascii("  Memory: reclaimed ");
        u64_to_str(num, ba_mem_pool_total() / (1024 * 1024));
        efi_print_ascii(num);
        efi_print_ascii(" MB from ");
        u64_to_str(num, ba_mem_pool_regions());
        efi_print_ascii(num);
        efi_print_ascii(" BS region(s)\r\n");
    }

    /* Load third-party UEFI drivers from \drivers\ */
    efi_load_drivers();

    /* Auto-probe wired Ethernet (before network init so DHCP can use it) */
    e1000e_probe();

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

#ifdef PROD
    /* PROD mode: skip menu, boot straight into chat */
    ensure_model_loaded();
    command_chat();
    efi_print_ascii("Goodbye.\r\n");
    for (;;) __asm__ volatile("hlt");
#else
    /* Boot straight into REPL */
    command_repl();
#endif
}
