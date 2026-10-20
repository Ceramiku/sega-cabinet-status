#include "cabinet_core.h"
#include <string.h>

void cab_defaults(cab_config *config) {
    config->hold_seconds = 45;
    config->guest_seconds = 6;
    config->idle_seconds = 0;
    config->signal_seconds = 60;
    config->reader_address = 0;
}

void cab_init(cab_state *state, uint64_t now_ms) {
    memset(state, 0, sizeof(*state));
    state->reason = "initializing";
    state->last_signal_ms = now_ms;
}

const char *cab_phase_name(cab_phase phase) {
    return phase == CAB_FREE ? "free" : phase == CAB_OCCUPIED ? "occupied" : "unknown";
}

static void cab_change(cab_state *state, cab_phase phase, const char *reason, uint64_t now_ms) {
    if (phase != state->phase || strcmp(reason, state->reason) != 0) {
        state->revision++;
    }
    if (phase == CAB_OCCUPIED && state->phase != CAB_OCCUPIED) {
        state->occupied_since_ms = now_ms;
    }
    state->phase = phase;
    state->reason = reason;
    if (phase == CAB_FREE) {
        state->observed_idle = 1;
        state->radio_off = 0;
    }
}

static void cab_frame(cab_state *state, const cab_config *config,
                      const uint8_t *frame, uint64_t now_ms) {
    uint8_t command = frame[3];
    if (frame[1] != config->reader_address) {
        return;
    }
    state->valid_frames++;
    state->last_command = command;
    switch (command) {
    case 0x40:
        state->radio_on_requests++;
        state->last_signal_ms = now_ms;
        state->radio_off = 0;
        break;
    case 0x42:
        state->last_signal_ms = now_ms;
        state->radio_off = 0;
        state->polls++;
        if (!state->poll_streak || now_ms - state->last_poll_ms > 1000) {
            state->poll_start_ms = now_ms;
            state->poll_streak = 0;
        }
        state->last_poll_ms = now_ms;
        state->poll_streak++;
        if (state->phase == CAB_UNKNOWN && state->poll_streak >= 3) {
            cab_change(state, CAB_FREE, "nfc_idle_polling", now_ms);
        } else if (state->phase == CAB_OCCUPIED && state->poll_streak >= 8 &&
                   now_ms - state->poll_start_ms >= 1500 &&
                   now_ms - state->occupied_since_ms >= (uint64_t)config->hold_seconds * 1000) {
            cab_change(state, CAB_FREE, "nfc_idle_resumed", now_ms);
        }
        break;
    case 0x43:
    case 0x52:
    case 0x55:
    case 0x71:
        state->last_signal_ms = now_ms;
        state->poll_streak = 0;
        cab_change(state, CAB_OCCUPIED, "nfc_card_request", now_ms);
        break;
    case 0x41:
        state->radio_off_requests++;
        state->last_signal_ms = now_ms;
        if (!state->radio_off) {
            state->radio_off = 1;
            state->off_since_ms = now_ms;
        }
        break;
    default:
        break;
    }
}

void cab_feed(cab_state *state, const cab_config *config, cab_decoder *decoder,
              const uint8_t *bytes, size_t length, uint64_t now_ms) {
    size_t offset;
    if (!bytes) {
        return;
    }
    for (offset = 0; offset < length; offset++) {
        uint8_t value = bytes[offset];
        if (value == 0xE0) {
            if (decoder->active && decoder->used) {
                state->invalid_frames++;
            }
            decoder->used = 0;
            decoder->escaped = 0;
            decoder->active = 1;
            continue;
        }
        if (!decoder->active) {
            continue;
        }
        if (decoder->escaped) {
            value = (uint8_t)(value + 1);
            decoder->escaped = 0;
        } else if (value == 0xD0) {
            decoder->escaped = 1;
            continue;
        }
        if (decoder->used >= sizeof(decoder->bytes)) {
            decoder->active = 0;
            state->invalid_frames++;
            continue;
        }
        decoder->bytes[decoder->used++] = value;
        if (decoder->bytes[0] < 5) {
            decoder->active = 0;
            state->invalid_frames++;
        } else if (decoder->used == (size_t)decoder->bytes[0] + 1) {
            size_t position;
            uint8_t checksum = 0;
            size_t frame_length = decoder->bytes[0];
            for (position = 0; position < frame_length; position++) {
                checksum = (uint8_t)(checksum + decoder->bytes[position]);
            }
            if (checksum == decoder->bytes[frame_length] &&
                decoder->bytes[4] == frame_length - 5) {
                cab_frame(state, config, decoder->bytes, now_ms);
            } else {
                state->invalid_frames++;
            }
            decoder->active = 0;
            decoder->used = 0;
        }
    }
}

static int cab_endpoint(const uint8_t *bytes, size_t length, const char *endpoint) {
    size_t offset = 0;
    size_t endpoint_start = 0;
    size_t endpoint_length = strlen(endpoint);
    if (length >= 5 && (!memcmp(bytes, "POST ", 5) || !memcmp(bytes, "GET /", 5))) {
        while (offset < length && bytes[offset] != ' ') {
            offset++;
        }
        offset++;
    }
    if (offset >= length || bytes[offset] != '/') {
        return 0;
    }
    for (; offset < length && bytes[offset] != '?' && bytes[offset] != ' ' &&
           bytes[offset] != '\r' && bytes[offset] != '\n'; offset++) {
        if (bytes[offset] == '/') {
            endpoint_start = offset + 1;
        }
    }
    return offset - endpoint_start == endpoint_length &&
           !memcmp(bytes + endpoint_start, endpoint, endpoint_length);
}

void cab_network(cab_state *state, const uint8_t *bytes, size_t length, uint64_t now_ms) {
    int login = 0;
    if (!bytes || length < 4) {
        return;
    }
    if (length >= 32 && bytes[0] == 0x3E && bytes[1] == 0xA1) {
        uint16_t command = (uint16_t)(bytes[4] | (bytes[5] << 8));
        login = command == 1 || command == 4 || command == 9 || command == 15 || command == 17;
    }
    login = login || cab_endpoint(bytes, length, "UserLoginApi") ||
            cab_endpoint(bytes, length, "GameLoginApi") ||
            cab_endpoint(bytes, length, "UpsertUserAllApi") ||
            cab_endpoint(bytes, length, "UploadUserPlaylogApi");
    if (login) {
        state->last_signal_ms = now_ms;
        state->poll_streak = 0;
        cab_change(state, CAB_OCCUPIED, "network_player_request", now_ms);
    } else if (cab_endpoint(bytes, length, "UserLogoutApi") ||
               cab_endpoint(bytes, length, "GameLogoutApi")) {
        if (state->phase == CAB_OCCUPIED) {
            state->last_signal_ms = now_ms;
            state->poll_streak = 0;
        }
    }
}

void cab_tick(cab_state *state, const cab_config *config, uint64_t now_ms) {
    if (state->phase == CAB_FREE && state->observed_idle && state->radio_off &&
        config->guest_seconds && now_ms - state->off_since_ms >= (uint64_t)config->guest_seconds * 1000) {
        cab_change(state, CAB_OCCUPIED, "nfc_radio_off_heuristic", now_ms);
    }
    if (state->phase == CAB_OCCUPIED && config->idle_seconds &&
        now_ms - state->last_signal_ms >= (uint64_t)config->idle_seconds * 1000) {
        state->radio_off = 0;
        state->poll_streak = 0;
        cab_change(state, CAB_UNKNOWN, "occupied_signal_expired", now_ms);
    } else if (state->phase == CAB_FREE && config->signal_seconds &&
               now_ms - state->last_signal_ms >= (uint64_t)config->signal_seconds * 1000) {
        state->radio_off = 0;
        state->poll_streak = 0;
        cab_change(state, CAB_UNKNOWN, "idle_signal_expired", now_ms);
    }
}
