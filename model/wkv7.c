/*
 * wkv7.c - WKV-7 kernel implementation
 *
 * Core recurrent kernel for RWKV-7 blocks using the Generalized Delta Rule.
 * Evolves a per-head state matrix [head_dim, head_dim] each timestep via
 * learned decay, key-value outer products, and bonus terms.
 */

#include "model.h"

/* TODO: Implement WKV-7 kernel
 *
 * - wkv7_forward(r, k, v, w, a, b, state, output, T, H, D)
 * - For each timestep t, for each head h:
 *   - state *= diag(w[t])           (decay)
 *   - state += (a[t] @ state) * k^T (delta rule)
 *   - state += v[t] @ k[t]^T       (update)
 *   - output[t] = r[t] @ state     (readout)
 * - FP32 accumulation for numerical stability
 */
