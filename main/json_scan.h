/*
 * Streaming JSON field picker
 *
 * Reads a JSON document a chunk at a time and copies out only the values at a
 * fixed set of paths. Nothing is buffered, so the size of the document does
 * not matter and there is no recursion to bound.
 */

#ifndef JSON_SCAN_H
#define JSON_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSON_SCAN_NONE = 0,     // path never appeared
    JSON_SCAN_STRING,
    JSON_SCAN_NUMBER,
    JSON_SCAN_BOOL,
    JSON_SCAN_NULL,
    JSON_SCAN_CONTAINER,    // object or array; nothing is copied
} json_scan_type_t;

// One wanted value. Paths are written "data.authTicket.token" and
// "data[0].patientId". The first occurrence of a path wins.
typedef struct {
    const char *path;
    char *out;              // may be NULL to record presence and type only
    size_t out_size;
    json_scan_type_t type;
    bool truncated;         // value did not fit in out
    size_t len;
} json_pick_t;

// Path components deeper or longer than these are skipped, not matched.
#define JSON_SCAN_PATH_MAX   96
#define JSON_SCAN_TRACK_MAX  8
// Nesting beyond this fails the scan.
#define JSON_SCAN_DEPTH_MAX  32

typedef struct {
    json_pick_t *picks;
    size_t pick_count;
    json_pick_t *cur;
    uint32_t is_array;                      // one bit per open container
    uint16_t index[JSON_SCAN_TRACK_MAX + 1];
    uint8_t plen[JSON_SCAN_TRACK_MAX + 1];  // path length when the container opened
    char path[JSON_SCAN_PATH_MAX];
    uint8_t path_len;
    uint8_t depth;
    uint8_t bad_depth;                      // 0, or the depth at which the path stopped being tracked
    uint8_t state;
    uint8_t uni_left;
    bool esc;
} json_scan_t;

// Clears every pick's result fields and terminates every out buffer.
void json_scan_init(json_scan_t *s, json_pick_t *picks, size_t pick_count);
void json_scan_feed(json_scan_t *s, const char *data, size_t len);
// True once exactly one complete, well-formed top-level object or array was read.
bool json_scan_complete(const json_scan_t *s);

#ifdef __cplusplus
}
#endif

#endif // JSON_SCAN_H
