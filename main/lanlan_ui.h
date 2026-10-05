/* Cyber Lanlan LVGL screens.
 *
 * The application implements its own screens, layout, visual language and
 * navigation; it does not reuse the baseline hardware-test menu, the demo_*.c
 * pages or the ui_pixel test shell (see docs/development/ai-guide.md, "Mandatory
 * UI redesign for derivative applications").
 *
 * Rendering contract:
 *   - every function here runs under bsp_lvgl_lock(); nothing in this module
 *     performs JSON, HTTP or filesystem work;
 *   - the renderer reads only lanlan_ui_state_t, which carries the pure-logic
 *     view model plus values the application worker already resolved;
 *   - the companion sprite frames are copied one at a time into ONE canvas
 *     buffer that is allocated at init and never reallocated;
 *   - the screen's children are rebuilt in place, so any LVGL animation or
 *     timer that could reference a deleted object is stopped first. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "lanlan_caregiver.h"
#include "lanlan_model.h"
#include "lanlan_reminder.h"

/* Everything the renderer needs. The application worker fills this in; the UI
 * never reaches back into the network, storage or the sync state machine. */
typedef struct {
    const lanlan_model_t *model;
    const lanlan_records_view_t *view;
    const lanlan_local_now_t *now;
    /* Caregiver directory (service user id -> display name). A NULL table, an
     * unknown id and a rejected name all render the neutral label, never a raw
     * id and never an empty string. */
    const lanlan_caregiver_table_t *caregivers;
    int battery_percent;      /* -1 hides the battery reading entirely */
    const char *status_text;  /* top-bar sync text, already localized */
    uint32_t status_color;    /* status dot colour, 0xRRGGBB */
    const char *status_detail;/* status page second line; may be NULL */
    bool cache_rebuilt;       /* show the "cache rebuilt" notice on home */
    bool storage_limited;     /* NVS could not store the whole cache */
    bool secure_url;          /* false: plain-HTTP LAN development URL */
    /* Scheme and host of the configured service URL, or NULL/"" when unknown.
     * The application passes only the scheme and authority: never a path, a
     * query string, the device token or any other credential. */
    const char *service_host;
    /* Non-NULL while a reminder is due: the bottom line shows this instead of
     * the key hint, so the due state stays visible without overlapping any
     * page content and regardless of the sound settings. */
    const char *banner_text;
} lanlan_ui_state_t;

/* Creates the single application screen and the one sprite buffer. Must be
 * called under bsp_lvgl_lock(). Returns the screen in *screen_out. */
esp_err_t lanlan_ui_init(lv_obj_t **screen_out);
/* Rebuilds the screen for the model's current page. Under bsp_lvgl_lock(). */
void lanlan_ui_render(lv_obj_t *screen, const lanlan_ui_state_t *state);
/* Stops the companion animation and releases the sprite buffer. */
void lanlan_ui_deinit(void);

/* Companion animation entry points, called by the application worker while it
 * holds bsp_lvgl_lock(). IDLE is the resting state; BLINK is scheduled
 * internally, HAPPY is the pet reaction and BARK draws attention. */
void lanlan_ui_companion_react(lanlan_character_t state);
void lanlan_ui_companion_bark(void);
/* True while the companion is showing: used to suppress work when the page is
 * not visible. */
bool lanlan_ui_companion_visible(void);

/* Single explicit invariant check for the "a pet action never creates a record"
 * rule; logs and returns false when the model reports a pending record action. */
bool lanlan_ui_pet_invariant_held(const lanlan_model_t *model);

#include "lanlan_reaction.h"
void lanlan_ui_companion_pet(lanlan_reaction_t reaction);
