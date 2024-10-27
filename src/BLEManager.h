#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "Config.h"

void setupBLE();
void BLEManagerLoop();
void set_all_values();
void print_all_values();
void notify_all_characteristics();

#endif // BLE_MANAGER_H