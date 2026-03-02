# BootAI Models List

Target: 8GB RAM, CPU inference, Q4/Q8 quantization.

## Chat / Instruction

| Model | Params | Q4 RAM | Rec. RAM | Why | License | Downloaded | Converted | Tested |
|---|---|---|---|---|---|---|---|---|
| Qwen3-8B | 8B | ~5.5 GB | 8 GB | Best quality chat + tools under 8B. Matches Qwen2.5-14B. | Apache 2.0 | | | |
| Qwen3-4B | 4B | ~3 GB | 4 GB | Best value. MATH-500 97%. Native tool calling. Matches Qwen2.5-7B. | Apache 2.0 | | | |
| Qwen3-1.7B | 1.7B | ~1.5 GB | 4 GB | Matches Qwen2.5-3B. Good lightweight option. | Apache 2.0 | | | |
| Qwen3-0.6B | 0.6B | ~0.5 GB | 2 GB | Smallest viable chat model. Instant load. | Apache 2.0 | | | |
| Ministral 3-8B Instruct | 8B | ~5.5 GB | 8 GB | Beats Llama 3.1 8B. Native image understanding. | Apache 2.0 | | | |
| Ministral 3-3B Instruct | 3B | ~2.2 GB | 4 GB | Beats Gemma 2 2B and Llama 3.2 3B. | Apache 2.0 | | | |
| Phi-4-mini-instruct | 3.8B | ~4.5 GB | 8 GB | Strong instruction following. 128K context. | MIT | | | |
| Gemma 3 4B-it | 4B | ~2.6 GB | 4 GB | Comparable to Gemma 2 9B. Multimodal at 4B+. | Google custom | | | |
| SmolLM2-135M-Instruct | 135M | ~200 MB | 2 GB | Current BootAI model. Basic chat. | Apache 2.0 | Yes | Yes | Yes |
| SmolLM2-1.7B-Instruct | 1.7B | ~1.2 GB | 4 GB | Outclassed by Qwen3-1.7B but solid. | Apache 2.0 | | | |
| Llama 3.2 3B | 3B | ~2.2 GB | 4 GB | Meta pedigree. Outclassed by Qwen3-4B. | Llama Community | | | |

## Code

| Model | Params | Q4 RAM | Rec. RAM | Why | License | Downloaded | Converted | Tested |
|---|---|---|---|---|---|---|---|---|
| Qwen2.5-Coder-7B-Instruct | 7B | ~4.5 GB | 8 GB | 88.4% HumanEval. Best code model under 8B. 92+ languages. | Apache 2.0 | | | |
| Qwen2.5-Coder-3B-Instruct | 3B | ~2.2 GB | 4 GB | Best code model under 4B. | Apache 2.0 | | | |
| StarCoder2-3B | 3B | ~2 GB | 4 GB | Purpose-built for code. Fill-in-the-middle. | OpenRAIL-M | | | |

## Reasoning / Math

| Model | Params | Q4 RAM | Rec. RAM | Why | License | Downloaded | Converted | Tested |
|---|---|---|---|---|---|---|---|---|
| Phi-4-mini-reasoning | 3.8B | ~4.5 GB | 8 GB | 52% AIME 2024. Best reasoning under 4B. Chain-of-thought. | MIT | | | |
| Ministral 3-8B Reasoning | 8B | ~5.5 GB | 8 GB | 78.7% AIME 2025. Best reasoning at 8B. | Apache 2.0 | | | |
| Ministral 3-3B Reasoning | 3B | ~2.2 GB | 4 GB | 72% AIME 2025. Smallest reasoning model worth using. | Apache 2.0 | | | |
| DeepSeek-R1-Distill-Qwen-7B | 7.6B | ~5 GB | 8 GB | Distilled from 671B. MATH-500 92.8%. Verbose output. | MIT | | | |
| DeepSeek-R1-Distill-Qwen-1.5B | 1.5B | ~1.2 GB | 4 GB | MATH-500 83.9% from 1.5B. Impressive for size. | MIT | | | |

## Tool Use / Function Calling

Best tool calling models (not separate models — these are the same models from above ranked by tool use reliability):

1. **Qwen3-8B / Qwen3-4B** — native parallel tool calls, designed for agentic workflows
2. **Ministral 3-8B Instruct** — native function calling without special prompting
3. **Phi-4-mini-instruct** — built-in function calling, 128K context
4. **Avoid DeepSeek-R1-Distill** for tool use — verbose chain-of-thought interferes with clean JSON

## Notes

- **Q4 RAM** = approximate memory for Q4_K_M quantized weights only.
- **Rec. RAM** = recommended total system RAM (weights + KV cache + activations + UEFI overhead).
- **Qwen3-4B is the recommended primary model** — 3GB at Q4 leaves 5GB for everything else.
- All Qwen3 models support thinking/non-thinking modes (thinking = chain-of-thought reasoning).
- No Qwen3-Coder exists under 8B — Qwen2.5-Coder-7B remains best for dedicated code.
