/*
 * Streaming JSON field picker
 */

#include "json_scan.h"
#include <stdio.h>
#include <string.h>

enum {
    ST_VALUE = 0,       // expecting a value, or ']' straight after '['
    ST_KEY_OR_END,      // straight after '{'
    ST_KEY,             // after ',' inside an object
    ST_IN_KEY,
    ST_COLON,
    ST_IN_STRING,
    ST_IN_PRIM,
    ST_AFTER_VALUE,
    ST_DONE,
    ST_ERROR,
};

static bool in_array(const json_scan_t *s) {
    return s->depth > 0 && (s->is_array & (1u << (s->depth - 1))) != 0;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Start a new last path component for a child of the innermost container.
// Returns false, and stops tracking at this depth, if it cannot be recorded.
static bool path_begin_component(json_scan_t *s) {
    if (s->bad_depth != 0 && s->depth <= s->bad_depth) s->bad_depth = 0;
    if (s->bad_depth != 0) return false;
    if (s->depth > JSON_SCAN_TRACK_MAX) {
        s->bad_depth = s->depth;
        return false;
    }
    s->path_len = s->plen[s->depth];
    s->path[s->path_len] = '\0';
    return true;
}

static void path_append(json_scan_t *s, char c) {
    if (s->bad_depth != 0) return;
    if (s->path_len + 1 >= JSON_SCAN_PATH_MAX) {
        s->bad_depth = s->depth;
        return;
    }
    s->path[s->path_len++] = c;
    s->path[s->path_len] = '\0';
}

static void open_container(json_scan_t *s, bool array) {
    if (s->depth >= JSON_SCAN_DEPTH_MAX) {
        s->state = ST_ERROR;
        return;
    }
    s->depth++;
    if (array) s->is_array |= (1u << (s->depth - 1));
    else       s->is_array &= ~(1u << (s->depth - 1));
    if (s->depth <= JSON_SCAN_TRACK_MAX) {
        s->plen[s->depth] = s->path_len;
        s->index[s->depth] = 0;
    }
    s->state = array ? ST_VALUE : ST_KEY_OR_END;
}

static void close_container(json_scan_t *s, bool array) {
    if (s->depth == 0 || in_array(s) != array) {
        s->state = ST_ERROR;
        return;
    }
    s->depth--;
    s->state = (s->depth == 0) ? ST_DONE : ST_AFTER_VALUE;
}

static void value_begin(json_scan_t *s, json_scan_type_t type) {
    s->cur = NULL;

    if (in_array(s) && path_begin_component(s)) {
        char idx[12];
        int n = snprintf(idx, sizeof(idx), "[%u]", (unsigned)s->index[s->depth]);
        for (int i = 0; i < n; i++) path_append(s, idx[i]);
    }
    if (s->bad_depth != 0) return;

    for (size_t i = 0; i < s->pick_count; i++) {
        json_pick_t *p = &s->picks[i];
        if (p->type == JSON_SCAN_NONE && strcmp(p->path, s->path) == 0) {
            p->type = type;
            if (type != JSON_SCAN_CONTAINER) s->cur = p;
            return;
        }
    }
}

static void value_char(json_scan_t *s, char c) {
    json_pick_t *p = s->cur;
    if (p == NULL || p->out == NULL || p->out_size == 0) return;
    if (p->len + 1 >= p->out_size) {
        p->truncated = true;
        return;
    }
    p->out[p->len++] = c;
    p->out[p->len] = '\0';
}

void json_scan_init(json_scan_t *s, json_pick_t *picks, size_t pick_count) {
    memset(s, 0, sizeof(*s));
    s->picks = picks;
    s->pick_count = pick_count;
    s->state = ST_VALUE;
    for (size_t i = 0; i < pick_count; i++) {
        picks[i].type = JSON_SCAN_NONE;
        picks[i].truncated = false;
        picks[i].len = 0;
        if (picks[i].out != NULL && picks[i].out_size > 0) picks[i].out[0] = '\0';
    }
}

bool json_scan_complete(const json_scan_t *s) {
    return s->state == ST_DONE;
}

static void feed_char(json_scan_t *s, char c) {
    switch (s->state) {
    case ST_VALUE:
        if (is_ws(c)) return;
        // Only "[]" may close here. Deeper than the tracked levels the element
        // count is not kept, so a trailing comma there goes unnoticed.
        if (c == ']' && in_array(s) &&
            (s->depth > JSON_SCAN_TRACK_MAX || s->index[s->depth] == 0)) {
            close_container(s, true);
            return;
        }
        if (c == '{' || c == '[') {
            value_begin(s, JSON_SCAN_CONTAINER);
            open_container(s, c == '[');
        } else if (c == '"') {
            value_begin(s, JSON_SCAN_STRING);
            s->state = ST_IN_STRING;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            value_begin(s, JSON_SCAN_NUMBER);
            value_char(s, c);
            s->state = ST_IN_PRIM;
        } else if (c == 't' || c == 'f' || c == 'n') {
            value_begin(s, c == 'n' ? JSON_SCAN_NULL : JSON_SCAN_BOOL);
            value_char(s, c);
            s->state = ST_IN_PRIM;
        } else {
            s->state = ST_ERROR;
        }
        return;

    case ST_KEY_OR_END:
    case ST_KEY:
        if (is_ws(c)) return;
        if (c == '}' && s->state == ST_KEY_OR_END) {
            close_container(s, false);
            return;
        }
        if (c != '"') {
            s->state = ST_ERROR;
            return;
        }
        if (path_begin_component(s) && s->path_len > 0) path_append(s, '.');
        s->state = ST_IN_KEY;
        return;

    case ST_IN_KEY:
        if (s->esc) {
            s->esc = false;
            path_append(s, c);
        } else if (c == '\\') {
            s->esc = true;
        } else if (c == '"') {
            s->state = ST_COLON;
        } else {
            path_append(s, c);
        }
        return;

    case ST_COLON:
        if (is_ws(c)) return;
        s->state = (c == ':') ? ST_VALUE : ST_ERROR;
        return;

    case ST_IN_STRING:
        if (s->uni_left > 0) {
            // Nothing this picker is asked for carries a \u escape, so one
            // placeholder stands in for the code unit rather than decoding it.
            if (--s->uni_left == 0) value_char(s, '?');
        } else if (s->esc) {
            s->esc = false;
            switch (c) {
            case 'n': value_char(s, '\n'); break;
            case 't': value_char(s, '\t'); break;
            case 'r': value_char(s, '\r'); break;
            case 'b': value_char(s, '\b'); break;
            case 'f': value_char(s, '\f'); break;
            case 'u': s->uni_left = 4; break;
            default:  value_char(s, c); break;
            }
        } else if (c == '\\') {
            s->esc = true;
        } else if (c == '"') {
            s->cur = NULL;
            s->state = (s->depth == 0) ? ST_ERROR : ST_AFTER_VALUE;
        } else {
            value_char(s, c);
        }
        return;

    case ST_IN_PRIM:
        if (c == ',' || c == '}' || c == ']' || is_ws(c)) {
            s->cur = NULL;
            s->state = (s->depth == 0) ? ST_ERROR : ST_AFTER_VALUE;
            feed_char(s, c);
        } else {
            value_char(s, c);
        }
        return;

    case ST_AFTER_VALUE:
        if (is_ws(c)) return;
        if (c == ',') {
            if (in_array(s)) {
                if (s->depth <= JSON_SCAN_TRACK_MAX && s->index[s->depth] < UINT16_MAX) {
                    s->index[s->depth]++;
                }
                s->state = ST_VALUE;
            } else {
                s->state = ST_KEY;
            }
        } else if (c == '}') {
            close_container(s, false);
        } else if (c == ']') {
            close_container(s, true);
        } else {
            s->state = ST_ERROR;
        }
        return;

    case ST_DONE:
        if (!is_ws(c)) s->state = ST_ERROR;
        return;

    default:
        return;
    }
}

void json_scan_feed(json_scan_t *s, const char *data, size_t len) {
    for (size_t i = 0; i < len && s->state != ST_ERROR; i++) {
        feed_char(s, data[i]);
    }
}
