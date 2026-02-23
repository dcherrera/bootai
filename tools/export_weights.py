#!/usr/bin/env python3
"""
export_weights.py — Convert HuggingFace model to flat binary .btw + .btv formats.

Exports Qwen3-0.6B (or any Qwen/Llama-style model) to BootAI's binary format:
  - .btw: 128-byte header + contiguous weights (f16 embed, Q8 layers, f32 norms)
  - .btv: binary vocab with token table, merge rules, and special tokens

Usage:
    python3 tools/export_weights.py --model Qwen/Qwen3-0.6B --quant q8 \
        --output esp/rwkvos/model.btw
"""

import argparse
import struct
import sys
import os
import numpy as np
from pathlib import Path

def quantize_q8(tensor: np.ndarray) -> tuple:
    """Quantize a 2D float tensor to INT8 with per-row symmetric quantization.
    Returns (int8_data, float32_scales)."""
    assert tensor.ndim == 2, f"Expected 2D tensor, got {tensor.ndim}D"
    rows, cols = tensor.shape

    # Per-row absolute max
    row_max = np.abs(tensor).max(axis=1)
    scales = row_max / 127.0
    scales = np.where(scales == 0, 1.0, scales)  # avoid div by zero

    # Quantize
    inv_scales = 127.0 / row_max
    inv_scales = np.where(row_max == 0, 0.0, inv_scales)
    q = np.clip(np.round(tensor * inv_scales[:, None]), -127, 127).astype(np.int8)

    return q, scales.astype(np.float32)


def float32_to_float16(tensor: np.ndarray) -> np.ndarray:
    """Convert float32 tensor to float16 bytes."""
    return tensor.astype(np.float16)


def detect_architecture(config) -> tuple:
    """Detect model architecture and return (arch_id, has_qk_norm, has_qkv_bias)."""
    model_type = config.get("model_type", "")

    # Qwen3: has qk_norm, no QKV bias
    if "qwen3" in model_type.lower() or config.get("head_dim", 0) == 64:
        return 0, True, False  # arch=qwen3, qk_norm=True, bias=False

    # Qwen2.5: no qk_norm, has QKV bias
    if "qwen2" in model_type.lower():
        return 1, False, True  # arch=qwen2, qk_norm=False, bias=True

    # Default: assume Qwen3-style
    return 0, True, False


def export_weights(model_name: str, quant: str, output_path: str):
    """Export model weights to .btw format."""
    try:
        from transformers import AutoModelForCausalLM, AutoConfig
        import torch
    except ImportError:
        print("ERROR: pip install transformers torch")
        sys.exit(1)

    print(f"Loading model: {model_name}")
    config = AutoConfig.from_pretrained(model_name)
    model = AutoModelForCausalLM.from_pretrained(model_name, torch_dtype=torch.float32)
    sd = model.state_dict()

    # Extract config
    cfg = config.to_dict()
    n_layers = cfg.get("num_hidden_layers", cfg.get("n_layer", 28))
    n_embd = cfg.get("hidden_size", cfg.get("n_embd", 1024))
    n_heads = cfg.get("num_attention_heads", cfg.get("n_head", 16))
    n_kv_heads = cfg.get("num_key_value_heads", n_heads)
    head_dim = cfg.get("head_dim", n_embd // n_heads)
    ffn_dim = cfg.get("intermediate_size", cfg.get("ffn_dim", 2816))
    vocab_size = cfg.get("vocab_size", 151936)
    rope_theta = float(cfg.get("rope_theta", 1000000.0))
    rms_eps = float(cfg.get("rms_norm_eps", 1e-6))
    max_seq_len = cfg.get("max_position_embeddings", 40960)

    arch_id, has_qk_norm, has_qkv_bias = detect_architecture(cfg)

    print(f"Architecture: {'Qwen3' if arch_id == 0 else 'Qwen2'}")
    print(f"  Layers: {n_layers}, Hidden: {n_embd}, Heads: {n_heads}/{n_kv_heads}")
    print(f"  FFN: {ffn_dim}, Vocab: {vocab_size}, Head dim: {head_dim}")
    print(f"  QK-Norm: {has_qk_norm}, QKV-Bias: {has_qkv_bias}")

    q_dim = n_heads * head_dim
    kv_dim = n_kv_heads * head_dim

    # Build header (128 bytes)
    header = struct.pack(
        "<4sIIIIIIIIIIfIfII64s",
        b"BTAI",           # magic
        1,                 # version
        arch_id,           # arch (0=qwen3, 1=qwen2)
        2,                 # quant (2=q8)
        n_layers,
        n_embd,
        n_heads,
        n_kv_heads,
        head_dim,
        ffn_dim,
        vocab_size,
        rope_theta,
        rms_eps,
        max_seq_len,
        1 if has_qk_norm else 0,
        1 if has_qkv_bias else 0,
        b"\x00" * 64,     # reserved
    )
    assert len(header) == 128, f"Header size: {len(header)} (expected 128)"

    # Find weight names (HuggingFace naming)
    def get_weight(name):
        """Get weight tensor as numpy array."""
        if name in sd:
            return sd[name].float().numpy()
        # Try with model. prefix
        if f"model.{name}" in sd:
            return sd[f"model.{name}"].float().numpy()
        return None

    # Detect naming convention
    prefix = ""
    if "model.embed_tokens.weight" in sd:
        prefix = "model."

    print(f"Writing to: {output_path}")
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

    with open(output_path, "wb") as f:
        f.write(header)
        total_bytes = 128

        # Embedding: [vocab, n_embd] f16
        embed_w = sd[f"{prefix}embed_tokens.weight"].float().numpy()
        embed_f16 = embed_w.astype(np.float16)
        f.write(embed_f16.tobytes())
        total_bytes += embed_f16.nbytes
        print(f"  embed_tokens: {embed_w.shape} -> f16 ({embed_f16.nbytes // 1024 // 1024} MB)")

        # Per-layer weights
        for i in range(n_layers):
            lp = f"{prefix}layers.{i}."

            # input_layernorm.weight: [n_embd] f32
            ln1 = sd[f"{lp}input_layernorm.weight"].float().numpy()
            f.write(ln1.tobytes())
            total_bytes += ln1.nbytes

            # Q projection: [q_dim, n_embd] Q8
            q_w = sd[f"{lp}self_attn.q_proj.weight"].float().numpy()
            q_q8, q_scales = quantize_q8(q_w)
            f.write(q_q8.tobytes())
            f.write(q_scales.tobytes())
            total_bytes += q_q8.nbytes + q_scales.nbytes

            # K projection: [kv_dim, n_embd] Q8
            k_w = sd[f"{lp}self_attn.k_proj.weight"].float().numpy()
            k_q8, k_scales = quantize_q8(k_w)
            f.write(k_q8.tobytes())
            f.write(k_scales.tobytes())
            total_bytes += k_q8.nbytes + k_scales.nbytes

            # V projection: [kv_dim, n_embd] Q8
            v_w = sd[f"{lp}self_attn.v_proj.weight"].float().numpy()
            v_q8, v_scales = quantize_q8(v_w)
            f.write(v_q8.tobytes())
            f.write(v_scales.tobytes())
            total_bytes += v_q8.nbytes + v_scales.nbytes

            # O projection: [n_embd, q_dim] Q8
            o_w = sd[f"{lp}self_attn.o_proj.weight"].float().numpy()
            o_q8, o_scales = quantize_q8(o_w)
            f.write(o_q8.tobytes())
            f.write(o_scales.tobytes())
            total_bytes += o_q8.nbytes + o_scales.nbytes

            # QK-Norm (if present)
            if has_qk_norm:
                q_norm_key = f"{lp}self_attn.q_norm.weight"
                k_norm_key = f"{lp}self_attn.k_norm.weight"
                if q_norm_key in sd:
                    q_norm = sd[q_norm_key].float().numpy()
                    k_norm = sd[k_norm_key].float().numpy()
                else:
                    q_norm = np.ones(head_dim, dtype=np.float32)
                    k_norm = np.ones(head_dim, dtype=np.float32)
                f.write(q_norm.tobytes())
                f.write(k_norm.tobytes())
                total_bytes += q_norm.nbytes + k_norm.nbytes

            # QKV biases (if present)
            if has_qkv_bias:
                q_bias_key = f"{lp}self_attn.q_proj.bias"
                if q_bias_key in sd:
                    f.write(sd[q_bias_key].float().numpy().tobytes())
                    f.write(sd[f"{lp}self_attn.k_proj.bias"].float().numpy().tobytes())
                    f.write(sd[f"{lp}self_attn.v_proj.bias"].float().numpy().tobytes())
                    total_bytes += q_dim * 4 + kv_dim * 4 + kv_dim * 4
                else:
                    # Bias declared but not present — write zeros
                    f.write(np.zeros(q_dim, dtype=np.float32).tobytes())
                    f.write(np.zeros(kv_dim, dtype=np.float32).tobytes())
                    f.write(np.zeros(kv_dim, dtype=np.float32).tobytes())
                    total_bytes += q_dim * 4 + kv_dim * 4 + kv_dim * 4

            # post_attention_layernorm.weight: [n_embd] f32
            ln2 = sd[f"{lp}post_attention_layernorm.weight"].float().numpy()
            f.write(ln2.tobytes())
            total_bytes += ln2.nbytes

            # gate_proj: [ffn_dim, n_embd] Q8
            gate_w = sd[f"{lp}mlp.gate_proj.weight"].float().numpy()
            gate_q8, gate_scales = quantize_q8(gate_w)
            f.write(gate_q8.tobytes())
            f.write(gate_scales.tobytes())
            total_bytes += gate_q8.nbytes + gate_scales.nbytes

            # up_proj: [ffn_dim, n_embd] Q8
            up_w = sd[f"{lp}mlp.up_proj.weight"].float().numpy()
            up_q8, up_scales = quantize_q8(up_w)
            f.write(up_q8.tobytes())
            f.write(up_scales.tobytes())
            total_bytes += up_q8.nbytes + up_scales.nbytes

            # down_proj: [n_embd, ffn_dim] Q8
            down_w = sd[f"{lp}mlp.down_proj.weight"].float().numpy()
            down_q8, down_scales = quantize_q8(down_w)
            f.write(down_q8.tobytes())
            f.write(down_scales.tobytes())
            total_bytes += down_q8.nbytes + down_scales.nbytes

            if (i + 1) % 4 == 0 or i == n_layers - 1:
                print(f"  Layer {i+1}/{n_layers} done ({total_bytes // 1024 // 1024} MB)")

        # Final norm: [n_embd] f32
        ln_out = sd[f"{prefix}norm.weight"].float().numpy()
        f.write(ln_out.tobytes())
        total_bytes += ln_out.nbytes

        # LM head: [vocab, n_embd] f16
        # Check if weights are tied
        lm_head_key = "lm_head.weight"
        if lm_head_key in sd:
            lm_head = sd[lm_head_key].float().numpy()
            if not np.array_equal(lm_head, embed_w):
                lm_head_f16 = lm_head.astype(np.float16)
                f.write(lm_head_f16.tobytes())
                total_bytes += lm_head_f16.nbytes
                print(f"  lm_head: separate ({lm_head_f16.nbytes // 1024 // 1024} MB)")
            else:
                print(f"  lm_head: tied with embed_tokens (shared)")
        else:
            print(f"  lm_head: tied with embed_tokens (shared)")

    print(f"\nDone! Total size: {total_bytes / 1024 / 1024:.1f} MB")
    print(f"Output: {output_path}")

    # Also export vocab
    vocab_path = output_path.replace(".btw", ".btv")
    export_vocab(model_name, vocab_path)


def export_vocab(model_name: str, output_path: str):
    """Export tokenizer vocabulary to .btv binary format."""
    try:
        from transformers import AutoTokenizer
    except ImportError:
        print("ERROR: pip install transformers")
        sys.exit(1)

    print(f"\nExporting vocab to: {output_path}")
    tokenizer = AutoTokenizer.from_pretrained(model_name, trust_remote_code=True)

    vocab = tokenizer.get_vocab()
    n_tokens = len(vocab)

    # Build token table: sorted by ID
    id_to_token = {}
    for token_str, token_id in vocab.items():
        id_to_token[token_id] = token_str

    # Build GPT-2 unicode → raw byte reverse mapping.
    # GPT-2 bytes_to_unicode() maps non-printable bytes (0-32, 127-160, 173)
    # to Unicode 256+n. We reverse this so token strings are stored as the
    # original raw bytes — e.g. " the" instead of "Ġthe".
    unicode_to_byte = {}
    for byte_val in range(256):
        if 33 <= byte_val <= 126 or 161 <= byte_val <= 172 or 174 <= byte_val <= 255:
            unicode_to_byte[chr(byte_val)] = byte_val
        else:
            unicode_to_byte[chr(256 + byte_val)] = byte_val

    token_bytes = []
    for i in range(n_tokens):
        if i in id_to_token:
            s = id_to_token[i]
            try:
                b = bytes([unicode_to_byte[ch] for ch in s])
            except KeyError:
                b = s.encode("utf-8", errors="replace")
        else:
            b = b""
        token_bytes.append(b)

    # Build string blob
    blob = b""
    offsets = []
    lengths = []
    for b in token_bytes:
        offsets.append(len(blob))
        lengths.append(len(b))
        blob += b

    # Get merge rules
    merges = []
    if hasattr(tokenizer, "bpe_ranks"):
        # tiktoken-style (Qwen3)
        for pair, rank in sorted(tokenizer.bpe_ranks.items(), key=lambda x: x[1]):
            a_str, b_str = pair
            a_id = vocab.get(a_str, -1)
            b_id = vocab.get(b_str, -1)
            if a_id >= 0 and b_id >= 0:
                result_id = vocab.get(a_str + b_str, 0)
                merges.append((a_id, b_id, result_id))
    elif hasattr(tokenizer, "backend_tokenizer"):
        # HF tokenizers (GPT2TokenizerFast, Qwen2TokenizerFast, etc.)
        import json
        bt = tokenizer.backend_tokenizer
        data = json.loads(bt.to_str())
        hf_merges = data.get("model", {}).get("merges", [])
        for pair in hf_merges:
            a_str, b_str = pair[0], pair[1]
            a_id = vocab.get(a_str, -1)
            b_id = vocab.get(b_str, -1)
            if a_id >= 0 and b_id >= 0:
                result_id = vocab.get(a_str + b_str, 0)
                merges.append((a_id, b_id, result_id))

    # Special tokens
    special_tokens = {}
    for name in ["bos_token", "eos_token", "pad_token"]:
        tok_val = getattr(tokenizer, name, None)
        if tok_val and tok_val in vocab:
            special_tokens[tok_val] = vocab[tok_val]

    # Add chat template tokens
    for name in ["<|im_start|>", "<|im_end|>", "<|endoftext|>"]:
        if name in vocab:
            special_tokens[name] = vocab[name]

    n_merges = len(merges)
    n_special = len(special_tokens)

    print(f"  Tokens: {n_tokens}, Merges: {n_merges}, Special: {n_special}")

    with open(output_path, "wb") as f:
        # Header (32 bytes)
        header = struct.pack("<4sIII16s",
            b"BTAV",
            n_tokens,
            n_merges,
            n_special,
            b"\x00" * 16,
        )
        f.write(header)

        # Offsets: [n_tokens] uint32
        f.write(struct.pack(f"<{n_tokens}I", *offsets))

        # Lengths: [n_tokens] uint16
        f.write(struct.pack(f"<{n_tokens}H", *lengths))

        # String blob
        f.write(blob)

        # Merge table: merge_a[n_merges] then merge_b[n_merges] then merge_result[n_merges]
        if merges:
            merge_a = [m[0] for m in merges]
            merge_b = [m[1] for m in merges]
            merge_result = [m[2] for m in merges]
            f.write(struct.pack(f"<{n_merges}I", *merge_a))
            f.write(struct.pack(f"<{n_merges}I", *merge_b))
            f.write(struct.pack(f"<{n_merges}I", *merge_result))

        # Special tokens: {uint32 id, uint16 name_len, char name[]}
        for name, tid in special_tokens.items():
            name_bytes = name.encode("utf-8")
            f.write(struct.pack("<IH", tid, len(name_bytes)))
            f.write(name_bytes)

        # Byte-to-token mapping: [256] uint32
        # GPT-2 bytes_to_unicode() maps each byte to a Unicode char used in the vocab.
        # Pre-compute byte -> token ID for the tokenizer's encode path.
        byte_to_id = []
        for b in range(256):
            if 33 <= b <= 126 or 161 <= b <= 172 or 174 <= b <= 255:
                ch = chr(b)
            else:
                ch = chr(256 + b)
            tid = vocab.get(ch, 0)
            byte_to_id.append(tid)
        f.write(struct.pack("<256I", *byte_to_id))

    total_size = os.path.getsize(output_path)
    print(f"  Vocab size: {total_size / 1024:.1f} KB")


def main():
    parser = argparse.ArgumentParser(description="Export HuggingFace model to BootAI binary format")
    parser.add_argument("--model", required=True, help="HuggingFace model name (e.g. Qwen/Qwen3-0.6B)")
    parser.add_argument("--quant", default="q8", choices=["q8", "f16"],
                        help="Quantization format (default: q8)")
    parser.add_argument("--output", required=True, help="Output .btw file path")
    parser.add_argument("--vocab-only", action="store_true", help="Export only vocab (.btv)")
    args = parser.parse_args()

    if args.vocab_only:
        vocab_path = args.output.replace(".btw", ".btv")
        export_vocab(args.model, vocab_path)
    else:
        export_weights(args.model, args.quant, args.output)


if __name__ == "__main__":
    main()
