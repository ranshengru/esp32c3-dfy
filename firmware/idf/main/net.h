#ifndef COMPANION_NET_H
#define COMPANION_NET_H

#include "display.h"

#include <stdbool.h>
#include <stdint.h>

void net_init(void);
void net_compose(UiState *ui);
void net_request_fetch(void);
// One sweep talks, or dismisses a reminder. A second sweep within 2.5s
// refreshes the balance so a casual wave does not spend an API call.
void net_on_sweep(void);
void net_handle_line(char *line);

typedef struct {
    char ssid[32];
    char city[32];
    char weather[24];
    char total[16];
    char currency[8];
    char ip[20];
    char wifi[24];
    char detail[40];
    char clock[8];
    int drink_min;
    int move_min;
    int quiet_start;
    int quiet_end;
    int tz_hours;
    int drinks_today;
    bool key_set;
    bool connected;
    bool time_ok;
} NetSnapshot;

typedef struct {
    char ssid[32];
    char pass[64];
    char key[128];
    char city[32];
    int drink_min;
    int move_min;
    int quiet_start;
    int quiet_end;
    int tz_hours;
    bool has_ssid;
    bool has_pass;
    bool open_net;
    bool has_key;
    bool clear_key;
    bool has_city;
    bool has_nums;
} NetUpdate;

void net_snapshot(NetSnapshot *out);
void net_apply_update(const NetUpdate *update);

#endif
