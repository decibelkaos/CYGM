/*
 * whats_new.h - bullets for the post-update "What's New" card.
 *
 * Update this list with every release, alongside the version bump in
 * shared_state.h. Rules:
 *   - Each bullet must fit ONE line on the card: keep it under 38 characters
 *     (montserrat_12 on a 280px-wide panel, no wrapping).
 *   - At most 5 bullets — the card clips anything past what fits.
 *   - Plain words about what the user gets, not implementation detail.
 */

#ifndef WHATS_NEW_H
#define WHATS_NEW_H

static const char *const whats_new_bullets[] = {
    "Nightscout settings survive reboots",
    "This card - what changed, each update",
};
#define WHATS_NEW_COUNT (sizeof(whats_new_bullets) / sizeof(whats_new_bullets[0]))

#endif // WHATS_NEW_H
