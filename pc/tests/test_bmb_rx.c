// SPDX-License-Identifier: Apache-2.0
// Standalone native regression; no ESP-IDF, MQTT client, or parser dependency.
#include "bmb_rx.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SMALL_CAPACITY 128u
#define LARGE_LENGTH (64u * 1024u)

static const char topic[] = "device/0123456789ABCDEF/report";
static const char message[] =
    "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":42}}";
static const size_t topic_len = sizeof(topic) - 1;
static const size_t message_len = sizeof(message) - 1;
static bmb_rx_t rx;
static unsigned char guarded[SMALL_CAPACITY + 2];
static char large_storage[LARGE_LENGTH + 1];
static char large_message[LARGE_LENGTH + 1];

static void check_guards(size_t capacity)
{
    assert(guarded[0] == 0xa5);
    assert(guarded[capacity + 1] == 0xa5);
}

static void fresh_capacity(size_t capacity)
{
    assert(capacity <= SMALL_CAPACITY);
    memset(guarded, 0xa5, sizeof(guarded));
    bmb_rx_init(&rx, (char *)&guarded[1], capacity);
    check_guards(capacity);
}

static void fresh(void)
{
    fresh_capacity(SMALL_CAPACITY);
}

static void check_clear(void)
{
    assert(rx.received == 0 && rx.length == 0 && rx.topic_len == 0);
    assert(rx.topic[0] == '\0' && !rx.active);
    if (rx.data != NULL && rx.capacity > 0) {
        assert(rx.data[0] == '\0');
    }
}

static void check_complete(const char *expected, size_t length,
                           const char *expected_topic, size_t expected_topic_len)
{
    assert(!rx.active && rx.received == length && rx.length == length);
    assert(memcmp(rx.data, expected, length) == 0);
    assert(rx.data[length] == '\0');
    assert(rx.topic_len == expected_topic_len);
    assert(memcmp(rx.topic, expected_topic, expected_topic_len) == 0);
    assert(rx.topic[expected_topic_len] == '\0');
}

static void start_partial(void)
{
    fresh();
    assert(bmb_rx_feed(&rx, topic, topic_len, "ab", 2, 6, 0) == BMB_RX_WAIT);
    assert(rx.active && rx.received == 2 && rx.length == 6);
    assert(rx.data[2] == '\0');
}

static void test_splits(void)
{
    fresh();
    assert(bmb_rx_feed(&rx, topic, topic_len, message, message_len,
                       message_len, 0) == BMB_RX_COMPLETE);
    check_complete(message, message_len, topic, topic_len);
    for (size_t split = 1; split < message_len; ++split) {
        /* Test both repeated and absent continuation topics at every split. */
        for (unsigned repeat_topic = 0; repeat_topic < 2; ++repeat_topic) {
            fresh();
            assert(bmb_rx_feed(&rx, topic, topic_len, message, split,
                               message_len, 0) == BMB_RX_WAIT);
            assert(rx.active && rx.received == split && rx.length == message_len);
            assert(rx.data[split] == '\0');
            assert(memcmp(rx.data, message, split) == 0);
            assert(bmb_rx_feed(&rx, repeat_topic ? topic : NULL,
                               repeat_topic ? topic_len : 0,
                               message + split, message_len - split,
                               message_len, split) == BMB_RX_COMPLETE);
            check_complete(message, message_len, topic, topic_len);
            check_guards(SMALL_CAPACITY);
        }
    }
    fresh();
    for (size_t offset = 0; offset < message_len; ++offset) {
        const bmb_rx_result_t expected = offset + 1 == message_len
                                            ? BMB_RX_COMPLETE : BMB_RX_WAIT;
        assert(bmb_rx_feed(&rx, topic, offset == 0 ? topic_len : 0,
                           message + offset, 1, message_len, offset) == expected);
    }
    check_complete(message, message_len, topic, topic_len);
    puts("PASS all split positions and one-byte fragments");
}

static void test_large_message(void)
{
    const char prefix[] = "{\"print\":{\"padding\":\"";
    const char suffix[] = "\"}}";
    /* A syntactically valid 64 KiB JSON document, at the exact payload limit. */
    memset(large_message, 'x', LARGE_LENGTH);
    memcpy(large_message, prefix, sizeof(prefix) - 1);
    memcpy(large_message + LARGE_LENGTH - (sizeof(suffix) - 1),
           suffix, sizeof(suffix));
    bmb_rx_init(&rx, large_storage, sizeof(large_storage));
    size_t offset = 0;
    while (offset < LARGE_LENGTH) {
        size_t count = LARGE_LENGTH - offset;
        if (count > 4093) {
            count = 4093;
        }
        const bmb_rx_result_t expected = count == LARGE_LENGTH - offset
                                            ? BMB_RX_COMPLETE : BMB_RX_WAIT;
        assert(bmb_rx_feed(&rx, offset == 0 ? topic : NULL,
                           offset == 0 ? topic_len : 0,
                           large_message + offset, count,
                           LARGE_LENGTH, offset) == expected);
        offset += count;
    }
    check_complete(large_message, LARGE_LENGTH, topic, topic_len);
    assert(bmb_rx_feed(&rx, topic, topic_len, "x", 1,
                       LARGE_LENGTH + 1, 0) == BMB_RX_DROP);
    check_clear();
    /* The same maximum-sized message also works in a single delivery. */
    assert(bmb_rx_feed(&rx, topic, topic_len, large_message, LARGE_LENGTH,
                       LARGE_LENGTH, 0) == BMB_RX_COMPLETE);
    check_complete(large_message, LARGE_LENGTH, topic, topic_len);
    puts("PASS 64 KiB valid JSON and oversized rejection");
}

static void test_reset_and_replacement(void)
{
    start_partial();
    char *const storage = rx.data;
    bmb_rx_reset(&rx); /* Disconnect or externally detected truncation. */
    check_clear();
    assert(rx.data == storage && rx.capacity == SMALL_CAPACITY);
    assert(bmb_rx_feed(&rx, NULL, 0, "cdef", 4, 6, 2) == BMB_RX_DROP);
    check_clear();
    bmb_rx_reset(&rx);
    check_clear();

    start_partial();
    assert(bmb_rx_feed(&rx, "new", 3, "xyz", 3, 3, 0) == BMB_RX_COMPLETE);
    check_complete("xyz", 3, "new", 3);
    /* No continuations are accepted after a frame has completed. */
    assert(bmb_rx_feed(&rx, NULL, 0, "def", 3, 6, 3) == BMB_RX_DROP);
    check_clear();

    start_partial();
    assert(bmb_rx_feed(&rx, "new", 3, "1", 1, 2, 0) == BMB_RX_WAIT);
    assert(bmb_rx_feed(&rx, NULL, 0, "2", 1, 2, 1) == BMB_RX_COMPLETE);
    check_complete("12", 2, "new", 3);
    /* Even an invalid offset-zero replacement invalidates an old partial. */
    start_partial();
    assert(bmb_rx_feed(&rx, NULL, 0, "x", 1, 1, 0) == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, NULL, 0, "cdef", 4, 6, 2) == BMB_RX_DROP);
    check_clear();
    /* Reinitialization switches storage ownership and clears an old partial. */
    start_partial();
    bmb_rx_init(&rx, large_storage, sizeof(large_storage));
    check_clear();
    assert(rx.data == large_storage && rx.capacity == sizeof(large_storage));
    puts("PASS reset, disconnect, replacement, and stale continuations");
}

static void test_bad_continuations(void)
{
    const struct {
        const char *topic;
        size_t topic_len;
        const char *data;
        size_t data_len;
        size_t total;
        size_t offset;
    } bad[] = {
        {NULL, 0, "x", 1, 6, 1},       /* Overlapping/out-of-order offset. */
        {NULL, 0, "x", 1, 6, 3},       /* Missing bytes. */
        {NULL, 0, "x", 1, 7, 2},       /* Total changed. */
        {"other", 5, "x", 1, 6, 2},   /* Topic length changed. */
        {"Device/0123456789ABCDEF/report", sizeof(topic) - 1, "x", 1, 6, 2},
        {NULL, 1, "x", 1, 6, 2},       /* NULL nonempty topic. */
        {topic, SIZE_MAX, "x", 1, 6, 2},
        {NULL, 0, NULL, 1, 6, 2},
        {NULL, 0, "x", 0, 6, 2},       /* No progress. */
        {NULL, 0, "x", 1, 0, 2},
        {NULL, 0, "x", 1, 6, 6},       /* Past the declared end. */
        {NULL, 0, "x", 1, 6, 7},
        {NULL, 0, "12345", 5, 6, 2},   /* Fragment exceeds remaining bytes. */
        {NULL, 0, "x", SIZE_MAX, 6, 2},
        {NULL, 0, "x", 1, SIZE_MAX, 2},
        {NULL, 0, "x", 1, 6, SIZE_MAX},
        {NULL, 0, "x", SIZE_MAX, SIZE_MAX, SIZE_MAX},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        start_partial();
        assert(bmb_rx_feed(&rx, bad[i].topic, bad[i].topic_len,
                           bad[i].data, bad[i].data_len,
                           bad[i].total, bad[i].offset) == BMB_RX_DROP);
        check_clear();
        check_guards(SMALL_CAPACITY);
        assert(bmb_rx_feed(&rx, NULL, 0, "cdef", 4, 6, 2) == BMB_RX_DROP);
    }
    start_partial();
    assert(bmb_rx_feed(&rx, NULL, 0, "cd", 2, 6, 2) == BMB_RX_WAIT);
    assert(bmb_rx_feed(&rx, NULL, 0, "cd", 2, 6, 2) == BMB_RX_DROP);
    check_clear();
    fresh();
    assert(bmb_rx_feed(&rx, topic, topic_len, "cd", 2, 6, 2) == BMB_RX_DROP);
    check_clear();
    puts("PASS duplicate, out-of-order, mismatched, and overflowing fragments");
}

static void test_topics_and_binary_lengths(void)
{
    char max_topic[BMB_RX_TOPIC_CAPACITY];
    memset(max_topic, 't', sizeof(max_topic)); /* Deliberately unterminated. */
    const char bytes[] = {'a', '\0', 'b'};
    fresh();
    assert(bmb_rx_feed(&rx, max_topic, sizeof(max_topic) - 1,
                       bytes, sizeof(bytes), sizeof(bytes), 0) == BMB_RX_COMPLETE);
    check_complete(bytes, sizeof(bytes), max_topic, sizeof(max_topic) - 1);
    assert(bmb_rx_feed(&rx, max_topic, sizeof(max_topic), "x", 1, 1, 0)
           == BMB_RX_DROP);
    check_clear();
    assert(bmb_rx_feed(&rx, "a\0b", 3, "x", 1, 1, 0) == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, NULL, topic_len, "x", 1, 1, 0) == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, NULL, 0, "x", 1, 1, 0) == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, topic, 0, "x", 1, 1, 0) == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, topic, SIZE_MAX, "x", 1, 1, 0) == BMB_RX_DROP);
    check_clear();
    /* Match counted bytes, without inspecting bytes after the supplied range. */
    const char first[] = {'a', 'b', 'c', 'X'};
    const char next[] = {'a', 'b', 'c', 'Y'};
    assert(bmb_rx_feed(&rx, first, 3, bytes, 1, sizeof(bytes), 0) == BMB_RX_WAIT);
    assert(bmb_rx_feed(&rx, next, 3, bytes + 1, sizeof(bytes) - 1,
                       sizeof(bytes), 1) == BMB_RX_COMPLETE);
    check_complete(bytes, sizeof(bytes), first, 3);
    char embedded_nul_topic[sizeof(topic)];
    memcpy(embedded_nul_topic, topic, sizeof(topic));
    embedded_nul_topic[6] = '\0';
    start_partial();
    assert(bmb_rx_feed(&rx, embedded_nul_topic, topic_len, "cdef", 4, 6, 2)
           == BMB_RX_DROP);
    check_clear();
    puts("PASS topic bounds, counted inputs, and embedded payload NULs");
}

static void test_capacity_bounds(void)
{
    char payload[SMALL_CAPACITY];
    memset(payload, 'p', sizeof(payload));
    for (size_t capacity = 2; capacity <= SMALL_CAPACITY; ++capacity) {
        fresh_capacity(capacity);
        assert(bmb_rx_feed(&rx, topic, topic_len, payload, capacity - 1,
                           capacity - 1, 0) == BMB_RX_COMPLETE);
        check_complete(payload, capacity - 1, topic, topic_len);
        check_guards(capacity);
        assert(bmb_rx_feed(&rx, topic, topic_len, payload, capacity,
                           capacity, 0) == BMB_RX_DROP);
        check_clear();
        check_guards(capacity);
    }
    fresh();
    assert(bmb_rx_feed(&rx, topic, topic_len, "x", 1, SMALL_CAPACITY, 0)
           == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, topic, topic_len, "x", SIZE_MAX, 1, 0)
           == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, topic, topic_len, "x", 1, SIZE_MAX, 0)
           == BMB_RX_DROP);
    check_clear();
    check_guards(SMALL_CAPACITY);
    puts("PASS exact capacities, terminator space, and SIZE_MAX bounds");
}

static void test_null_and_empty(void)
{
    bmb_rx_init(NULL, NULL, 0);
    bmb_rx_reset(NULL);
    assert(bmb_rx_feed(NULL, NULL, 0, NULL, 0, 0, 0) == BMB_RX_DROP);
    bmb_rx_init(&rx, NULL, 0);
    assert(bmb_rx_feed(&rx, topic, topic_len, "x", 1, 1, 0) == BMB_RX_DROP);
    check_clear();
    bmb_rx_init(&rx, NULL, SMALL_CAPACITY);
    assert(bmb_rx_feed(&rx, topic, topic_len, "x", 1, 1, 0) == BMB_RX_DROP);
    check_clear();
    for (size_t capacity = 0; capacity < 2; ++capacity) {
        fresh_capacity(capacity);
        assert(bmb_rx_feed(&rx, topic, topic_len, "x", 1, 1, 0) == BMB_RX_DROP);
        assert(bmb_rx_feed(&rx, topic, topic_len, NULL, 0, 0, 0) == BMB_RX_DROP);
        check_clear();
        check_guards(capacity);
    }
    fresh();
    assert(bmb_rx_feed(&rx, topic, topic_len, NULL, 1, 1, 0) == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, topic, topic_len, "", 0, 0, 0) == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, topic, topic_len, "", 0, 1, 0) == BMB_RX_DROP);
    assert(bmb_rx_feed(&rx, topic, topic_len, "x", 1, 0, 0) == BMB_RX_DROP);
    check_clear();
    assert(bmb_rx_feed(&rx, topic, topic_len, "x", 1, 1, 0) == BMB_RX_COMPLETE);
    check_complete("x", 1, topic, topic_len);
    assert(bmb_rx_feed(&rx, NULL, 0, "", 0, 1, 1) == BMB_RX_DROP);
    check_clear();
    puts("PASS NULL arguments, empty frames/fragments, and disabled storage");
}

int main(void)
{
    test_splits();
    test_large_message();
    test_reset_and_replacement();
    test_bad_continuations();
    test_topics_and_binary_lengths();
    test_capacity_bounds();
    test_null_and_empty();
    puts("All bmb_rx tests passed.");
    return 0;
}
