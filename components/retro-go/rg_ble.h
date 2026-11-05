#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef RG_ENABLE_BLE_GAMEPAD
void rg_ble_init(void);
uint32_t rg_ble_gamepad_state(void);
#if RG_RUNTIME_DIAGNOSTICS
uint32_t rg_ble_gamepad_buttons(void);
#endif
bool rg_ble_gamepad_connected(void);
#if CONFIG_BT_BLUEDROID_ENABLED
#define RG_BT_MAX_DEVICES 8
typedef struct
{
    uint8_t address[6];
    char name[32];
} rg_bt_device_t;
bool rg_bt_enabled(void);
const char *rg_bt_status(void);
bool rg_bt_set_enabled(bool enabled);
bool rg_bt_scan(void);
bool rg_bt_connect(int index);
bool rg_bt_reconnect(void);
bool rg_bt_save(void);
bool rg_bt_device_get(int index, rg_bt_device_t *device);
bool rg_bt_saved_get(rg_bt_device_t *device);
void rg_bt_shutdown(void);
#endif
#else
#define rg_ble_init()
#define rg_ble_gamepad_state() 0
#define rg_ble_gamepad_buttons() 0
#define rg_ble_gamepad_connected() false
#endif
