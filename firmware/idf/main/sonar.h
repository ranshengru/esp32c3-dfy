#ifndef COMPANION_SONAR_H
#define COMPANION_SONAR_H

#include <stdbool.h>

// HC-SR04 sweep detector. Calls net_on_sweep() once per hand wave.
void sonar_start(void);
int sonar_threshold_cm(void);
bool sonar_set_threshold_cm(int cm); // 5..200, persisted in NVS key "sweep"
void sonar_set_debug(bool on);        // print every reading on serial
void sonar_status(char *out, int size);

#endif
