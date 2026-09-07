/*
 * whats_new.h - bullets for the post-update "What's New" card.
 *
 * Update this list with every release, alongside the version bump in
 * shared_state.h. Rules:
 *   - Each bullet must fit ONE line on the card: keep it under 38 characters
 *     (montserrat_12 on a 280px-wide panel, no wrapping).
 *   - At most 5 bullets, or 4 when WHATS_NEW_FOOTER is defined: the footer
 *     takes the fifth row's space, and a sixth line would be drawn over the OK
 *     button. home_screen.c enforces this by dropping the extra bullet, so
 *     going over loses a line rather than corrupting the card.
 *   - Plain words about what the user gets, not implementation detail.
 */

#ifndef WHATS_NEW_H
#define WHATS_NEW_H

static const char *const whats_new_bullets[] = {
    "Verified connections to your CGM",
    "Urgent-low alarm always stays on",
    "Old readings never look current",
    "Updates come only from cygm.me",
    "Long alarms go quiet, keep flashing",
};
#define WHATS_NEW_COUNT (sizeof(whats_new_bullets) / sizeof(whats_new_bullets[0]))

/* Optional closing line, drawn below the bullets without one of its own and
   dimmed, to set it apart from the list. Comment out when there is none.
   This release uses the fifth bullet instead: with a footer defined the card
   only has room for four. */
/* #define WHATS_NEW_FOOTER "Security release: update every device" */

#endif // WHATS_NEW_H
