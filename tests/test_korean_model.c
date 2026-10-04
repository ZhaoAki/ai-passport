#include "korean_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
static unsigned answer_slot(const ko_model_t *m) {
    for (unsigned i = 0; i < 3; ++i)
        if (m->choices[i] == m->questions[m->round_pos]) return i;
    assert(false); return 0;
}
static void start(ko_model_t *m, unsigned menu) {
    ko_handle(m, KO_BACK); m->menu = menu; ko_handle(m, KO_OK);
}
static void answer(ko_model_t *m, bool correct) {
    unsigned slot = answer_slot(m);
    m->selection = correct ? slot : (slot + 1) % 3;
    assert(ko_handle(m, KO_OK));
}
int main(void) {
    ko_model_t m;
    ko_init(&m, 0);
    assert(m.page == KO_HOME);
    ko_handle(&m, KO_UP); assert(m.menu == 4);
    ko_handle(&m, KO_OK); assert(m.page == KO_EMPTY);
    ko_handle(&m, KO_OK); assert(m.page == KO_HOME);
    start(&m, 0); assert(m.card == 0 && ko_seen_count(&m.progress) == 1);
    ko_handle(&m, KO_UP); assert(m.card == 23);
    ko_handle(&m, KO_DOWN); assert(m.card == 0);
    ko_handle(&m, KO_OK); assert(m.flipped);
    ko_handle(&m, KO_DOWN); assert(!m.flipped && m.card == 1);
    start(&m, 1); assert(m.card == 24);
    ko_handle(&m, KO_UP); assert(m.card == 59);
    ko_handle(&m, KO_DOWN); assert(m.card == 24);
    start(&m, 2);
    unsigned wrong_id = m.questions[0];
    answer(&m, false);
    assert(ko_mistake_count(&m.progress) == 1 && !m.last_correct);
    uint32_t answered = m.progress.answered;
    // Feedback confirmation advances; it never counts a repeated submission.
    ko_handle(&m, KO_DOWN); assert(m.progress.answered == answered);
    start(&m, 4); assert(m.round_count == 1 && m.questions[0] == wrong_id);
    answer(&m, true); assert(ko_mistake_count(&m.progress) == 1);
    ko_handle(&m, KO_OK); assert(m.page == KO_RESULT && m.round_correct == 1);
    start(&m, 4); answer(&m, true); assert(ko_mistake_count(&m.progress) == 0);
    start(&m, 4); assert(m.page == KO_EMPTY);
    uint8_t bytes[KO_SAVE_BYTES]; ko_encode(&m.progress, bytes);
    ko_progress_t loaded = {0}; assert(ko_decode(&loaded, bytes, sizeof(bytes)));
    assert(loaded.seen == m.progress.seen && loaded.answered == m.progress.answered);
    assert(memcmp(loaded.streak, m.progress.streak, KO_COUNT) == 0);
    ko_progress_t before = loaded;
    assert(!ko_decode(&loaded, bytes, sizeof(bytes) - 1));
    assert(!ko_decode(&loaded, NULL, sizeof(bytes)));
    bytes[7] = 0xFF; assert(!ko_decode(&loaded, bytes, sizeof(bytes)));
    assert(loaded.seen == before.seen);
    ko_encode(&m.progress, bytes); bytes[24] = 4;
    assert(!ko_decode(&loaded, bytes, sizeof(bytes)));
    for (unsigned seed = 0; seed < 500; ++seed) {
        ko_init(&m, seed);
        start(&m, seed % 2 ? 2 : 3);
        assert(m.round_count == 5 && m.listening == (seed % 2 == 0));
        uint64_t seen = 0;
        for (unsigned i = 0; i < m.round_count; ++i) {
            unsigned q = m.questions[m.round_pos];
            assert(q >= 24 && q < 60 && !(seen & (UINT64_C(1) << q)));
            seen |= UINT64_C(1) << q;
            for (unsigned j = 0; j < 3; ++j) {
                assert(m.choices[j] >= 24 && m.choices[j] < 60);
                for (unsigned k = j + 1; k < 3; ++k) assert(m.choices[j] != m.choices[k]);
            }
            answer(&m, true); ko_handle(&m, KO_OK);
        }
        assert(m.page == KO_RESULT && m.round_correct == 5 && m.progress.correct == 5);
    }
    ko_init(&m, 7); m.progress.answered = UINT32_MAX; m.progress.correct = UINT32_MAX;
    start(&m, 2); answer(&m, true);
    assert(m.progress.answered == UINT32_MAX && m.progress.correct == UINT32_MAX);
    // Review may include alphabet records, with distractors from the same deck.
    ko_init(&m, 5); m.progress.seen = m.progress.mistakes = UINT64_C(1) << 3;
    start(&m, 4);
    for (unsigned i = 0; i < 3; ++i) assert(m.choices[i] < 24);
    puts("Korean model: PASS (navigation, quiz, review, persistence, saturation, 500 seeds)");
    return 0;
}
