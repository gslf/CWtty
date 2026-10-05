/* Exercise the private audio callback with an in-memory device. Including
 * the implementation here avoids opening real devices or exposing test-only
 * state in the public API. This target does not link cwtty_core. */
#include "../src/audio.c"

static int failures;

#define CHECK(condition, message)                    \
    do {                                             \
        if (!(condition)) {                          \
            fprintf(stderr, "FAIL: %s\n", message); \
            failures++;                              \
        }                                            \
    } while (0)

static int setup(void)
{
    memset(&A, 0, sizeof A);
    pending_valid = 0;
    if (ma_rb_init(EVENT_RING_BYTES, NULL, NULL, &A.events) != MA_SUCCESS)
        return 0;
    if (ma_rb_init(SEQ_RING_BYTES, NULL, NULL, &A.seq) != MA_SUCCESS) {
        ma_rb_uninit(&A.events);
        return 0;
    }
    A.play_ok = 1;
    A.play.sampleRate = 48000;
    A.play.playback.channels = 1;
    at_set(&A.wpm, 20);
    at_set(&A.rise_ms, 5);
    keyer_init(&A.keyer, KEYER_IAMBIC_B, 2880);
    return 1;
}

static void teardown(void)
{
    ma_rb_uninit(&A.events);
    ma_rb_uninit(&A.seq);
}

static void test_idle_after_rebase(void)
{
    /* Ten seconds of history, followed by a new device clock origin. */
    A.base_ms = 10000.0;
    A.base_samples = 480000;
    A.play.sampleRate = 44100;
    at_set64(&A.clock, 524100);
    at_set64(&A.op_last_active, 515280);
    CHECK(fabs(audio_clock_ms() - 11000.0) < 0.001, "rebased absolute clock");
    CHECK(fabs(audio_operator_idle_ms() - 200.0) < 0.001, "rebasing changed a 200 ms idle duration");
    at_set(&A.straight_level, 1);
    CHECK(audio_operator_idle_ms() == 0.0, "held key reported idle time");
    at_set(&A.straight_level, 0);
    A.play.sampleRate = 48000;
}

static void test_playback(void)
{
    float output[20000];
    audio_event events[8];
    int n, i;
    static const uint64_t expected[] = {480, 3360, 12000, 14880};
    at_set64(&A.clock, 0);
    audio_play_text("EE", 20, 0, 10);
    CHECK(audio_playback_busy(), "queued text not busy");
    playback_callback(&A.play, output, NULL, ARRAY_LEN(output));
    n = audio_poll_events(events, ARRAY_LEN(events));
    CHECK(n == ARRAY_LEN(expected), "wrong number of playback transitions");
    for (i = 0; i < n && i < ARRAY_LEN(expected); i++)
        CHECK(events[i].t == expected[i] && events[i].on == !(i & 1) &&
                  events[i].src == SRC_PLAYBACK, "incorrect sample-accurate playback timing");
    CHECK(!audio_playback_busy(), "finished text still busy");
}

int main(void)
{
    if (!setup()) {
        fprintf(stderr, "Could not allocate audio test buffers\n");
        return 1;
    }
    test_idle_after_rebase();
    test_playback();
    teardown();
    if (failures)
        return 1;
    puts("audio tests passed");
    return 0;
}
