#include "cabinet_core.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, label) do { if (!(condition)) { printf("FAIL: %s\n", label); failures++; } } while (0)

static void test_radio_cycles(void) {
    const uint8_t on[] = {0xE0, 5, 0, 1, 0x40, 0, 0x46};
    const uint8_t poll[] = {0xE0, 5, 0, 1, 0x42, 0, 0x48};
    const uint8_t off[] = {0xE0, 5, 0, 1, 0x41, 0, 0x47};
    const uint8_t card[] = {0xE0, 9, 0, 1, 0x43, 4, 1, 2, 3, 4, 0x5B};
    cab_config config;
    cab_state state;
    cab_decoder decoder = {{0}, 0, 0, 0};
    int index;
    int remained_free = 1;
    cab_defaults(&config);
    cab_init(&state, 1000);
    for (index = 0; index < 120; index++) {
        uint64_t timestamp = 1000 + index * 200;
        cab_feed(&state, &config, &decoder, on, sizeof(on), timestamp);
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), timestamp + 20);
        cab_feed(&state, &config, &decoder, off, sizeof(off), timestamp + 40);
        cab_tick(&state, &config, timestamp + 50);
        if (index >= 2 && state.phase != CAB_FREE) remained_free = 0;
    }
    CHECK(remained_free, "ON POLL OFF cycles establish and maintain idle beyond guest timeout");
    CHECK(state.polls == 120 && state.poll_streak == 120 &&
          state.radio_on_requests == 120 && state.radio_off_requests == 120,
          "count real polls separately from radio controls");
    cab_feed(&state, &config, &decoder, card, sizeof(card), 25000);
    CHECK(state.phase == CAB_OCCUPIED && state.poll_streak == 0, "card interrupts accumulated polling");
    for (index = 0; index < 9; index++) {
        uint64_t timestamp = 26000 + index * 250;
        cab_feed(&state, &config, &decoder, on, sizeof(on), timestamp);
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), timestamp + 20);
        cab_feed(&state, &config, &decoder, off, sizeof(off), timestamp + 40);
    }
    CHECK(state.phase == CAB_OCCUPIED, "interleaved cycles respect occupied hold");
    for (index = 0; index < 9; index++) {
        uint64_t timestamp = 71000 + index * 250;
        cab_feed(&state, &config, &decoder, on, sizeof(on), timestamp);
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), timestamp + 20);
        cab_feed(&state, &config, &decoder, off, sizeof(off), timestamp + 40);
    }
    CHECK(state.phase == CAB_FREE, "interleaved cycles resume idle after hold");
    cab_tick(&state, &config, 79040);
    CHECK(state.phase == CAB_OCCUPIED, "sustained radio off still enables guest heuristic");
    cab_init(&state, 1000);
    memset(&decoder, 0, sizeof(decoder));
    for (index = 0; index < 5; index++) {
        cab_feed(&state, &config, &decoder, on, sizeof(on), 1100 + index * 100);
        cab_feed(&state, &config, &decoder, off, sizeof(off), 1150 + index * 100);
    }
    CHECK(state.phase == CAB_UNKNOWN && state.polls == 0, "radio controls alone cannot establish idle");
    for (index = 0; index < 3; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 2000 + index * 1100);
        cab_feed(&state, &config, &decoder, off, sizeof(off), 2050 + index * 1100);
    }
    CHECK(state.phase == CAB_UNKNOWN && state.poll_streak == 1, "long gaps still break polling continuity");
}

static void test_continued_play(void) {
    const uint8_t poll[] = {0xE0, 5, 0, 1, 0x42, 0, 0x48};
    const uint8_t off[] = {0xE0, 5, 0, 1, 0x41, 0, 0x47};
    const uint8_t card[] = {0xE0, 9, 0, 1, 0x43, 4, 1, 2, 3, 4, 0x5B};
    const char *login = "/Maimai2Servlet/UserLoginApi";
    const char *logout = "/Maimai2Servlet/UserLogoutApi";
    cab_config config;
    cab_state state;
    cab_decoder decoder = {{0}, 0, 0, 0};
    int index;
    cab_defaults(&config);
    CHECK(config.idle_seconds == 0, "default disables occupied expiry for continued play");
    cab_init(&state, 1000);
    for (index = 0; index < 3; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 1100 + index * 100);
    }
    cab_feed(&state, &config, &decoder, card, sizeof(card), 2000);
    cab_feed(&state, &config, &decoder, off, sizeof(off), 2100);
    cab_tick(&state, &config, 902100);
    CHECK(state.phase == CAB_OCCUPIED, "continued play remains occupied past fifteen minutes");
    cab_tick(&state, &config, 1082735);
    CHECK(state.phase == CAB_OCCUPIED, "reported eighteen minute silence remains occupied");
    cab_tick(&state, &config, 7200000);
    CHECK(state.phase == CAB_OCCUPIED, "two hour continued play remains occupied without new card events");
    cab_network(&state, (const uint8_t *)logout, strlen(logout), 7201000);
    for (index = 0; index < 3; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 7202000 + index * 250);
    }
    CHECK(state.phase == CAB_OCCUPIED, "brief polling during continue does not release occupied state");
    for (index = 3; index < 8; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 7202000 + index * 250);
    }
    CHECK(state.phase == CAB_FREE, "confirmed idle polling releases long occupied state");
    cab_feed(&state, &config, &decoder, card, sizeof(card), 7205000);
    cab_feed(&state, &config, &decoder, off, sizeof(off), 7205100);
    cab_tick(&state, &config, 14400000);
    CHECK(state.phase == CAB_OCCUPIED, "second player session also survives long silence");
    cab_init(&state, 20000000);
    cab_tick(&state, &config, 30000000);
    CHECK(state.phase == CAB_UNKNOWN, "disabling occupied expiry does not invent initial state");
    cab_network(&state, (const uint8_t *)login, strlen(login), 30001000);
    cab_network(&state, (const uint8_t *)logout, strlen(logout), 30002000);
    CHECK(state.phase == CAB_OCCUPIED, "network logout alone cannot end cabinet occupancy");
    cab_tick(&state, &config, 40000000);
    CHECK(state.phase == CAB_OCCUPIED, "network-only occupied session also survives long silence");
    cab_init(&state, 0);
    memset(&decoder, 0, sizeof(decoder));
    for (index = 0; index < 3; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 100 + index * 100);
    }
    cab_feed(&state, &config, &decoder, off, sizeof(off), 400);
    cab_tick(&state, &config, 6400);
    cab_tick(&state, &config, 3600000);
    CHECK(state.phase == CAB_OCCUPIED, "guest continued play also survives long silence");
}

int main(void) {
    const uint8_t poll[] = {0xE0, 5, 0, 1, 0x42, 0, 0x48};
    const uint8_t off[] = {0xE0, 5, 0, 1, 0x41, 0, 0x47};
    const uint8_t select_card[] = {0xE0, 9, 0, 1, 0x43, 4, 1, 2, 3, 4, 0x5B};
    const uint8_t bad_poll[] = {0xE0, 5, 0, 1, 0x42, 0, 0x49};
    const uint8_t old_wrong_frame[] = {0xE0, 4, 0, 1, 0x42, 0, 0x47};
    const uint8_t escaped_card[] = {0xE0, 6, 0, 1, 0x43, 1, 0xD0, 0xDF, 0x2B};
    cab_config config;
    cab_state state;
    cab_decoder decoder = {{0}, 0, 0, 0};
    uint8_t huge[1100];
    int index;
    cab_defaults(&config);
    cab_init(&state, 1000);
    CHECK(state.phase == CAB_UNKNOWN, "initial state unknown");
    cab_feed(&state, &config, &decoder, poll, 2, 1100);
    cab_feed(&state, &config, &decoder, poll + 2, sizeof(poll) - 2, 1100);
    cab_feed(&state, &config, &decoder, poll, sizeof(poll), 1200);
    cab_feed(&state, &config, &decoder, poll, sizeof(poll), 1300);
    CHECK(state.phase == CAB_FREE && state.polls == 3, "real upstream polls including split write");
    cab_feed(&state, &config, &decoder, select_card, sizeof(select_card), 2000);
    CHECK(state.phase == CAB_OCCUPIED, "real upstream card request");
    for (index = 0; index < 12; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 3000 + index * 200);
    }
    CHECK(state.phase == CAB_OCCUPIED, "second player polling cannot override minimum hold");
    for (index = 0; index < 10; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 50000 + index * 250);
    }
    CHECK(state.phase == CAB_FREE, "sustained idle resumes after hold");
    cab_feed(&state, &config, &decoder, off, sizeof(off), 55000);
    cab_tick(&state, &config, 60999);
    CHECK(state.phase == CAB_FREE, "guest threshold not yet reached");
    cab_tick(&state, &config, 61000);
    CHECK(state.phase == CAB_OCCUPIED, "guest heuristic runs on timer");
    config.idle_seconds = 900;
    cab_tick(&state, &config, 956000);
    CHECK(state.phase == CAB_UNKNOWN, "stale occupied becomes unknown rather than free");
    cab_feed(&state, &config, &decoder, bad_poll, sizeof(bad_poll), 960000);
    cab_feed(&state, &config, &decoder, old_wrong_frame, sizeof(old_wrong_frame), 960100);
    CHECK(state.phase == CAB_UNKNOWN && state.invalid_frames >= 2, "reject bad checksum and old wrong length");
    cab_feed(&state, &config, &decoder, escaped_card, 7, 961000);
    cab_feed(&state, &config, &decoder, escaped_card + 7, sizeof(escaped_card) - 7, 961000);
    CHECK(state.phase == CAB_OCCUPIED, "escaped payload split across writes");
    memset(huge, 0xAA, sizeof(huge));
    huge[0] = 0xE0;
    cab_feed(&state, &config, &decoder, huge, sizeof(huge), 962000);
    cab_feed(&state, &config, &decoder, poll, sizeof(poll), 962200);
    CHECK(state.polls >= 26, "decoder recovers after malformed input");
    cab_init(&state, 1000);
    cab_network(&state, (const uint8_t *)"random UserLoginApi payload", 26, 2000);
    CHECK(state.phase == CAB_UNKNOWN, "do not classify arbitrary payload substring");
    cab_network(&state, (const uint8_t *)"POST /Maimai2Servlet/UserLoginApi HTTP/1.1\r\n", strlen("POST /Maimai2Servlet/UserLoginApi HTTP/1.1\r\n"), 3000);
    CHECK(state.phase == CAB_OCCUPIED, "HTTP login endpoint");
    cab_network(&state, (const uint8_t *)"/Maimai2Servlet/UserLogoutApi", strlen("/Maimai2Servlet/UserLogoutApi"), 4000);
    CHECK(state.phase == CAB_OCCUPIED, "logout waits for confirmed cabinet idle");
    cab_init(&state, 1000);
    memset(&decoder, 0, sizeof(decoder));
    for (index = 0; index < 3; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 1100 + index * 100);
    }
    cab_tick(&state, &config, 61300);
    CHECK(state.phase == CAB_UNKNOWN, "stale idle signal becomes unknown");
    config.guest_seconds = 0;
    cab_init(&state, 1000);
    memset(&decoder, 0, sizeof(decoder));
    for (index = 0; index < 3; index++) {
        cab_feed(&state, &config, &decoder, poll, sizeof(poll), 1100 + index * 100);
    }
    cab_feed(&state, &config, &decoder, off, sizeof(off), 2000);
    cab_tick(&state, &config, 10000);
    CHECK(state.phase == CAB_FREE, "guest heuristic can be disabled");
    test_radio_cycles();
    test_continued_play();
    printf("CORE_TEST_FAILURES=%d\n", failures);
    return failures ? 1 : 0;
}
