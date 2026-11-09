#ifndef COMPANION_NET_H
#define COMPANION_NET_H

#include "display.h"

#include <stdbool.h>
#include <stdint.h>

void net_init(void);
void net_compose(UiState *ui);
// HC-SR04 sweep and the serial FETCH command both land here.
void net_request_fetch(void);
void net_handle_line(char *line);

#endif
