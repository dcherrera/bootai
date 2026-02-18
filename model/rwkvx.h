/*
 * rwkvx.h - RWKV-X model configuration constants
 *
 * Architecture parameters for the RWKV-X hybrid model.
 * Interleaves RWKV-7 recurrent blocks (~75%) with sparse attention (~25%).
 */

#ifndef BOOTAI_RWKVX_H
#define BOOTAI_RWKVX_H

/* RWKV-X 0.2B configuration */
#define RWKVX_02B_LAYERS      12
#define RWKVX_02B_HIDDEN      768
#define RWKVX_02B_HEADS       12
#define RWKVX_02B_HEAD_DIM    64
#define RWKVX_02B_FFN         2048
#define RWKVX_02B_VOCAB       65536

/* RWKV-X 3.6B configuration */
#define RWKVX_36B_LAYERS      32
#define RWKVX_36B_HIDDEN      2560
#define RWKVX_36B_HEADS       40
#define RWKVX_36B_HEAD_DIM    64
#define RWKVX_36B_FFN         6912
#define RWKVX_36B_VOCAB       65536

/* Hybrid architecture ratios */
#define RWKVX_RWKV7_RATIO     0.75f
#define RWKVX_ATTN_RATIO      0.25f

#endif /* BOOTAI_RWKVX_H */
