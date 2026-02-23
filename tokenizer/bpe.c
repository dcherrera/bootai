/*
 * bpe.c - BPE tokenizer for Qwen3/Qwen2.5
 *
 * Loads binary vocab (.btv) exported by tools/export_weights.py.
 * BPE merge-based encoding, ID→string decode, special token handling.
 */

#include "../model/model.h"
#include "../efi/efi.h"

/* Freestanding libc */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* ---- Tokenizer state ---- */

struct ba_tokenizer {
    /* Token table: indexed by ID */
    uint32_t *offsets;    /* byte offset into string_blob */
    uint16_t *lengths;    /* byte length of each token */
    char     *string_blob; /* concatenated UTF-8 token bytes */
    int       n_tokens;

    /* Merge table: pairs in priority order */
    uint32_t *merge_a;    /* first token of pair */
    uint32_t *merge_b;    /* second token of pair */
    uint32_t *merge_result; /* result token of merge (pre-computed) */
    int       n_merges;

    /* Special tokens */
    int       im_start_id;  /* <|im_start|> */
    int       im_end_id;    /* <|im_end|> */
    int       eos_id;       /* <|endoftext|> */
    int       pad_id;

    /* Byte→token mapping: byte_tokens[b] = token ID for raw byte b */
    int       byte_tokens[256];

    /* Raw file data (owns memory) */
    void     *raw_data;
    UINTN     raw_size;
};

/* .btv file header */
typedef struct __attribute__((packed)) {
    char     magic[4];    /* "BTAV" */
    uint32_t n_tokens;
    uint32_t n_merges;
    uint32_t n_special;
    uint8_t  reserved[16];
} btv_header;

/* Global tokenizer instance */
static ba_tokenizer *g_tok = 0;

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

ba_tokenizer* ba_tokenizer_load(const char *path) {
    if (efi_fs_init() != EFI_SUCCESS) return 0;

    void *data = 0;
    UINTN file_size = 0;
    if (efi_read_file(path, &data, &file_size) != EFI_SUCCESS) return 0;

    UINTN file_pages = (file_size + 4095) / 4096;

    if (file_size < sizeof(btv_header)) {
        gBS->FreePages((uint64_t)(UINTN)data, file_pages);
        return 0;
    }

    btv_header *hdr = (btv_header *)data;
    if (hdr->magic[0] != 'B' || hdr->magic[1] != 'T' ||
        hdr->magic[2] != 'A' || hdr->magic[3] != 'V') {
        efi_print_ascii("  ERROR: invalid vocab magic\r\n");
        gBS->FreePages((uint64_t)(UINTN)data, file_pages);
        return 0;
    }

    ba_tokenizer *tok = (ba_tokenizer *)uefi_calloc(1, sizeof(ba_tokenizer));
    tok->raw_data = data;
    tok->raw_size = file_size;
    tok->n_tokens = (int)hdr->n_tokens;
    tok->n_merges = (int)hdr->n_merges;

    uint8_t *cursor = (uint8_t *)data + sizeof(btv_header);

    /* Token offsets: [n_tokens] uint32 */
    tok->offsets = (uint32_t *)cursor;
    cursor += tok->n_tokens * sizeof(uint32_t);

    /* Token lengths: [n_tokens] uint16 */
    tok->lengths = (uint16_t *)cursor;
    cursor += tok->n_tokens * sizeof(uint16_t);

    /* String blob: variable length */
    /* Compute total blob size from last offset + last length */
    uint32_t blob_end = 0;
    if (tok->n_tokens > 0) {
        blob_end = tok->offsets[tok->n_tokens - 1] + tok->lengths[tok->n_tokens - 1];
    }
    tok->string_blob = (char *)cursor;
    cursor += blob_end;

    /* Merge table: [n_merges] pairs of uint32 + pre-computed result IDs */
    tok->merge_a = (uint32_t *)cursor;
    cursor += tok->n_merges * sizeof(uint32_t);
    tok->merge_b = (uint32_t *)cursor;
    cursor += tok->n_merges * sizeof(uint32_t);
    tok->merge_result = (uint32_t *)cursor;
    cursor += tok->n_merges * sizeof(uint32_t);

    /* Special tokens */
    int n_special = (int)hdr->n_special;
    tok->im_start_id = -1;
    tok->im_end_id = -1;
    tok->eos_id = -1;
    tok->pad_id = -1;

    for (int i = 0; i < n_special; i++) {
        uint32_t id = *(uint32_t *)cursor; cursor += sizeof(uint32_t);
        uint16_t name_len = *(uint16_t *)cursor; cursor += sizeof(uint16_t);
        char *name = (char *)cursor; cursor += name_len;

        if (name_len == 12 && memcmp(name, "<|im_start|>", 12) == 0)
            tok->im_start_id = (int)id;
        else if (name_len == 10 && memcmp(name, "<|im_end|>", 10) == 0)
            tok->im_end_id = (int)id;
        else if (name_len == 13 && memcmp(name, "<|endoftext|>", 13) == 0)
            tok->eos_id = (int)id;
        else if (name_len == 3 && memcmp(name, "<s>", 3) == 0)
            tok->im_start_id = (int)id; /* BOS for Llama-family */
        else if (name_len == 4 && memcmp(name, "</s>", 4) == 0) {
            tok->im_end_id = (int)id;  /* EOS for Llama-family */
            if (tok->eos_id == -1) tok->eos_id = (int)id;
        }
    }

    /* Read byte_to_id[256] table (pre-computed GPT-2 byte mapping).
     * This maps each raw byte 0-255 to its token ID, handling the GPT-2
     * bytes_to_unicode() remapping of control characters and space. */
    uint32_t *byte_to_id = (uint32_t *)cursor;
    cursor += 256 * sizeof(uint32_t);
    for (int b = 0; b < 256; b++) {
        tok->byte_tokens[b] = (int)byte_to_id[b];
    }

    g_tok = tok;
    return tok;
}

/* Encode text to token IDs using BPE merge rules */
int ba_tokenizer_encode(ba_tokenizer *tok, const char *text,
                         int *tokens_out, int max_tokens) {
    if (!tok || !text) return 0;

    int text_len = str_len(text);
    if (text_len == 0) return 0;

    /* Start with one token per byte */
    int n = text_len;
    if (n > max_tokens) n = max_tokens;

    int *ids = (int *)uefi_malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        unsigned char b = (unsigned char)text[i];
        ids[i] = tok->byte_tokens[b];
        if (ids[i] == -1) ids[i] = 0; /* fallback to token 0 */
    }

    /* Iteratively apply merges in priority order.
     * merge_result[] is pre-computed by the converter — O(1) lookup per merge. */
    for (int m = 0; m < tok->n_merges && n > 1; m++) {
        int a = (int)tok->merge_a[m];
        int b = (int)tok->merge_b[m];
        int merged_id = (int)tok->merge_result[m];
        if (merged_id == 0 && (a != 0 || b != 0)) continue; /* unresolved merge */

        /* Apply this merge to the sequence */
        for (int i = 0; i < n - 1; i++) {
            if (ids[i] == a && ids[i + 1] == b) {
                ids[i] = merged_id;
                /* Shift remaining tokens left */
                for (int j = i + 1; j < n - 1; j++)
                    ids[j] = ids[j + 1];
                n--;
                /* Don't advance i — check for another merge at same position */
                i--;
            }
        }
    }

    /* Copy result */
    if (n > max_tokens) n = max_tokens;
    for (int i = 0; i < n; i++)
        tokens_out[i] = ids[i];

    uefi_free(ids);
    return n;
}

/* Decode token IDs to text */
int ba_tokenizer_decode(ba_tokenizer *tok, const int *tokens, int n_tokens,
                         char *text_out, int max_len) {
    if (!tok || !tokens || !text_out || max_len <= 0) return 0;

    int pos = 0;
    for (int i = 0; i < n_tokens; i++) {
        int id = tokens[i];
        if (id < 0 || id >= tok->n_tokens) continue;

        int len = tok->lengths[id];
        char *str = &tok->string_blob[tok->offsets[id]];

        for (int j = 0; j < len && pos < max_len - 1; j++) {
            text_out[pos++] = str[j];
        }
    }
    text_out[pos] = '\0';
    return pos;
}

/* Get special token IDs */
int ba_tokenizer_im_start(ba_tokenizer *tok) { return tok->im_start_id; }
int ba_tokenizer_im_end(ba_tokenizer *tok) { return tok->im_end_id; }
int ba_tokenizer_eos(ba_tokenizer *tok) { return tok->eos_id; }

void ba_tokenizer_free(ba_tokenizer *tok) {
    if (!tok) return;
    if (tok->raw_data) {
        UINTN pages = (tok->raw_size + 4095) / 4096;
        gBS->FreePages((uint64_t)(UINTN)tok->raw_data, pages);
    }
    uefi_free(tok);
    if (g_tok == tok) g_tok = 0;
}
