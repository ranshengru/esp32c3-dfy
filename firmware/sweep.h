#ifndef SWEEP_H
#define SWEEP_H

#include <stdbool.h>
#include <stdint.h>

enum {
    SWEEP_CAPACITY = 16,
    SWEEP_TICKS_PER_MS = 16000,
    SWEEP_WINDOW = 650 * SWEEP_TICKS_PER_MS,
    SWEEP_STABLE = 500 * SWEEP_TICKS_PER_MS,
    SWEEP_THRESHOLD_MM = 200,
    SWEEP_STILL_MM = 40,
};

typedef struct {
    uint32_t tick;
    unsigned mm;
} SweepSample;

typedef struct {
    SweepSample samples[SWEEP_CAPACITY];
    unsigned first;
    unsigned length;
    bool latched;
} SweepGate;

// Return the measured swing in millimeters only when a new sweep fires.
static unsigned update_sweep(SweepGate *gate, uint32_t tick, unsigned mm)
{
    while (gate->length && tick - gate->samples[gate->first].tick > SWEEP_WINDOW) {
        gate->first = (gate->first + 1) % SWEEP_CAPACITY;
        --gate->length;
    }
    if (!mm) {
        if (!gate->length)
            gate->latched = false;
        return 0;
    }

    if (gate->length == SWEEP_CAPACITY) {
        gate->first = (gate->first + 1) % SWEEP_CAPACITY;
        --gate->length;
    }
    const unsigned position = (gate->first + gate->length) % SWEEP_CAPACITY;
    gate->samples[position].tick = tick;
    gate->samples[position].mm = mm;
    ++gate->length;

    unsigned low = mm;
    unsigned high = mm;
    for (unsigned i = 0; i < gate->length; ++i) {
        const unsigned value = gate->samples[(gate->first + i) % SWEEP_CAPACITY].mm;
        if (value < low)
            low = value;
        if (value > high)
            high = value;
    }
    const unsigned swing = high - low;
    if (gate->latched) {
        if (swing <= SWEEP_STILL_MM && gate->length >= 4 &&
            tick - gate->samples[gate->first].tick >= SWEEP_STABLE)
            gate->latched = false;
        return 0;
    }
    if (swing > SWEEP_THRESHOLD_MM) {
        gate->latched = true;
        return swing;
    }
    return 0;
}

#endif
