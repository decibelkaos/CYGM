/*
 * whats_new.h - copy for the post-update "What's New" card.
 *
 * Update this with every release, alongside the version bump in
 * shared_state.h. Rules:
 *   - Each bullet must fit ONE line on the card: keep it under 38 characters
 *     (montserrat_12 on a 280px-wide panel, no wrapping).
 *   - The row budget is worked out in home_screen.c from the space above the
 *     OK button. Five rows bare, four with an intro, one fewer again with a
 *     footer. Going over loses a line rather than corrupting the card.
 *   - Plain words about what the user gets, not implementation detail.
 */

#ifndef WHATS_NEW_H
#define WHATS_NEW_H

/* Optional opener, in place of the "WHAT'S NEW" caption. Two lines at most:
   each one costs a bullet. Comment out to get the caption back. */
/* #define WHATS_NEW_INTRO "" */

static const char *const whats_new_bullets[] = {
    "LibreLinkUp sign-in fixed",
    "Works with larger Libre accounts",
    "Clearer sign-in error messages",
    "Full release notes on CYGM.me",
};
#define WHATS_NEW_COUNT (sizeof(whats_new_bullets) / sizeof(whats_new_bullets[0]))

/* Optional closing line, drawn below the bullets without one of its own and
   dimmed, to set it apart from the list. Comment out when there is none. */

#endif // WHATS_NEW_H
