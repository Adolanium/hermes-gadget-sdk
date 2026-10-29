/**
 * @file ml_json_strip.c
 * @brief Remove unused members from a JSON document in place.
 *
 * MapResponses carry large fields TailTiny never reads (Hostinfo, CapMap,
 * PacketFilter, UserProfiles, ...). cJSON's DOM costs roughly 3x the text
 * size, so dropping them before cJSON_Parse() cuts the peak heap needed to
 * join a tailnet, which is what decides whether small chips fit at all.
 *
 * Single pass, output written over the input (the write position never
 * passes the read position). Strings and escapes are respected, so braces or
 * quotes inside string values cannot confuse it. Only object members whose
 * key is on the list are removed, at any depth; array elements are kept.
 * Input that is not valid JSON is never made "more valid": on anything
 * unexpected the function stops stripping and copies the rest unchanged.
 */

#include "ml_json_strip.h"
#include <string.h>
#include <stdbool.h>

#define MAX_DEPTH 64

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Returns index just past the string starting at s[i] == '"', or len if unterminated. */
static size_t skip_string(const char *s, size_t i, size_t len) {
    i++;
    while (i < len) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == '"') return i + 1;
        i++;
    }
    return len;
}

/* Returns index just past the JSON value starting at s[i] (after whitespace),
 * or len if it runs off the end. */
static size_t skip_value(const char *s, size_t i, size_t len) {
    while (i < len && is_ws(s[i])) i++;
    if (i >= len) return len;
    if (s[i] == '"') return skip_string(s, i, len);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < len) {
            char c = s[i];
            if (c == '"') { i = skip_string(s, i, len); continue; }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) return i + 1;
            }
            i++;
        }
        return len;
    }
    /* number, true, false, null */
    while (i < len && s[i] != ',' && s[i] != '}' && s[i] != ']' && !is_ws(s[i])) i++;
    return i;
}

static bool key_listed(const char *key, size_t key_len, const char *const *keys) {
    for (const char *const *k = keys; *k; k++) {
        if (strlen(*k) == key_len && memcmp(*k, key, key_len) == 0) return true;
    }
    return false;
}

size_t ml_json_strip_keys(char *buf, size_t len, const char *const *keys) {
    bool in_object[MAX_DEPTH];
    int depth = 0;
    size_t r = 0, w = 0;
    /* true when the next string token in the current object is a member key */
    bool expect_key = false;

    while (r < len) {
        char c = buf[r];

        if (c == '"') {
            size_t end = skip_string(buf, r, len);
            if (expect_key && depth > 0 && in_object[depth - 1]) {
                /* Look past the key for ':' */
                size_t colon = end;
                while (colon < len && is_ws(buf[colon])) colon++;
                if (colon < len && buf[colon] == ':' &&
                    key_listed(buf + r + 1, end - r - 2, keys)) {
                    size_t after = skip_value(buf, colon + 1, len);
                    if (after >= len) {
                        /* Truncated input: stop stripping, keep the rest */
                        memmove(buf + w, buf + r, len - r);
                        return w + (len - r);
                    }
                    /* Drop the member and one separating comma */
                    size_t next = after;
                    while (next < len && is_ws(buf[next])) next++;
                    if (next < len && buf[next] == ',') {
                        r = next + 1;               /* first or middle member */
                    } else {
                        /* last member: remove the comma we already wrote */
                        size_t back = w;
                        while (back > 0 && is_ws(buf[back - 1])) back--;
                        if (back > 0 && buf[back - 1] == ',') w = back - 1;
                        r = after;
                    }
                    /* still expecting a key (next member) */
                    continue;
                }
            }
            memmove(buf + w, buf + r, end - r);
            w += end - r;
            r = end;
            expect_key = false;
            continue;
        }

        if (c == '{' || c == '[') {
            if (depth >= MAX_DEPTH) {
                memmove(buf + w, buf + r, len - r);
                return w + (len - r);
            }
            in_object[depth++] = (c == '{');
            expect_key = (c == '{');
        } else if (c == '}' || c == ']') {
            if (depth > 0) depth--;
            expect_key = false;
        } else if (c == ',') {
            expect_key = (depth > 0 && in_object[depth - 1]);
        } else if (c == ':') {
            expect_key = false;
        }
        buf[w++] = c;
        r++;
    }
    return w;
}

bool ml_json_next_member(const char *buf, size_t len, size_t *pos,
                         size_t *key_start, size_t *key_len,
                         size_t *val_start, size_t *val_len) {
    size_t i = *pos;
    if (i < len && buf[i] == '{') i++;          /* first call: at the '{' */
    while (i < len && (is_ws(buf[i]) || buf[i] == ',')) i++;
    if (i >= len || buf[i] != '"') return false;   /* '}' or malformed */
    size_t kend = skip_string(buf, i, len);
    if (kend >= len) return false;
    size_t c = kend;
    while (c < len && is_ws(buf[c])) c++;
    if (c >= len || buf[c] != ':') return false;
    size_t v = c + 1;
    while (v < len && is_ws(buf[v])) v++;
    size_t vend = skip_value(buf, v, len);
    if (vend > len || v >= len) return false;
    *key_start = i + 1;
    *key_len = kend - i - 2;
    *val_start = v;
    *val_len = vend - v;
    *pos = vend;
    return true;
}

bool ml_json_find_member(const char *buf, size_t len, const char *key,
                         size_t *val_start, size_t *val_len) {
    size_t pos = 0, ks, kl;
    while (pos < len && is_ws(buf[pos])) pos++;
    if (pos >= len || buf[pos] != '{') return false;
    size_t klen = strlen(key);
    while (ml_json_next_member(buf, len, &pos, &ks, &kl, val_start, val_len)) {
        if (kl == klen && memcmp(buf + ks, key, klen) == 0) return true;
    }
    return false;
}
