#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define KO_COUNT 60
#define KO_LETTERS 24
#define KO_ROUND 5
#define KO_SAVE_BYTES (24 + KO_COUNT)
typedef struct {
    const char *ko, *zh, *roman, *speech, *note;
} ko_item_t;
extern const ko_item_t ko_items[KO_COUNT];
typedef struct {
    uint64_t seen, mistakes;
    uint32_t answered, correct;
    uint8_t streak[KO_COUNT];
} ko_progress_t;
typedef enum { KO_HOME, KO_CARDS, KO_QUIZ, KO_RESULT, KO_EMPTY, KO_AUDIO_ERROR } ko_page_t;
typedef enum { KO_UP, KO_DOWN, KO_OK, KO_BACK } ko_key_t;
typedef struct {
    ko_progress_t progress;
    ko_page_t page;
    uint8_t menu, card, selection, round_count, round_pos, round_correct;
    uint8_t questions[KO_ROUND], choices[3];
    bool flipped, feedback, last_correct, listening, review;
    uint32_t rng;
} ko_model_t;
void ko_init(ko_model_t *m, uint32_t seed);
bool ko_handle(ko_model_t *m, ko_key_t key);
unsigned ko_mistake_count(const ko_progress_t *p);
unsigned ko_seen_count(const ko_progress_t *p);
void ko_encode(const ko_progress_t *p, uint8_t out[KO_SAVE_BYTES]);
bool ko_decode(ko_progress_t *p, const uint8_t *data, size_t size);
