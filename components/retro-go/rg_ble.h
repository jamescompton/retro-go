#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef RG_ENABLE_BLE_GAMEPAD
void rg_ble_init(void);
uint32_t rg_ble_gamepad_state(void);
bool rg_ble_gamepad_connected(void);
#else
#define rg_ble_init()
#define rg_ble_gamepad_state() 0
#define rg_ble_gamepad_connected() false
#endif
