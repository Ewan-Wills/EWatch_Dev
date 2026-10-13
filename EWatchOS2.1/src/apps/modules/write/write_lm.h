// Write app: English context for the CNN (ml-write/lm.py is the reference).
//  - decode(): pick a character from the CNN probabilities, weighted by a
//    letter trigram prior given the word so far (letters) or a digit prior
//    (inside a number).
//  - correct(): when a word is finished, score every dictionary word of the
//    same length (top 20k English words, Norvig count_1w) against the stored
//    per-letter CNN log-probabilities plus word frequency; replace the typed
//    word only if a dictionary word wins by kMargin nats.
// Pure C++ (firmware, ewsim_host, host bench).
#pragma once
#include <stdint.h>

namespace wlm {

constexpr int kClasses = 36;   // wcnn class order: A-Z then 0-9
constexpr int kMaxWord = 24;

// logp: CNN log-probabilities (36). word: the current word so far, upper
// case letters/digits only, len chars. Returns the class index chosen.
int decode(const float *logp, const char *word, int len);

// typed: the decoded word (upper case), L letters; logp[i]: letter i's CNN
// log-probabilities. Writes the chosen word (upper case, L chars) to out.
// Returns true if it differs from typed.
bool correct(const char *typed, int L, const float (*logp)[kClasses], char *out);

// Dictionary lookup of an upper-case word: kWord if it is a listed word,
// kPrefix if a longer listed word starts with it; *logf = its log-frequency
// (relative to "the"; -1e9 if not listed). ~20k words, ~1 ms.
constexpr int kWord = 1, kPrefix = 2;
int wordStatus(const char *w, int L, float *logf = nullptr);

}  // namespace wlm
