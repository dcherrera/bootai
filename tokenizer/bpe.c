/*
 * bpe.c - BPE tokenizer for Qwen2.5
 *
 * Byte-Pair Encoding tokenizer compatible with the Qwen2.5 vocabulary.
 * Loads merge rules from a vocab file and encodes/decodes text to/from
 * token IDs. Supports the full 152064-token Qwen2.5 vocabulary.
 */

#include "../model/model.h"

/* TODO: Implement BPE tokenizer
 *
 * - bpe_load(vocab_path) -> tokenizer state
 * - bpe_encode(text, tokens_out, max_tokens) -> n_tokens
 *   Apply merge rules iteratively to produce token IDs
 * - bpe_decode(tokens, n_tokens, text_out, max_len) -> n_chars
 *   Map token IDs back to UTF-8 byte sequences
 * - bpe_free(tokenizer) -> release vocabulary tables
 * - Handle special tokens: <|im_start|>, <|im_end|>, etc.
 */
