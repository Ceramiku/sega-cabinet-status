#ifndef CABINET_CORE_H
#define CABINET_CORE_H

#include <stddef.h>
#include <stdint.h>

#define CABINET_VERSION "0.2.2"

typedef enum {
    CAB_UNKNOWN,
    CAB_FREE,
    CAB_OCCUPIED
} cab_phase;

typedef struct {
    uint32_t hold_seconds;
    uint32_t guest_seconds;
    uint32_t idle_seconds;
    uint32_t signal_seconds;
    uint8_t reader_address;
} cab_config;

typedef struct {
    cab_phase phase;
    const char *reason;
    uint32_t revision;
    uint32_t polls;
    uint32_t radio_on_requests;
    uint32_t radio_off_requests;
    uint8_t last_command;
    uint32_t valid_frames;
    uint32_t invalid_frames;
    uint64_t last_signal_ms;
    uint64_t occupied_since_ms;
    uint64_t off_since_ms;
    uint64_t poll_start_ms;
    uint64_t last_poll_ms;
    uint32_t poll_streak;
    int radio_off;
    int observed_idle;
} cab_state;

typedef struct {
    uint8_t bytes[256];
    size_t used;
    int active;
    int escaped;
} cab_decoder;

void cab_defaults(cab_config *config);
void cab_init(cab_state *state, uint64_t now_ms);
void cab_feed(cab_state *state, const cab_config *config, cab_decoder *decoder,
              const uint8_t *bytes, size_t length, uint64_t now_ms);
void cab_network(cab_state *state, const uint8_t *bytes, size_t length, uint64_t now_ms);
void cab_tick(cab_state *state, const cab_config *config, uint64_t now_ms);
const char *cab_phase_name(cab_phase phase);

#endif
