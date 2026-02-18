/*
 * rwkv_tokenizer.c - RWKV World tokenizer (trie-based)
 *
 * Trie-based tokenizer for the RWKV World vocabulary (65536 tokens).
 * Uses greedy longest-match traversal of a prebuilt trie structure
 * loaded from a vocab file. Supports full Unicode via UTF-8 byte input.
 */

#include "../model/model.h"

/* TODO: Implement RWKV World tokenizer
 *
 * - rwkv_tok_load(vocab_path) -> tokenizer state (trie)
 * - rwkv_tok_encode(text, tokens_out, max_tokens) -> n_tokens
 *   Greedy longest-match trie traversal on UTF-8 bytes
 * - rwkv_tok_decode(tokens, n_tokens, text_out, max_len) -> n_chars
 *   Map token IDs to byte sequences
 * - rwkv_tok_free(tokenizer) -> release trie memory
 */
