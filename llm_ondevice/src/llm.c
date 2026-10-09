/* llm.c - Inference for a Llama-2 (llama2.c) tiny model, ported to ESP32-S3.
 *
 * Derived from Andrej Karpathy's llama2.c run.c (MIT). Changes for on-device:
 *   - No file I/O / mmap. The checkpoint and tokenizer are embedded in flash
 *     (see model_blob.S) and memory-mapped by the MMU; weight pointers index
 *     straight into the flash-resident blob (zero RAM copy of the ~1 MB model).
 *   - RunState activation/KV-cache buffers are allocated in QSPI PSRAM
 *     (heap_caps_calloc MALLOC_CAP_SPIRAM), with a fallback to internal RAM.
 *   - generate() emits tokens through a callback and times itself with
 *     esp_timer; no printf/stdin/CLI.
 *   - exit()/fprintf error paths replaced by graceful failure returns.
 */
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "llm.h"

// ---- flash-embedded assets (model_blob.S) ---------------------------------
extern const unsigned char stories_bin[];
extern const unsigned char stories_bin_end[];
extern const unsigned char tok_bin[];
extern const unsigned char tok_bin_end[];

// ---------------------------------------------------------------------------
// Transformer model
typedef struct {
    int dim, hidden_dim, n_layers, n_heads, n_kv_heads, vocab_size, seq_len;
} Config;

typedef struct {
    float* token_embedding_table;
    float* rms_att_weight;
    float* rms_ffn_weight;
    float* wq; float* wk; float* wv; float* wo;
    float* w1; float* w2; float* w3;
    float* rms_final_weight;
    float* wcls;
} TransformerWeights;

typedef struct {
    float *x, *xb, *xb2, *hb, *hb2, *q, *k, *v, *att, *logits;
    float *key_cache, *value_cache;
} RunState;

typedef struct {
    Config config;
    TransformerWeights weights;
    RunState state;
} Transformer;

// PSRAM-preferred calloc, falling back to internal RAM.
static void* ps_calloc_safe(size_t n, size_t sz) {
    void* p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM);
    if (!p) p = calloc(n, sz);
    return p;
}

static int malloc_run_state(RunState* s, Config* p) {
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    s->x   = ps_calloc_safe(p->dim, sizeof(float));
    s->xb  = ps_calloc_safe(p->dim, sizeof(float));
    s->xb2 = ps_calloc_safe(p->dim, sizeof(float));
    s->hb  = ps_calloc_safe(p->hidden_dim, sizeof(float));
    s->hb2 = ps_calloc_safe(p->hidden_dim, sizeof(float));
    s->q   = ps_calloc_safe(p->dim, sizeof(float));
    s->key_cache   = ps_calloc_safe((size_t)p->n_layers * p->seq_len * kv_dim, sizeof(float));
    s->value_cache = ps_calloc_safe((size_t)p->n_layers * p->seq_len * kv_dim, sizeof(float));
    s->att    = ps_calloc_safe((size_t)p->n_heads * p->seq_len, sizeof(float));
    s->logits = ps_calloc_safe(p->vocab_size, sizeof(float));
    if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q
     || !s->key_cache || !s->value_cache || !s->att || !s->logits) return 0;
    return 1;
}

static void memory_map_weights(TransformerWeights* w, Config* p, float* ptr, int shared_weights) {
    int head_size = p->dim / p->n_heads;
    unsigned long long n_layers = p->n_layers;
    w->token_embedding_table = ptr; ptr += (size_t)p->vocab_size * p->dim;
    w->rms_att_weight = ptr;        ptr += n_layers * p->dim;
    w->wq = ptr;                    ptr += n_layers * p->dim * (p->n_heads * head_size);
    w->wk = ptr;                    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wv = ptr;                    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wo = ptr;                    ptr += n_layers * (p->n_heads * head_size) * p->dim;
    w->rms_ffn_weight = ptr;        ptr += n_layers * p->dim;
    w->w1 = ptr;                    ptr += n_layers * p->dim * p->hidden_dim;
    w->w2 = ptr;                    ptr += n_layers * p->hidden_dim * p->dim;
    w->w3 = ptr;                    ptr += n_layers * p->dim * p->hidden_dim;
    w->rms_final_weight = ptr;      ptr += p->dim;
    ptr += p->seq_len * head_size / 2;  // skip (old) freq_cis_real
    ptr += p->seq_len * head_size / 2;  // skip (old) freq_cis_imag
    w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

// Point the weights at the flash-embedded checkpoint (no file, no copy).
static int build_transformer(Transformer* t) {
    memcpy(&t->config, stories_bin, sizeof(Config));
    int shared_weights = t->config.vocab_size > 0 ? 1 : 0;
    t->config.vocab_size = abs(t->config.vocab_size);
    float* weights_ptr = (float*)(stories_bin + sizeof(Config));
    memory_map_weights(&t->weights, &t->config, weights_ptr, shared_weights);
    return malloc_run_state(&t->state, &t->config);
}

// ---------------------------------------------------------------------------
// neural net blocks
static void rmsnorm(float* o, float* x, float* weight, int size) {
    float ss = 0.0f;
    for (int j = 0; j < size; j++) ss += x[j] * x[j];
    ss /= size; ss += 1e-5f; ss = 1.0f / sqrtf(ss);
    for (int j = 0; j < size; j++) o[j] = weight[j] * (ss * x[j]);
}

static void softmax(float* x, int size) {
    float max_val = x[0];
    for (int i = 1; i < size; i++) if (x[i] > max_val) max_val = x[i];
    float sum = 0.0f;
    for (int i = 0; i < size; i++) { x[i] = expf(x[i] - max_val); sum += x[i]; }
    for (int i = 0; i < size; i++) x[i] /= sum;
}

static void matmul(float* xout, float* x, float* w, int n, int d) {
    for (int i = 0; i < d; i++) {
        float val = 0.0f;
        for (int j = 0; j < n; j++) val += w[i * n + j] * x[j];
        xout[i] = val;
    }
}

static float* forward(Transformer* transformer, int token, int pos) {
    Config* p = &transformer->config;
    TransformerWeights* w = &transformer->weights;
    RunState* s = &transformer->state;
    float* x = s->x;
    int dim = p->dim;
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul = p->n_heads / p->n_kv_heads;
    int hidden_dim = p->hidden_dim;
    int head_size = dim / p->n_heads;

    float* content_row = w->token_embedding_table + token * dim;
    memcpy(x, content_row, dim * sizeof(*x));

    for (unsigned long long l = 0; l < (unsigned)p->n_layers; l++) {
        rmsnorm(s->xb, x, w->rms_att_weight + l * dim, dim);
        int loff = l * p->seq_len * kv_dim;
        s->k = s->key_cache + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;
        matmul(s->q, s->xb, w->wq + l * dim * dim, dim, dim);
        matmul(s->k, s->xb, w->wk + l * dim * kv_dim, dim, kv_dim);
        matmul(s->v, s->xb, w->wv + l * dim * kv_dim, dim, kv_dim);
        for (int i = 0; i < dim; i += 2) {
            int head_dim = i % head_size;
            float freq = 1.0f / powf(10000.0f, head_dim / (float)head_size);
            float val = pos * freq;
            float fcr = cosf(val), fci = sinf(val);
            int rotn = i < kv_dim ? 2 : 1;
            for (int v = 0; v < rotn; v++) {
                float* vec = v == 0 ? s->q : s->k;
                float v0 = vec[i], v1 = vec[i + 1];
                vec[i]     = v0 * fcr - v1 * fci;
                vec[i + 1] = v0 * fci + v1 * fcr;
            }
        }
        for (int h = 0; h < p->n_heads; h++) {
            float* q = s->q + h * head_size;
            float* att = s->att + h * p->seq_len;
            for (int t = 0; t <= pos; t++) {
                float* k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                float score = 0.0f;
                for (int i = 0; i < head_size; i++) score += q[i] * k[i];
                score /= sqrtf(head_size);
                att[t] = score;
            }
            softmax(att, pos + 1);
            float* xb = s->xb + h * head_size;
            memset(xb, 0, head_size * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                float* v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                float a = att[t];
                for (int i = 0; i < head_size; i++) xb[i] += a * v[i];
            }
        }
        matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);
        for (int i = 0; i < dim; i++) x[i] += s->xb2[i];
        rmsnorm(s->xb, x, w->rms_ffn_weight + l * dim, dim);
        matmul(s->hb,  s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
        matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);
        for (int i = 0; i < hidden_dim; i++) {
            float val = s->hb[i];
            val *= (1.0f / (1.0f + expf(-val)));
            val *= s->hb2[i];
            s->hb[i] = val;
        }
        matmul(s->xb, s->hb, w->w2 + l * dim * hidden_dim, hidden_dim, dim);
        for (int i = 0; i < dim; i++) x[i] += s->xb[i];
    }
    rmsnorm(x, x, w->rms_final_weight, dim);
    matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
    return s->logits;
}

// ---------------------------------------------------------------------------
// Byte Pair Encoding tokenizer (built from the embedded tok_bin blob)
typedef struct { char* str; int id; } TokenIndex;
typedef struct {
    char** vocab;
    float* vocab_scores;
    TokenIndex* sorted_vocab;
    int vocab_size;
    unsigned int max_token_length;
    unsigned char byte_pieces[512];
} Tokenizer;

static int compare_tokens(const void* a, const void* b) {
    return strcmp(((TokenIndex*)a)->str, ((TokenIndex*)b)->str);
}

static int build_tokenizer(Tokenizer* t, const unsigned char* blob, int vocab_size) {
    t->vocab_size = vocab_size;
    t->vocab = (char**)malloc(vocab_size * sizeof(char*));
    t->vocab_scores = (float*)malloc(vocab_size * sizeof(float));
    t->sorted_vocab = NULL;
    if (!t->vocab || !t->vocab_scores) return 0;
    for (int i = 0; i < 256; i++) {
        t->byte_pieces[i * 2] = (unsigned char)i;
        t->byte_pieces[i * 2 + 1] = '\0';
    }
    const unsigned char* p = blob;
    memcpy(&t->max_token_length, p, sizeof(int)); p += sizeof(int);
    for (int i = 0; i < vocab_size; i++) {
        memcpy(t->vocab_scores + i, p, sizeof(float)); p += sizeof(float);
        int len; memcpy(&len, p, sizeof(int)); p += sizeof(int);
        t->vocab[i] = (char*)malloc(len + 1);
        if (!t->vocab[i]) return 0;
        memcpy(t->vocab[i], p, len); p += len;
        t->vocab[i][len] = '\0';
    }
    return 1;
}

static char* decode(Tokenizer* t, int prev_token, int token) {
    char* piece = t->vocab[token];
    if (prev_token == 1 && piece[0] == ' ') piece++;
    unsigned char byte_val;
    if (sscanf(piece, "<0x%02hhX>", &byte_val) == 1)
        piece = (char*)t->byte_pieces + byte_val * 2;
    return piece;
}

static int str_lookup(char* str, TokenIndex* sorted_vocab, int vocab_size) {
    TokenIndex tok; tok.str = str;
    TokenIndex* res = bsearch(&tok, sorted_vocab, vocab_size, sizeof(TokenIndex), compare_tokens);
    return res != NULL ? res->id : -1;
}

static void encode(Tokenizer* t, const char* text, int8_t bos, int8_t eos,
                   int* tokens, int* n_tokens) {
    if (t->sorted_vocab == NULL) {
        t->sorted_vocab = malloc(t->vocab_size * sizeof(TokenIndex));
        for (int i = 0; i < t->vocab_size; i++) {
            t->sorted_vocab[i].str = t->vocab[i];
            t->sorted_vocab[i].id = i;
        }
        qsort(t->sorted_vocab, t->vocab_size, sizeof(TokenIndex), compare_tokens);
    }
    char* str_buffer = malloc((t->max_token_length * 2 + 1 + 2) * sizeof(char));
    size_t str_len = 0;
    *n_tokens = 0;
    if (bos) tokens[(*n_tokens)++] = 1;
    if (text[0] != '\0') {
        int dummy_prefix = str_lookup((char*)" ", t->sorted_vocab, t->vocab_size);
        tokens[(*n_tokens)++] = dummy_prefix;
    }
    for (const char* c = text; *c != '\0'; c++) {
        if ((*c & 0xC0) != 0x80) str_len = 0;
        str_buffer[str_len++] = *c;
        str_buffer[str_len] = '\0';
        if ((*(c + 1) & 0xC0) == 0x80 && str_len < 4) continue;
        int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
        if (id != -1) {
            tokens[(*n_tokens)++] = id;
        } else {
            for (size_t i = 0; i < str_len; i++)
                tokens[(*n_tokens)++] = (unsigned char)str_buffer[i] + 3;
        }
        str_len = 0;
    }
    while (1) {
        float best_score = -1e10f;
        int best_id = -1, best_idx = -1;
        for (int i = 0; i < (*n_tokens - 1); i++) {
            sprintf(str_buffer, "%s%s", t->vocab[tokens[i]], t->vocab[tokens[i + 1]]);
            int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
            if (id != -1 && t->vocab_scores[id] > best_score) {
                best_score = t->vocab_scores[id]; best_id = id; best_idx = i;
            }
        }
        if (best_idx == -1) break;
        tokens[best_idx] = best_id;
        for (int i = best_idx + 1; i < (*n_tokens - 1); i++) tokens[i] = tokens[i + 1];
        (*n_tokens)--;
    }
    if (eos) tokens[(*n_tokens)++] = 2;
    free(str_buffer);
}

// ---------------------------------------------------------------------------
// Sampler
typedef struct { float prob; int index; } ProbIndex;
typedef struct {
    int vocab_size;
    ProbIndex* probindex;
    float temperature, topp;
    unsigned long long rng_state;
} Sampler;

static int sample_argmax(float* p, int n) {
    int mi = 0; float mp = p[0];
    for (int i = 1; i < n; i++) if (p[i] > mp) { mi = i; mp = p[i]; }
    return mi;
}
static int sample_mult(float* p, int n, float coin) {
    float cdf = 0.0f;
    for (int i = 0; i < n; i++) { cdf += p[i]; if (coin < cdf) return i; }
    return n - 1;
}
static int compare_prob(const void* a, const void* b) {
    ProbIndex* a_ = (ProbIndex*)a; ProbIndex* b_ = (ProbIndex*)b;
    if (a_->prob > b_->prob) return -1;
    if (a_->prob < b_->prob) return 1;
    return 0;
}
static int sample_topp(float* probabilities, int n, float topp, ProbIndex* probindex, float coin) {
    int n0 = 0;
    const float cutoff = (1.0f - topp) / (n - 1);
    for (int i = 0; i < n; i++)
        if (probabilities[i] >= cutoff) { probindex[n0].index = i; probindex[n0].prob = probabilities[i]; n0++; }
    qsort(probindex, n0, sizeof(ProbIndex), compare_prob);
    float cumulative_prob = 0.0f; int last_idx = n0 - 1;
    for (int i = 0; i < n0; i++) { cumulative_prob += probindex[i].prob; if (cumulative_prob > topp) { last_idx = i; break; } }
    float r = coin * cumulative_prob; float cdf = 0.0f;
    for (int i = 0; i <= last_idx; i++) { cdf += probindex[i].prob; if (r < cdf) return probindex[i].index; }
    return probindex[last_idx].index;
}
static unsigned int random_u32(unsigned long long* state) {
    *state ^= *state >> 12; *state ^= *state << 25; *state ^= *state >> 27;
    return (*state * 0x2545F4914F6CDD1Dull) >> 32;
}
static float random_f32(unsigned long long* state) {
    return (random_u32(state) >> 8) / 16777216.0f;
}
static int sample(Sampler* s, float* logits) {
    int next;
    if (s->temperature == 0.0f) {
        next = sample_argmax(logits, s->vocab_size);
    } else {
        for (int q = 0; q < s->vocab_size; q++) logits[q] /= s->temperature;
        softmax(logits, s->vocab_size);
        float coin = random_f32(&s->rng_state);
        if (s->topp <= 0 || s->topp >= 1) next = sample_mult(logits, s->vocab_size, coin);
        else next = sample_topp(logits, s->vocab_size, s->topp, s->probindex, coin);
    }
    return next;
}

// ---------------------------------------------------------------------------
// module state + public API
static Transformer g_t;
static Tokenizer   g_tok;
static Sampler     g_sampler;
static int         g_ready = 0;

int llm_setup(float temperature, float topp, unsigned long long rng_seed, const char** out_err) {
    if (temperature < 0.0f) temperature = 0.0f;
    if (topp < 0.0f || topp > 1.0f) topp = 0.9f;
    if (!build_transformer(&g_t)) { if (out_err) *out_err = "RunState alloc failed (PSRAM?)"; return 0; }
    if (!build_tokenizer(&g_tok, tok_bin, g_t.config.vocab_size)) { if (out_err) *out_err = "tokenizer alloc failed"; return 0; }
    g_sampler.vocab_size = g_t.config.vocab_size;
    g_sampler.temperature = temperature;
    g_sampler.topp = topp;
    g_sampler.rng_state = rng_seed ? rng_seed : 0xC0FFEEULL;
    g_sampler.probindex = malloc(g_sampler.vocab_size * sizeof(ProbIndex));
    if (!g_sampler.probindex) { if (out_err) *out_err = "sampler alloc failed"; return 0; }
    g_ready = 1;
    return 1;
}

int llm_generate(const char* prompt, int steps, llm_emit_cb cb, float* out_toks_per_sec) {
    if (!g_ready) return 0;
    if (prompt == NULL) prompt = "";
    if (steps <= 0 || steps > g_t.config.seq_len) steps = g_t.config.seq_len;

    int num_prompt_tokens = 0;
    int* prompt_tokens = (int*)malloc((strlen(prompt) + 3) * sizeof(int));
    if (!prompt_tokens) return 0;
    encode(&g_tok, prompt, 1, 0, prompt_tokens, &num_prompt_tokens);
    if (num_prompt_tokens < 1) { free(prompt_tokens); return 0; }

    int64_t start = 0;
    int next;
    int token = prompt_tokens[0];
    int pos = 0;
    while (pos < steps) {
        float* logits = forward(&g_t, token, pos);
        if (pos < num_prompt_tokens - 1) next = prompt_tokens[pos + 1];
        else next = sample(&g_sampler, logits);
        pos++;
        if (next == 1) break;  // BOS delimits; stop
        char* piece = decode(&g_tok, token, next);
        if (cb && piece && piece[0] != '\0') {
            // filter unprintable single bytes, like run.c's safe_printf
            if (!(piece[1] == '\0' && !(isprint((unsigned char)piece[0]) || isspace((unsigned char)piece[0]))))
                cb(piece);
        }
        token = next;
        if (start == 0) start = esp_timer_get_time();
    }
    if (out_toks_per_sec) {
        *out_toks_per_sec = 0.0f;
        if (pos > 1 && start > 0) {
            int64_t end = esp_timer_get_time();
            double secs = (double)(end - start) / 1e6;
            if (secs > 0) *out_toks_per_sec = (float)((pos - 1) / secs);
        }
    }
    free(prompt_tokens);
    return pos;
}

int  llm_dim(void)         { return g_t.config.dim; }
int  llm_n_layers(void)    { return g_t.config.n_layers; }
int  llm_vocab_size(void)  { return g_t.config.vocab_size; }
int  llm_seq_len(void)     { return g_t.config.seq_len; }
long llm_param_count(void) {
    return (long)((stories_bin_end - stories_bin) - (long)sizeof(Config)) / 4;
}
