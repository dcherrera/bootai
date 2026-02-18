/*
 * loader.c - Load quantized model weights, auto-detect architecture
 *
 * Reads weight files from the EFI filesystem and populates the model.
 * Auto-detects Qwen2.5 vs RWKV-X from weight structure.
 * Supports Q4_K_M and Q8 quantization formats.
 */

#include "model.h"

/* TODO: Implement model loader
 *
 * - ba_model_load(path) -> ba_model*
 * - Read file header to detect format and architecture
 * - Auto-detect: Qwen2.5 ("model.layers") vs RWKV-X ("rwkv."/"moba.")
 * - Parse tensor metadata: name, shape, dtype, data offset
 * - Support Q4_K_M and Q8_0 quantization
 * - Allocate and populate model struct
 * - ba_model_free(model) to release memory
 */
