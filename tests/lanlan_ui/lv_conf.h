/* LVGL configuration for the host-side Cyber Lanlan preview harness.
 *
 * Mirrors the important parts of the firmware configuration so the screens
 * render with the same colour depth, colour format and placeholder strategy as
 * the device: 16-bit RGB565 and placeholder glyphs for code points the
 * generated subsets do not carry. The heap size is deliberately larger than
 * the firmware's dedicated LVGL pool because the harness keeps several
 * iterations of the screen alive while it checks for leaks; the harness still
 * asserts at the end of every capture that allocator pressure stayed bounded.
 *
 * This file is only used by tests/lanlan_ui (host build). It is never part of
 * the firmware build. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (48 * 1024)

#define LV_USE_OS LV_OS_NONE
#define LV_COLOR_DEPTH 16

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_MEM_INTEGRITY 1

#define LV_USE_DRAW_SW 1
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_USE_DRAW_SW_COMPLEX 1

/* Placeholder glyphs are how the design renders a free caregiver note whose
 * characters fall outside the generated subset; the harness checks both the
 * covered and the uncovered case. */
#define LV_USE_FONT_PLACEHOLDER 1

#define LV_USE_THORVG 0
#define LV_USE_THORVG_INTERNAL 0

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif /* LV_CONF_H */
