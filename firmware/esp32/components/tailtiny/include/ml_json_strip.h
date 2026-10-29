/**
 * @file ml_json_strip.h
 * @brief Remove unused members from a JSON document in place.
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Remove every object member whose key is in @p keys (NULL-terminated list),
 * at any nesting depth, rewriting @p buf in place.
 *
 * @return the new length. The buffer is not NUL-terminated by this function.
 */
size_t ml_json_strip_keys(char *buf, size_t len, const char *const *keys);

/**
 * Walk the members of the JSON object that starts at buf[*pos] == '{'.
 * Start with *pos at the '{'; each call returns the next member's key and
 * value as offsets into buf (key without quotes) and advances *pos.
 *
 * @return false when there are no more members (or the input is malformed)
 */
bool ml_json_next_member(const char *buf, size_t len, size_t *pos,
                         size_t *key_start, size_t *key_len,
                         size_t *val_start, size_t *val_len);

/**
 * Find a direct member of the object in buf (buf must start with '{',
 * leading whitespace allowed). Nested objects are not searched.
 */
bool ml_json_find_member(const char *buf, size_t len, const char *key,
                         size_t *val_start, size_t *val_len);

#ifdef __cplusplus
}
#endif
