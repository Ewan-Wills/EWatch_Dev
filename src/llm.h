// llm.h - tiny on-device LLM runtime (llama2.c port), C API.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Called once per generated token with the decoded text piece (NUL-terminated,
// already filtered for printable bytes). Implemented by the sketch to print to
// Serial and draw on the display.
typedef void (*llm_emit_cb)(const char* piece);

// Build the transformer, tokenizer and sampler from the flash-embedded blobs.
// Returns 1 on success, 0 on allocation failure. Fills *out_err on failure.
int  llm_setup(float temperature, float topp, unsigned long long rng_seed,
               const char** out_err);

// Generate up to `steps` tokens from `prompt` (may be ""), emitting each piece
// via cb. Returns the number of tokens produced; writes achieved tokens/sec
// (measured on-device) into *out_toks_per_sec.
int  llm_generate(const char* prompt, int steps, llm_emit_cb cb,
                  float* out_toks_per_sec);

// Model hyper-parameters, valid after llm_setup().
int  llm_dim(void);
int  llm_n_layers(void);
int  llm_vocab_size(void);
int  llm_seq_len(void);
long llm_param_count(void);

#ifdef __cplusplus
}
#endif
