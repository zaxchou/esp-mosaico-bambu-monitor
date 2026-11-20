// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Dependency-free report scanner. A document is validated completely before
 * any spans are inspected. Limits bound work and the iterative scanner stack;
 * returned spans borrow the input, which must remain immutable while in use. */
#define BMB_JSON_MAX_BYTES (64U * 1024U)
#define BMB_JSON_MAX_DEPTH 32U

typedef struct {
    const char *p;
    const char *end;
} bmb_json_span_t;

bool bmb_json_validate(const char *json, size_t len, bmb_json_span_t *root);
bool bmb_json_object_get(bmb_json_span_t object, const char *key,
                          bmb_json_span_t *value);
bool bmb_json_array_begin(bmb_json_span_t array, bmb_json_span_t *cursor);
/* Success always advances cursor. Only use on a completely validated array. */
bool bmb_json_array_next(bmb_json_span_t *cursor, bmb_json_span_t *value);
bool bmb_json_string_equal(bmb_json_span_t value, const char *text);
/* Returns the decoded byte length, or SIZE_MAX for a non-string/embedded NUL.
 * Output is terminated and truncated only between complete UTF-8 characters. */
size_t bmb_json_string_copy(bmb_json_span_t value, char *out, size_t cap);
/* FNV-1a over the raw bytes between a string value's quotes. Unlike the
 * truncating copy this distinguishes long strings that share a prefix, so
 * callers can detect identity changes that a bounded display copy cannot. */
bool bmb_json_string_hash(bmb_json_span_t value, uint32_t *hash);
bool bmb_json_uint32(bmb_json_span_t value, bool allow_string, uint32_t *out);
bool bmb_json_rounded(bmb_json_span_t value, bool allow_string, int *out);
