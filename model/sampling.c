/*
 * sampling.c - Temperature, top-k, top-p sampling
 *
 * Implements token sampling strategies for autoregressive generation.
 * Operates on raw logit vectors from the model forward pass.
 * All math is bare-metal (no stdlib dependency).
 */

#include "model.h"

/* TODO: Implement sampling
 *
 * - ba_model_sample(logits, vocab_size, config) -> token_id
 * - Temperature scaling: logits[i] /= temperature
 * - Top-k: keep k highest logits, rest to -inf
 * - Top-p: sort by prob, keep cumulative <= p
 * - Softmax -> weighted random selection
 * - Greedy: argmax when temperature == 0
 */
