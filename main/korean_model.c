#include "korean_model.h"
#include <string.h>
#include <limits.h>

static uint32_t random_next(ko_model_t *m) {
    m->rng ^= m->rng << 13;
    m->rng ^= m->rng >> 17;
    m->rng ^= m->rng << 5;
    return m->rng;
}
static unsigned bit_count(uint64_t mask) {
    unsigned n = 0;
    while (mask) { n += mask & 1u; mask >>= 1; }
    return n;
}
unsigned ko_mistake_count(const ko_progress_t *p) { return bit_count(p->mistakes); }
unsigned ko_seen_count(const ko_progress_t *p) { return bit_count(p->seen); }
void ko_init(ko_model_t *m, uint32_t seed) {
    memset(m, 0, sizeof(*m));
    m->rng = seed ? seed : 0x51a7u;
}
static void choices(ko_model_t *m) {
    uint8_t q = m->questions[m->round_pos];
    unsigned start = q < KO_LETTERS ? 0 : KO_LETTERS;
    unsigned count = q < KO_LETTERS ? KO_LETTERS : KO_COUNT - KO_LETTERS;
    unsigned correct_slot = random_next(m) % 3;
    m->choices[correct_slot] = q;
    uint8_t used[KO_COUNT] = {0};
    used[q] = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (i == correct_slot) continue;
        unsigned id = start + random_next(m) % count;
        while (used[id]) id = start + ((id - start + 1) % count);
        used[id] = 1;
        m->choices[i] = (uint8_t)id;
    }
    m->selection = 0;
    m->feedback = false;
}
static void start_quiz(ko_model_t *m) {
    uint8_t pool[KO_COUNT];
    unsigned n = 0;
    m->review = m->menu == 4;
    m->listening = m->menu == 3;
    for (unsigned i = 0; i < KO_COUNT; ++i) {
        if (m->review ? ((m->progress.mistakes >> i) & 1u) : i >= KO_LETTERS)
            pool[n++] = (uint8_t)i;
    }
    if (!n) { m->page = KO_EMPTY; return; }
    // Fisher-Yates produces a bounded round without duplicate questions.
    for (unsigned i = n - 1; i > 0; --i) {
        unsigned j = random_next(m) % (i + 1);
        uint8_t tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
    }
    m->round_count = n < KO_ROUND ? n : KO_ROUND;
    memcpy(m->questions, pool, m->round_count);
    m->round_pos = 0;
    m->round_correct = 0;
    m->page = KO_QUIZ;
    choices(m);
}
bool ko_handle(ko_model_t *m, ko_key_t key) {
    bool dirty = false;
    if (key == KO_BACK) { m->page = KO_HOME; return false; }
    if (m->page == KO_HOME) {
        if (key == KO_UP) m->menu = (m->menu + 4) % 5;
        if (key == KO_DOWN) m->menu = (m->menu + 1) % 5;
        if (key == KO_OK) {
            if (m->menu < 2) {
                m->page = KO_CARDS;
                m->card = m->menu == 0 ? 0 : KO_LETTERS;
                m->flipped = false;
                m->progress.seen |= UINT64_C(1) << m->card;
                dirty = true;
            } else start_quiz(m);
        }
    } else if (m->page == KO_CARDS) {
        unsigned first = m->menu == 0 ? 0 : KO_LETTERS;
        unsigned count = m->menu == 0 ? KO_LETTERS : KO_COUNT - KO_LETTERS;
        if (key == KO_OK) m->flipped = !m->flipped;
        if (key == KO_UP || key == KO_DOWN) {
            m->card = first + (m->card - first + count + (key == KO_UP ? -1 : 1)) % count;
            m->flipped = false;
            uint64_t bit = UINT64_C(1) << m->card;
            dirty = !(m->progress.seen & bit);
            m->progress.seen |= bit;
        }
    } else if (m->page == KO_QUIZ) {
        if (m->feedback) {
            if (key == KO_OK) {
                if (++m->round_pos == m->round_count) m->page = KO_RESULT;
                else choices(m);
            }
        } else {
            if (key == KO_UP) m->selection = (m->selection + 2) % 3;
            if (key == KO_DOWN) m->selection = (m->selection + 1) % 3;
            if (key == KO_OK) {
                unsigned q = m->questions[m->round_pos];
                uint64_t bit = UINT64_C(1) << q;
                m->last_correct = m->choices[m->selection] == q;
                m->progress.seen |= bit;
                if (m->progress.answered < UINT32_MAX) ++m->progress.answered;
                if (m->last_correct) {
                    ++m->round_correct;
                    if (m->progress.correct < UINT32_MAX) ++m->progress.correct;
                    if (m->progress.streak[q] < 3) ++m->progress.streak[q];
                    if (m->progress.streak[q] >= 2) m->progress.mistakes &= ~bit;
                } else {
                    m->progress.mistakes |= bit;
                    m->progress.streak[q] = 0;
                }
                m->feedback = true;
                dirty = true;
            }
        }
    } else if (key == KO_OK) m->page = KO_HOME;
    return dirty;
}
static void put32(uint8_t *out, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out[i] = n >> (8 * i);
}
static uint32_t get32(const uint8_t *p) {
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= (uint32_t)p[i] << (8 * i);
    return n;
}
void ko_encode(const ko_progress_t *p, uint8_t out[KO_SAVE_BYTES]) {
    // Explicit little-endian format; never persist compiler-dependent struct padding.
    put32(out, (uint32_t)p->seen); put32(out + 4, p->seen >> 32);
    put32(out + 8, (uint32_t)p->mistakes); put32(out + 12, p->mistakes >> 32);
    put32(out + 16, p->answered); put32(out + 20, p->correct);
    memcpy(out + 24, p->streak, KO_COUNT);
}
bool ko_decode(ko_progress_t *p, const uint8_t *data, size_t size) {
    if (!data || size != KO_SAVE_BYTES) return false;
    ko_progress_t candidate = {0};
    candidate.seen = get32(data) | ((uint64_t)get32(data + 4) << 32);
    candidate.mistakes = get32(data + 8) | ((uint64_t)get32(data + 12) << 32);
    candidate.answered = get32(data + 16); candidate.correct = get32(data + 20);
    memcpy(candidate.streak, data + 24, KO_COUNT);
    if ((candidate.seen >> KO_COUNT) || (candidate.mistakes >> KO_COUNT)
        || (candidate.mistakes & ~candidate.seen) || candidate.correct > candidate.answered)
        return false;
    for (unsigned i = 0; i < KO_COUNT; ++i) {
        if (candidate.streak[i] > 3 || (candidate.streak[i] && !(candidate.seen & (UINT64_C(1) << i))))
            return false;
    }
    *p = candidate;
    return true;
}
