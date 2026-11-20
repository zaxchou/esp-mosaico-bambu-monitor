// SPDX-License-Identifier: Apache-2.0
#include "bmb_json.h"

#include <limits.h>
#include <string.h>

static void whitespace(bmb_json_span_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' ||
                             *s->p == '\r' || *s->p == '\n')) {
        ++s->p;
    }
}

static bool hex4(bmb_json_span_t *s, uint32_t *out)
{
    if (s->end - s->p < 4) return false;
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)*s->p++;
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10U;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10U;
        else return false;
        n = n * 16U + digit;
    }
    *out = n;
    return true;
}

/* Read a single string character, checking UTF-8 and JSON escapes. This also
 * validates ignored strings/keys, including paired UTF-16 surrogate escapes. */
static bool character(bmb_json_span_t *s, uint32_t *out)
{
    if (s->p >= s->end) return false;
    unsigned char c = (unsigned char)*s->p++;
    if (c < 0x20U || c == '"') return false;
    uint32_t cp = c;
    if (c == '\\') {
        if (s->p >= s->end) return false;
        switch (*s->p++) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (!hex4(s, &cp)) return false;
            if (cp >= 0xD800U && cp <= 0xDBFFU) {
                uint32_t low;
                if (s->end - s->p < 6 || s->p[0] != '\\' || s->p[1] != 'u') {
                    return false;
                }
                s->p += 2;
                if (!hex4(s, &low) || low < 0xDC00U || low > 0xDFFFU) return false;
                cp = 0x10000U + (cp - 0xD800U) * 0x400U + low - 0xDC00U;
            } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                return false;
            }
            break;
        default: return false;
        }
    } else if (c >= 0x80U) {
        unsigned count;
        uint32_t minimum;
        if (c >= 0xC2U && c <= 0xDFU) {
            count = 1; minimum = 0x80U; cp = c & 0x1FU;
        } else if (c >= 0xE0U && c <= 0xEFU) {
            count = 2; minimum = 0x800U; cp = c & 0x0FU;
        } else if (c >= 0xF0U && c <= 0xF4U) {
            count = 3; minimum = 0x10000U; cp = c & 0x07U;
        } else {
            return false;
        }
        if ((size_t)(s->end - s->p) < count) return false;
        while (count--) {
            unsigned char next = (unsigned char)*s->p++;
            if ((next & 0xC0U) != 0x80U) return false;
            cp = cp * 64U + (next & 0x3FU);
        }
        if (cp < minimum || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            return false;
        }
    }
    *out = cp;
    return true;
}

static bool string(bmb_json_span_t *s)
{
    if (s->p >= s->end || *s->p++ != '"') return false;
    while (s->p < s->end && *s->p != '"') {
        uint32_t cp;
        if (!character(s, &cp)) return false;
    }
    if (s->p == s->end) return false;
    ++s->p;
    return true;
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool number(bmb_json_span_t *s)
{
    if (s->p < s->end && *s->p == '-') ++s->p;
    if (s->p == s->end) return false;
    if (*s->p == '0') {
        ++s->p;
    } else if (*s->p >= '1' && *s->p <= '9') {
        do { ++s->p; } while (s->p < s->end && digit(*s->p));
    } else {
        return false;
    }
    if (s->p < s->end && *s->p == '.') {
        ++s->p;
        if (s->p == s->end || !digit(*s->p)) return false;
        do { ++s->p; } while (s->p < s->end && digit(*s->p));
    }
    if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
        ++s->p;
        if (s->p < s->end && (*s->p == '-' || *s->p == '+')) ++s->p;
        if (s->p == s->end || !digit(*s->p)) return false;
        do { ++s->p; } while (s->p < s->end && digit(*s->p));
    }
    return true;
}

/* Each frame is one byte; even maximum nesting does not recurse or allocate.
 * The grammar requires separators explicitly, so stray brackets, missing
 * values, trailing commas, invalid literals and non-progress cannot pass. */
static bool value(bmb_json_span_t *s)
{
    enum {
        ROOT_VALUE, ROOT_END, OBJECT_FIRST, OBJECT_KEY, OBJECT_COLON,
        OBJECT_VALUE, OBJECT_END, ARRAY_FIRST, ARRAY_VALUE, ARRAY_END,
    };
    unsigned char frames[BMB_JSON_MAX_DEPTH + 1U] = {ROOT_VALUE};
    unsigned depth = 1;
    for (;;) {
        unsigned char *frame = &frames[depth - 1U];
        if (*frame == ROOT_END) return true;
        whitespace(s);
        switch (*frame) {
        case ROOT_VALUE: *frame = ROOT_END; break;
        case OBJECT_FIRST:
            if (s->p < s->end && *s->p == '}') { ++s->p; --depth; continue; }
            /* fall through */
        case OBJECT_KEY:
            if (!string(s)) return false;
            *frame = OBJECT_COLON;
            continue;
        case OBJECT_COLON:
            if (s->p == s->end || *s->p++ != ':') return false;
            *frame = OBJECT_VALUE;
            continue;
        case OBJECT_VALUE: *frame = OBJECT_END; break;
        case OBJECT_END:
            if (s->p == s->end) return false;
            if (*s->p == '}') { ++s->p; --depth; continue; }
            if (*s->p++ != ',') return false;
            *frame = OBJECT_KEY;
            continue;
        case ARRAY_FIRST:
            if (s->p < s->end && *s->p == ']') { ++s->p; --depth; continue; }
            /* fall through */
        case ARRAY_VALUE: *frame = ARRAY_END; break;
        case ARRAY_END:
            if (s->p == s->end) return false;
            if (*s->p == ']') { ++s->p; --depth; continue; }
            if (*s->p++ != ',') return false;
            *frame = ARRAY_VALUE;
            continue;
        default: return false;
        }
        if (s->p == s->end) return false;
        char c = *s->p;
        if (c == '{' || c == '[') {
            if (depth > BMB_JSON_MAX_DEPTH) return false;
            frames[depth++] = c == '{' ? OBJECT_FIRST : ARRAY_FIRST;
            ++s->p;
        } else if (c == '"') {
            if (!string(s)) return false;
        } else if (c == '-' || digit(c)) {
            if (!number(s)) return false;
        } else {
            const char *literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
            size_t len = strlen(literal);
            if ((size_t)(s->end - s->p) < len || memcmp(s->p, literal, len) != 0) {
                return false;
            }
            s->p += len;
        }
    }
}

bool bmb_json_validate(const char *json, size_t len, bmb_json_span_t *root)
{
    if (!json || !root || len == 0 || len > BMB_JSON_MAX_BYTES) return false;
    bmb_json_span_t s = {json, json + len};
    whitespace(&s);
    const char *start = s.p;
    if (!value(&s)) return false;
    const char *end = s.p;
    whitespace(&s);
    if (s.p != s.end) return false;
    *root = (bmb_json_span_t){start, end};
    return true;
}

static unsigned encode(uint32_t cp, char bytes[4])
{
    if (cp < 0x80U) { bytes[0] = (char)cp; return 1; }
    if (cp < 0x800U) {
        bytes[0] = (char)(0xC0U | (cp >> 6));
        bytes[1] = (char)(0x80U | (cp & 0x3FU));
        return 2;
    }
    if (cp < 0x10000U) {
        bytes[0] = (char)(0xE0U | (cp >> 12));
        bytes[1] = (char)(0x80U | ((cp >> 6) & 0x3FU));
        bytes[2] = (char)(0x80U | (cp & 0x3FU));
        return 3;
    }
    bytes[0] = (char)(0xF0U | (cp >> 18));
    bytes[1] = (char)(0x80U | ((cp >> 12) & 0x3FU));
    bytes[2] = (char)(0x80U | ((cp >> 6) & 0x3FU));
    bytes[3] = (char)(0x80U | (cp & 0x3FU));
    return 4;
}

size_t bmb_json_string_copy(bmb_json_span_t s, char *out, size_t cap)
{
    if (!out || cap == 0) return SIZE_MAX;
    out[0] = '\0';
    if (s.p == s.end || *s.p++ != '"') return SIZE_MAX;
    size_t length = 0, used = 0;
    bool full = false;
    while (s.p < s.end && *s.p != '"') {
        uint32_t cp;
        char bytes[4];
        if (!character(&s, &cp) || cp == 0) { out[0] = '\0'; return SIZE_MAX; }
        unsigned n = encode(cp, bytes);
        length += n;
        if (!full && used + n < cap) {
            memcpy(out + used, bytes, n);
            used += n;
        } else {
            full = true;
        }
    }
    out[used] = '\0';
    if (s.p == s.end || *s.p++ != '"' || s.p != s.end) return SIZE_MAX;
    return length;
}

bool bmb_json_string_hash(bmb_json_span_t value, uint32_t *hash)
{
    if (!hash || value.p == value.end || *value.p != '"') return false;
    value.p++;
    uint32_t h = 2166136261U;
    while (value.p < value.end && *value.p != '"') {
        uint32_t cp;
        char bytes[4];
        if (!character(&value, &cp) || cp == 0) return false;
        unsigned n = encode(cp, bytes);
        for (unsigned i = 0; i < n; ++i) h = (h ^ (unsigned char)bytes[i]) * 16777619U;
    }
    if (value.p == value.end || *value.p++ != '"' || value.p != value.end) return false;
    *hash = h;
    return true;
}

bool bmb_json_string_equal(bmb_json_span_t s, const char *text)
{
    if (s.p == s.end || *s.p++ != '"') return false;
    size_t remaining = strlen(text);
    while (s.p < s.end && *s.p != '"') {
        uint32_t cp;
        char bytes[4];
        if (!character(&s, &cp) || cp == 0) return false;
        unsigned n = encode(cp, bytes);
        if (remaining < n || memcmp(text, bytes, n) != 0) return false;
        remaining -= n;
        text += n;
    }
    return s.p < s.end && *s.p++ == '"' && s.p == s.end && remaining == 0;
}

bool bmb_json_object_get(bmb_json_span_t s, const char *key, bmb_json_span_t *out)
{
    if (s.p == s.end || *s.p++ != '{') return false;
    whitespace(&s);
    while (s.p < s.end && *s.p != '}') {
        bmb_json_span_t name = {s.p, NULL};
        if (!string(&s)) return false;
        name.end = s.p;
        whitespace(&s);
        if (s.p == s.end || *s.p++ != ':') return false;
        whitespace(&s);
        const char *start = s.p;
        if (!value(&s)) return false;
        if (bmb_json_string_equal(name, key)) {
            *out = (bmb_json_span_t){start, s.p};
            return true;
        }
        whitespace(&s);
        if (s.p < s.end && *s.p == ',') { ++s.p; whitespace(&s); }
        else break;
    }
    return false;
}

bool bmb_json_array_begin(bmb_json_span_t array, bmb_json_span_t *cursor)
{
    if (array.end - array.p < 2 || array.p[0] != '[' || array.end[-1] != ']') {
        return false;
    }
    *cursor = (bmb_json_span_t){array.p + 1, array.end - 1};
    return true;
}

bool bmb_json_array_next(bmb_json_span_t *cursor, bmb_json_span_t *out)
{
    whitespace(cursor);
    if (cursor->p == cursor->end) return false;
    const char *start = cursor->p;
    if (!value(cursor) || cursor->p <= start) return false;
    *out = (bmb_json_span_t){start, cursor->p};
    whitespace(cursor);
    if (cursor->p < cursor->end && *cursor->p == ',') ++cursor->p;
    return true;
}

bool bmb_json_uint32(bmb_json_span_t s, bool allow_string, uint32_t *out)
{
    char decoded[16];
    if (allow_string && s.p < s.end && *s.p == '"') {
        size_t len = bmb_json_string_copy(s, decoded, sizeof(decoded));
        if (len == SIZE_MAX || len >= sizeof(decoded)) return false;
        s = (bmb_json_span_t){decoded, decoded + len};
    }
    if (s.p == s.end) return false;
    uint32_t n = 0;
    while (s.p < s.end && digit(*s.p)) {
        uint32_t d = (uint32_t)(*s.p++ - '0');
        if (n > (UINT32_MAX - d) / 10U) return false;
        n = n * 10U + d;
    }
    if (s.p != s.end) return false;
    *out = n;
    return true;
}

/* Decimal arithmetic avoids floating point boundary/locale problems and signed
 * overflow. Exponents are bounded while reading; the input itself is bounded.
 * Round halves away from zero, including at the exact signed integer limits. */
bool bmb_json_rounded(bmb_json_span_t s, bool allow_string, int *out)
{
    char decoded[64];
    if (allow_string && s.p < s.end && *s.p == '"') {
        size_t len = bmb_json_string_copy(s, decoded, sizeof(decoded));
        if (len == SIZE_MAX || len >= sizeof(decoded)) return false;
        s = (bmb_json_span_t){decoded, decoded + len};
    }
    bmb_json_span_t grammar = s;
    if (!number(&grammar) || grammar.p != grammar.end) return false;
    bool negative = *s.p == '-';
    if (negative) ++s.p;
    const char *mantissa = s.p;
    int before = 0, total = 0, leading = 0;
    bool fraction = false, nonzero = false;
    while (s.p < s.end && (*s.p == '.' || digit(*s.p))) {
        if (*s.p == '.') fraction = true;
        else {
            if (!fraction) ++before;
            ++total;
            if (!nonzero && *s.p == '0') ++leading;
            else nonzero = true;
        }
        ++s.p;
    }
    const char *mantissa_end = s.p;
    int exponent = 0;
    if (s.p < s.end) {
        ++s.p; /* e/E, already validated */
        bool minus = s.p < s.end && *s.p == '-';
        if (s.p < s.end && (*s.p == '+' || *s.p == '-')) ++s.p;
        while (s.p < s.end) {
            int d = *s.p++ - '0';
            if (exponent < (int)BMB_JSON_MAX_BYTES) exponent = exponent * 10 + d;
        }
        if (minus) exponent = -exponent;
    }
    const int decimal = before + exponent;
    if (nonzero && decimal - leading > 10) return false;
    /* Leading zeros before the decimal cannot overflow; at most ten other
     * integral digits were admitted. Accumulate them in a wider type. */
    uint64_t magnitude = 0;
    int position = 0;
    unsigned rounding = 0;
    for (const char *p = mantissa; p < mantissa_end; ++p) {
        if (*p == '.') continue;
        unsigned d = (unsigned)(*p - '0');
        if (position < decimal) magnitude = magnitude * 10U + d;
        else if (position == decimal) rounding = d;
        ++position;
    }
    if (nonzero) {
        for (int i = total; i < decimal; ++i) magnitude *= 10U;
    }
    if (decimal >= 0 && rounding >= 5) ++magnitude;
    const uint64_t limit = negative ? (uint64_t)INT_MAX + 1U : (uint64_t)INT_MAX;
    if (magnitude > limit) return false;
    *out = negative ? (magnitude == (uint64_t)INT_MAX + 1U ? INT_MIN : -(int)magnitude)
                    : (int)magnitude;
    return true;
}
