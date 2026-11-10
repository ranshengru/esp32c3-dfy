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
    // v0.5
    char place[32];
    int lat_e6;
    int lon_e6;
    bool has_geo;
    bool geo_fixed;
    char wx_today[24];
    char wx_tomorrow[24];
    int mode;            // 0 companion, 1 birthday
    bool birthday_now;   // birthday screen is showing (mode 1 or auto on the date)
    char bname[32];
    int bmonth;
    int bday;
    bool bauto;
    int days_to_birthday; // -1 when no birthday is set
    char msg[40];
    bool rpause;
    int llm_today;
    int llm_cap;
    bool ap_active;
    char ap_ssid[24];
    int expr_on;
    char assets[80];
    unsigned heap_free;
    unsigned heap_min;
    unsigned uptime_s;
    int sweep_cm;
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
    bool geo_auto; // city given without coordinates: drop fixed coordinates and geocode
} NetUpdate;

void net_snapshot(NetSnapshot *out);
// v0.5 settings shared by the web page and USB serial. All persist in NVS.
bool net_set_location(int lat_e6, int lon_e6, const char *name);
void net_set_mode(int mode);
void net_set_expr(int on);
bool net_set_birthday(const char *name, int month, int day, bool auto_switch);
void net_set_message(const char *text);
void net_set_pause(bool paused);
void net_apply_update(const NetUpdate *update);

#endif
