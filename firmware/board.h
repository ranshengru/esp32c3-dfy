#ifndef COMPANION_BOARD_H
#define COMPANION_BOARD_H

#include <stdint.h>

enum { WIDTH = 128, HEIGHT = 160 };

void delay_ms(uint32_t ms);
void log_usb(const char *line);
int usb_read_byte(void);
uint32_t timer_ticks(void);
void init_sonar(void);
unsigned distance_mm(void);
void init_display(void);
void configure_hardware_spi(unsigned divider);
void begin_rectangle(unsigned x, unsigned y, unsigned w, unsigned h);
// Input must be 4-byte aligned and the byte count must be a multiple of 64.
void write_pixels(const uint8_t *bytes, unsigned size);
void end_rectangle(void);

#endif
