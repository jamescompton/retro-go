#include "rg_system.h"
#include "rg_ble.h"

#ifdef RG_ENABLE_BLE_GAMEPAD

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <ctype.h>

#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if CONFIG_BT_NIMBLE_ENABLED
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_store.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <nimble/nimble_npl.h>
#include <os/os_mbuf.h>
#else
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <esp_hidh.h>
#include <esp_hidh_api.h>
#include <freertos/queue.h>
#endif

#define UUID_HID_SERVICE 0x1812
#define UUID_REPORT_MAP 0x2A4B
#define UUID_REPORT 0x2A4D
#define UUID_PROTOCOL_MODE 0x2A4E
#define UUID_HID_CONTROL_POINT 0x2A4C
#define UUID_REPORT_REFERENCE 0x2908
#define UUID_CCC 0x2902
#define MAX_CHARACTERISTICS 32
#define MAX_REPORT_MAP_SIZE 4096

typedef struct
{
    bool valid;
    uint16_t report_id;
    int buttons_offset;
    int buttons_count;
    int back_offset;
    int back_size;
    int hat_offset;
    int hat_size;
    int hat_min;
    int x_offset;
    int x_size;
    int x_min;
    int x_max;
    int y_offset;
    int y_size;
    int y_min;
    int y_max;
    int lt_offset;
    int lt_size;
    int lt_max;
    int rt_offset;
    int rt_size;
    int rt_max;
} hid_layout_t;

#ifdef RG_GAMEPAD_HID_BUTTON_MAP
static const uint32_t button_map[] = RG_GAMEPAD_HID_BUTTON_MAP;
#else
static const uint32_t button_map[] = {
    RG_KEY_A, RG_KEY_B, RG_KEY_X, RG_KEY_Y, RG_KEY_L, RG_KEY_R, RG_KEY_SELECT, RG_KEY_START,
    RG_KEY_MENU, RG_KEY_OPTION,
};
#endif

static volatile uint32_t ble_keys;
#if RG_RUNTIME_DIAGNOSTICS
static volatile uint32_t diagnostic_buttons;
#endif
static volatile bool ble_connected;
static volatile bool connecting;
static hid_layout_t hid_layout;
#if RG_RUNTIME_DIAGNOSTICS
static void log_hid_bytes(const char *label, uint16_t id, const uint8_t *data, size_t length)
{
    for (size_t offset = 0; offset < length; offset += 16)
    {
        char hex[16 * 3 + 1];
        size_t count = RG_MIN(length - offset, 16);
        for (size_t i = 0; i < count; ++i)
            snprintf(hex + i * 3, 4, "%02x ", data[offset + i]);
        RG_LOGI("HID %s id=%u offset=%u bytes=%s", label, id, (unsigned)offset, hex);
    }
}
#endif
#if CONFIG_BT_NIMBLE_ENABLED
static uint8_t own_address_type;
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
static bool closing;
static struct ble_gatt_svc hid_service;
static struct ble_gatt_chr characteristics[MAX_CHARACTERISTICS];
static size_t characteristic_count;
static size_t current_characteristic;
static uint16_t report_map_handle;
static uint16_t reference_handle;
static uint16_t ccc_handle;
static uint8_t report_ids[MAX_CHARACTERISTICS];
static bool subscribed[MAX_CHARACTERISTICS];
static uint8_t *report_map;
static size_t report_map_length;
static struct ble_npl_callout setup_timeout;
static const char *setup_stage;
static unsigned input_reports;
static size_t configuration_characteristic;

static void start_scan(void);
static int gap_event(struct ble_gap_event *event, void *arg);
static void next_report(void);
static void configure_hid(void);
#endif

static int32_t item_value(const uint8_t *data, int size, bool is_signed)
{
    uint32_t value = 0;
    for (int i = 0; i < size; ++i)
        value |= (uint32_t)data[i] << (8 * i);

    if (is_signed && size > 0 && size < 4 && (value & (1u << (size * 8 - 1))))
        value |= ~((1u << (size * 8)) - 1);
    return (int32_t)value;
}

static void parse_report_map(const uint8_t *data, size_t length)
{
    #if RG_RUNTIME_DIAGNOSTICS
    log_hid_bytes("map", 0, data, length);
    #endif
    hid_layout_t parsed = {
        .buttons_offset = -1,
        .back_offset = -1,
        .hat_offset = -1,
        .x_offset = -1,
        .y_offset = -1,
        .lt_offset = -1,
        .rt_offset = -1,
    };
    int usage_page = 0;
    int report_size = 0;
    int report_count = 0;
    int logical_min = 0;
    int logical_max = 0;
    int bit_offset = 0;
    int usages[16];
    int usage_count = 0;
    int usage_min = -1;
    int collection_depth = 0;
    int gamepad_depth = -1;

    for (size_t i = 0; i < length;)
    {
        uint8_t prefix = data[i++];
        if (prefix == 0xFE)
        {
            if (i + 1 >= length)
                break;
            size_t long_length = data[i];
            i += 2;
            if (long_length > length - i)
                break;
            i += long_length;
            continue;
        }

        int size = prefix & 0x03;
        if (size == 3)
            size = 4;
        if ((size_t)size > length - i)
            break;
        int type = (prefix >> 2) & 0x03;
        int tag = prefix >> 4;
        int32_t value = item_value(&data[i], size,
                                  type == 1 && (tag == 1 || (tag == 2 && logical_min < 0)));
        uint32_t unsigned_value = (uint32_t)value;
        i += size;

        if (type == 1)
        {
            if (tag == 0)
                usage_page = (int)unsigned_value;
            else if (tag == 1)
                logical_min = value;
            else if (tag == 2)
                logical_max = value;
            else if (tag == 7 && unsigned_value <= 32)
                report_size = (int)unsigned_value;
            else if (tag == 9 && unsigned_value <= 64)
                report_count = (int)unsigned_value;
            else if (tag == 8)
            {
                if (bit_offset > 0 && (parsed.buttons_offset >= 0 || parsed.hat_offset >= 0 ||
                                      parsed.x_offset >= 0 || parsed.y_offset >= 0))
                    break;
                parsed.report_id = (uint16_t)unsigned_value;
                bit_offset = 0;
            }
        }
        else if (type == 2)
        {
            if (tag == 0 && usage_count < (int)RG_COUNT(usages))
                usages[usage_count++] = (int)unsigned_value;
            else if (tag == 1)
                usage_min = (int)unsigned_value;
        }
        else if (type == 0 && tag == 10)
        {
            ++collection_depth;
            if (value == 1 && usage_page == 1 && usage_count > 0 &&
                (usages[usage_count - 1] == 4 || usages[usage_count - 1] == 5))
                gamepad_depth = collection_depth;
        }
        else if (type == 0 && tag == 12)
        {
            if (gamepad_depth == collection_depth)
                gamepad_depth = -1;
            --collection_depth;
        }
        else if (type == 0 && tag == 8)
        {
            if (gamepad_depth >= 0 && !(unsigned_value & 1) && report_size > 0 && report_count > 0)
            {
                if (usage_page == 9 && parsed.buttons_offset < 0)
                {
                    parsed.buttons_offset = bit_offset;
                    parsed.buttons_count = RG_MIN(report_size * report_count, 32);
                }
                else if (usage_page == 0x0C && (unsigned_value & 2))
                {
                    for (int k = 0; k < report_count; ++k)
                    {
                        int usage = k < usage_count ? usages[k] : (usage_min >= 0 ? usage_min + k : -1);
                        if (usage == 0x224 && parsed.back_offset < 0)
                        {
                            parsed.back_offset = bit_offset + k * report_size;
                            parsed.back_size = report_size;
                        }
                    }
                }
                else if (usage_page == 0x02 && (unsigned_value & 2))
                {
                    // Simulation controls: Brake (0xC5) is the left trigger, Accelerator (0xC4) the right.
                    for (int k = 0; k < report_count; ++k)
                    {
                        int usage = k < usage_count ? usages[k] : (usage_min >= 0 ? usage_min + k : -1);
                        int offset = bit_offset + k * report_size;
                        if (usage == 0xC5 && parsed.lt_offset < 0)
                        {
                            parsed.lt_offset = offset;
                            parsed.lt_size = report_size;
                            parsed.lt_max = logical_max;
                        }
                        else if (usage == 0xC4 && parsed.rt_offset < 0)
                        {
                            parsed.rt_offset = offset;
                            parsed.rt_size = report_size;
                            parsed.rt_max = logical_max;
                        }
                    }
                }
                else if (usage_page == 1 && (unsigned_value & 2))
                {
                    for (int k = 0; k < report_count; ++k)
                    {
                        int usage = k < usage_count ? usages[k] : (usage_min >= 0 ? usage_min + k : -1);
                        int offset = bit_offset + k * report_size;
                        if (usage == 0x39 && parsed.hat_offset < 0)
                        {
                            parsed.hat_offset = offset;
                            parsed.hat_size = report_size;
                            parsed.hat_min = logical_min;
                        }
                        else if (usage == 0x30 && parsed.x_offset < 0)
                        {
                            parsed.x_offset = offset;
                            parsed.x_size = report_size;
                            parsed.x_min = logical_min;
                            parsed.x_max = logical_max;
                        }
                        else if (usage == 0x31 && parsed.y_offset < 0)
                        {
                            parsed.y_offset = offset;
                            parsed.y_size = report_size;
                            parsed.y_min = logical_min;
                            parsed.y_max = logical_max;
                        }
                    }
                }
            }
            bit_offset += report_size * report_count;
        }

        if (type == 0)
        {
            usage_count = 0;
            usage_min = -1;
        }
    }

    parsed.valid = parsed.buttons_offset >= 0 || parsed.hat_offset >= 0 ||
                   parsed.x_offset >= 0 || parsed.y_offset >= 0;
    hid_layout = parsed;
    RG_LOGI("BLE HID report map: bytes=%u report=%u valid=%d buttons=%d hat=%d x=%d y=%d lt=%d rt=%d",
            (unsigned)length, parsed.report_id, parsed.valid, parsed.buttons_count,
            parsed.hat_offset, parsed.x_offset, parsed.y_offset, parsed.lt_offset, parsed.rt_offset);
}

static uint32_t get_bits(const uint8_t *data, size_t length, int offset, int size, bool is_signed)
{
    uint32_t value = 0;
    if (offset < 0 || size <= 0 || size > 32)
        return 0;

    for (int i = 0; i < size; ++i)
    {
        size_t bit = (size_t)offset + i;
        if (bit / 8 < length && (data[bit / 8] & (1u << (bit % 8))))
            value |= 1u << i;
    }

    if (is_signed && size < 32 && (value & (1u << (size - 1))))
        value |= ~((1u << size) - 1);
    return value;
}

static uint32_t axis_keys(const uint8_t *data, size_t length, int offset, int size,
                          int min_value, int max_value, uint32_t negative, uint32_t positive)
{
    if (offset < 0 || size <= 0 || size > 16 || max_value <= min_value)
        return 0;

    int value = (int32_t)get_bits(data, length, offset, size, min_value < 0);
    int percent = (value - min_value) * 100 / (max_value - min_value);
    return percent < 25 ? negative : percent > 75 ? positive : 0;
}

static bool name_contains(const uint8_t *name, size_t name_length, const char *pattern)
{
    size_t pattern_length = strlen(pattern);
    if (pattern_length > name_length)
        return false;

    for (size_t i = 0; i <= name_length - pattern_length; ++i)
    {
        size_t j = 0;
        while (j < pattern_length)
        {
            uint8_t c = name[i + j];
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if (c != (uint8_t)pattern[j])
                break;
            ++j;
        }
        if (j == pattern_length)
            return true;
    }
    return false;
}

static void handle_gamepad_report(uint16_t report_id, const uint8_t *data, size_t length)
{
    if (!hid_layout.valid || (hid_layout.report_id && hid_layout.report_id != report_id))
        return;

    uint32_t keys = 0;
    uint32_t buttons = get_bits(data, length, hid_layout.buttons_offset, hid_layout.buttons_count, false);
    #if RG_RUNTIME_DIAGNOSTICS
    diagnostic_buttons = buttons;
    #endif
    for (size_t i = 0; i < RG_COUNT(button_map); ++i)
        if (buttons & (1u << i))
            keys |= button_map[i];
    #ifdef RG_GAMEPAD_HID_BACK_KEY
    if (get_bits(data, length, hid_layout.back_offset, hid_layout.back_size, false))
        keys |= RG_GAMEPAD_HID_BACK_KEY;
    #endif
    #ifdef RG_GAMEPAD_HID_LTRIGGER_KEY
    if (hid_layout.lt_offset >= 0 && hid_layout.lt_max > 0 &&
        (int)get_bits(data, length, hid_layout.lt_offset, hid_layout.lt_size, false) > hid_layout.lt_max / 2)
        keys |= RG_GAMEPAD_HID_LTRIGGER_KEY;
    #endif
    #ifdef RG_GAMEPAD_HID_RTRIGGER_KEY
    if (hid_layout.rt_offset >= 0 && hid_layout.rt_max > 0 &&
        (int)get_bits(data, length, hid_layout.rt_offset, hid_layout.rt_size, false) > hid_layout.rt_max / 2)
        keys |= RG_GAMEPAD_HID_RTRIGGER_KEY;
    #endif

    if (hid_layout.hat_offset >= 0 && hid_layout.hat_size <= 8)
    {
        static const uint32_t hat_keys[8] = {
            RG_KEY_UP, RG_KEY_UP | RG_KEY_RIGHT, RG_KEY_RIGHT, RG_KEY_DOWN | RG_KEY_RIGHT,
            RG_KEY_DOWN, RG_KEY_DOWN | RG_KEY_LEFT, RG_KEY_LEFT, RG_KEY_UP | RG_KEY_LEFT,
        };
        int hat = (int)get_bits(data, length, hid_layout.hat_offset, hid_layout.hat_size, false) -
                  hid_layout.hat_min;
        if (hat >= 0 && hat < 8)
            keys |= hat_keys[hat];
    }

    keys |= axis_keys(data, length, hid_layout.x_offset, hid_layout.x_size,
                      hid_layout.x_min, hid_layout.x_max, RG_KEY_LEFT, RG_KEY_RIGHT);
    keys |= axis_keys(data, length, hid_layout.y_offset, hid_layout.y_size,
                      hid_layout.y_min, hid_layout.y_max, RG_KEY_UP, RG_KEY_DOWN);
    #if RG_ENABLE_DEV_MODE
    if (keys != ble_keys)
        RG_LOGI("BLE gamepad keys: 0x%08x", (unsigned)keys);
    #endif
    ble_keys = keys;
}

#if CONFIG_BT_NIMBLE_ENABLED
static bool is_gamepad_advertisement(const struct ble_gap_disc_desc *disc)
{
    bool has_hid_service = false;
    bool has_appearance = false;
    bool has_gamepad_appearance = false;
    bool name_matches = false;
    const uint8_t *name = NULL;
    size_t name_length = 0;
    uint16_t appearance = 0;

    for (size_t i = 0; i < disc->length_data;)
    {
        uint8_t item_length = disc->data[i];
        if (item_length == 0 || (size_t)item_length > disc->length_data - i - 1)
            break;

        uint8_t type = disc->data[i + 1];
        const uint8_t *value = &disc->data[i + 2];
        size_t value_length = item_length - 1;
        if ((type == 0x02 || type == 0x03) && value_length >= 2)
        {
            for (size_t j = 0; j + 1 < value_length; j += 2)
                if ((value[j] | (value[j + 1] << 8)) == UUID_HID_SERVICE)
                    has_hid_service = true;
        }
        else if (type == 0x19 && value_length >= 2)
        {
            appearance = value[0] | (value[1] << 8);
            has_appearance = true;
            has_gamepad_appearance = appearance == 0x03C3 || appearance == 0x03C4;
        }
        else if (type == 0x08 || type == 0x09)
        {
            name = value;
            name_length = value_length;
            if (name_contains(value, value_length, "q36") ||
                name_contains(value, value_length, "gamepad") ||
                name_contains(value, value_length, "controller") ||
                name_contains(value, value_length, "joystick"))
                name_matches = true;
        }
        i += item_length + 1;
    }

    bool matches = has_gamepad_appearance || name_matches || (has_hid_service && !has_appearance);
    RG_LOGI("BLE advertisement %02x:%02x:%02x:%02x:%02x:%02x type=%d rssi=%d "
            "name='%.*s' hid=%d appearance=0x%04x match=%d",
            disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
            disc->addr.val[2], disc->addr.val[1], disc->addr.val[0],
            disc->event_type, disc->rssi, (int)name_length, name ? (const char *)name : "",
            has_hid_service, appearance, matches);
    return matches;
}

static bool setup_active(uint16_t conn_handle)
{
    return conn_handle == connection && connecting && !closing;
}

static void fail_setup(const char *stage, int rc)
{
    RG_LOGE("BLE HID %s failed: %d", stage, rc);
    ble_npl_callout_stop(&setup_timeout);
    closing = true;
    if (connection != BLE_HS_CONN_HANDLE_NONE)
    {
        int terminate_rc = ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        if (terminate_rc != 0)
            RG_LOGE("BLE disconnect request failed: %d", terminate_rc);
    }
}

static void setup_timed_out(struct ble_npl_event *event)
{
    fail_setup(setup_stage, BLE_HS_ETIMEOUT);
}

static void begin_stage(const char *stage)
{
    setup_stage = stage;
    RG_LOGI("BLE HID setup: %s", stage);
    ble_npl_callout_reset(&setup_timeout, ble_npl_time_ms_to_ticks32(15000));
}

static void check_request(int rc)
{
    if (rc != 0)
        fail_setup(setup_stage, rc);
}

static int subscribed_report(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    if (!setup_active(conn_handle))
        return 0;
    if (error->status != 0)
    {
        fail_setup("notification subscription", error->status);
        return 0;
    }
    subscribed[current_characteristic] = true;
    RG_LOGI("BLE HID subscribed: handle=%u report=%u", characteristics[current_characteristic].val_handle,
            report_ids[current_characteristic]);
    ++current_characteristic;
    next_report();
    return 0;
}

static int report_reference_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg)
{
    if (!setup_active(conn_handle))
        return 0;
    uint8_t reference[2];
    if (error->status != 0 || !attr || OS_MBUF_PKTLEN(attr->om) != sizeof(reference) ||
        os_mbuf_copydata(attr->om, 0, sizeof(reference), reference) != 0)
    {
        fail_setup("report reference read", error->status ? error->status : BLE_HS_EBADDATA);
        return 0;
    }
    report_ids[current_characteristic] = reference[0];
    if (reference[1] == 1 && (!hid_layout.report_id || reference[0] == hid_layout.report_id))
    {
        const uint8_t enable_notifications[] = {1, 0};
        begin_stage("notification subscription");
        check_request(ble_gattc_write_flat(connection, ccc_handle, enable_notifications,
                                          sizeof(enable_notifications), subscribed_report, NULL));
    }
    else
    {
        ++current_characteristic;
        next_report();
    }
    return 0;
}

static int descriptors_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  uint16_t chr_val_handle, const struct ble_gatt_dsc *descriptor,
                                  void *arg)
{
    if (!setup_active(conn_handle))
        return 0;
    if (error->status == 0)
    {
        uint16_t uuid = ble_uuid_u16(&descriptor->uuid.u);
        if (uuid == UUID_REPORT_REFERENCE)
            reference_handle = descriptor->handle;
        else if (uuid == UUID_CCC)
            ccc_handle = descriptor->handle;
    }
    else if (error->status == BLE_HS_EDONE)
    {
        if (!reference_handle || !ccc_handle)
        {
            fail_setup("missing report reference/CCC descriptor", BLE_HS_ENOENT);
            return 0;
        }
        begin_stage("report reference read");
        check_request(ble_gattc_read(connection, reference_handle, report_reference_read, NULL));
    }
    else
        fail_setup("descriptor discovery", error->status);
    return 0;
}

static void next_report(void)
{
    while (current_characteristic < characteristic_count)
    {
        struct ble_gatt_chr *chr = &characteristics[current_characteristic];
        if (ble_uuid_u16(&chr->uuid.u) != UUID_REPORT || !(chr->properties & BLE_GATT_CHR_PROP_NOTIFY))
        {
            ++current_characteristic;
            continue;
        }
        uint16_t end = current_characteristic + 1 < characteristic_count ?
                       characteristics[current_characteristic + 1].def_handle - 1 : hid_service.end_handle;
        reference_handle = 0;
        ccc_handle = 0;
        begin_stage("report descriptor discovery");
        check_request(ble_gattc_disc_all_dscs(connection, chr->val_handle, end,
                                             descriptors_discovered, NULL));
        return;
    }

    for (size_t i = 0; i < characteristic_count; ++i)
    {
        if (subscribed[i])
        {
            ble_npl_callout_stop(&setup_timeout);
            connecting = false;
            ble_connected = true;
            initial_pairing = false;
            scan_requested = false;
            if (inquiry_active)
            {
                esp_err_t cancel_err = esp_bt_gap_cancel_discovery();
                if (cancel_err != ESP_OK)
                    RG_LOGW("Bluetooth inquiry cancellation: %s", esp_err_to_name(cancel_err));
            }
            RG_LOGI("BLE gamepad ready");
            return;
        }
    }
    fail_setup("no supported gamepad input report", BLE_HS_ENOENT);
}

static int report_map_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    if (!setup_active(conn_handle))
        return 0;
    if (error->status == 0)
    {
        size_t length = OS_MBUF_PKTLEN(attr->om);
        if (length > MAX_REPORT_MAP_SIZE - report_map_length)
        {
            fail_setup("report map too large", BLE_HS_EMSGSIZE);
            return BLE_HS_EMSGSIZE;
        }
        uint8_t *resized = realloc(report_map, report_map_length + length);
        if (!resized)
        {
            fail_setup("report map allocation", BLE_HS_ENOMEM);
            return BLE_HS_ENOMEM;
        }
        report_map = resized;
        int rc = os_mbuf_copydata(attr->om, 0, length, report_map + report_map_length);
        if (rc != 0)
        {
            fail_setup("report map copy", rc);
            return rc;
        }
        report_map_length += length;
    }
    else if (error->status == BLE_HS_EDONE)
    {
        parse_report_map(report_map, report_map_length);
        free(report_map);
        report_map = NULL;
        report_map_length = 0;
        if (!hid_layout.valid)
            fail_setup("unsupported report map", BLE_HS_ENOTSUP);
        else
        {
            current_characteristic = 0;
            next_report();
        }
    }
    else
        fail_setup("report map read", error->status);
    return 0;
}

static int characteristics_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                      const struct ble_gatt_chr *chr, void *arg)
{
    if (!setup_active(conn_handle))
        return 0;
    if (error->status == 0)
    {
        if (characteristic_count == MAX_CHARACTERISTICS)
        {
            fail_setup("too many HID characteristics", BLE_HS_EMSGSIZE);
            return BLE_HS_EMSGSIZE;
        }
        characteristics[characteristic_count++] = *chr;
        if (ble_uuid_u16(&chr->uuid.u) == UUID_REPORT_MAP)
            report_map_handle = chr->val_handle;
    }
    else if (error->status == BLE_HS_EDONE)
    {
        if (!report_map_handle)
            fail_setup("missing report map", BLE_HS_ENOENT);
        else
        {
            configuration_characteristic = 0;
            configure_hid();
        }
    }
    else
        fail_setup("characteristic discovery", error->status);
    return 0;
}

static int hid_configured(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    if (!setup_active(conn_handle))
        return 0;
    if (error->status != 0)
        fail_setup("HID mode configuration", error->status);
    else
        configure_hid();
    return 0;
}

static void configure_hid(void)
{
    while (configuration_characteristic < characteristic_count)
    {
        const struct ble_gatt_chr *chr = &characteristics[configuration_characteristic++];
        uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        if (uuid != UUID_PROTOCOL_MODE && uuid != UUID_HID_CONTROL_POINT)
            continue;

        // Report protocol and Exit Suspend both use value 1.
        const uint8_t value = 1;
        begin_stage(uuid == UUID_PROTOCOL_MODE ? "report protocol selection" : "exit HID suspend");
        if (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP)
        {
            int rc = ble_gattc_write_no_rsp_flat(connection, chr->val_handle, &value, sizeof(value));
            if (rc != 0)
            {
                fail_setup(setup_stage, rc);
                return;
            }
        }
        else if (chr->properties & BLE_GATT_CHR_PROP_WRITE)
        {
            check_request(ble_gattc_write_flat(connection, chr->val_handle, &value,
                                              sizeof(value), hid_configured, NULL));
            return;
        }
        else
        {
            fail_setup("HID mode characteristic is not writable", BLE_HS_ENOTSUP);
            return;
        }
    }

    begin_stage("report map read");
    check_request(ble_gattc_read_long(connection, report_map_handle, 0, report_map_read, NULL));
}

static int service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    if (!setup_active(conn_handle))
        return 0;
    if (error->status == 0 && !hid_service.start_handle)
        hid_service = *service;
    else if (error->status == BLE_HS_EDONE)
    {
        if (!hid_service.start_handle)
            fail_setup("missing HID service", BLE_HS_ENOENT);
        else
        {
            begin_stage("characteristic discovery");
            check_request(ble_gattc_disc_all_chrs(connection, hid_service.start_handle,
                                                hid_service.end_handle, characteristics_discovered, NULL));
        }
    }
    else if (error->status != 0)
        fail_setup("service discovery", error->status);
    return 0;
}

static void discover_hid(void)
{
    const ble_uuid16_t uuid = BLE_UUID16_INIT(UUID_HID_SERVICE);
    begin_stage("HID service discovery");
    check_request(ble_gattc_disc_svc_by_uuid(connection, &uuid.u, service_discovered, NULL));
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC && !connecting && !ble_connected &&
        is_gamepad_advertisement(&event->disc))
    {
        connecting = true;
        int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY)
        {
            RG_LOGE("BLE scan cancellation failed: %d", rc);
            connecting = false;
            return 0;
        }
        RG_LOGI("Connecting to BLE HID device");
        rc = ble_gap_connect(own_address_type, &event->disc.addr, 10000, NULL, gap_event, NULL);
        if (rc != 0)
        {
            RG_LOGE("BLE connection request failed: %d", rc);
            connecting = false;
            start_scan();
        }
    }
    else if (event->type == BLE_GAP_EVENT_CONNECT)
    {
        if (event->connect.status != 0)
        {
            RG_LOGW("BLE connection failed: %d", event->connect.status);
            connecting = false;
            start_scan();
            return 0;
        }
        connection = event->connect.conn_handle;
        RG_LOGI("BLE link established");
        begin_stage("pairing/encryption");
        int rc = ble_gap_security_initiate(connection);
        if (rc == BLE_HS_EALREADY)
            discover_hid();
        else
            check_request(rc);
    }
    else if (event->type == BLE_GAP_EVENT_ENC_CHANGE && setup_active(event->enc_change.conn_handle))
    {
        if (event->enc_change.status != 0)
            fail_setup("pairing/encryption", event->enc_change.status);
        else
            discover_hid();
    }
    else if (event->type == BLE_GAP_EVENT_DISCONNECT)
    {
        RG_LOGW("BLE disconnected: %d", event->disconnect.reason);
        ble_npl_callout_stop(&setup_timeout);
        connection = BLE_HS_CONN_HANDLE_NONE;
        ble_connected = false;
        connecting = false;
        closing = false;
        ble_keys = 0;
        characteristic_count = 0;
        report_map_handle = 0;
        memset(&hid_layout, 0, sizeof(hid_layout));
        memset(&hid_service, 0, sizeof(hid_service));
        memset(subscribed, 0, sizeof(subscribed));
        free(report_map);
        report_map = NULL;
        report_map_length = 0;
        input_reports = 0;
        start_scan();
    }
    else if (event->type == BLE_GAP_EVENT_CONN_UPDATE)
    {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0)
            RG_LOGI("BLE parameters: status=%d interval=%u latency=%u timeout=%u",
                    event->conn_update.status, desc.conn_itvl, desc.conn_latency,
                    desc.supervision_timeout);
    }
    else if (event->type == BLE_GAP_EVENT_NOTIFY_RX && event->notify_rx.conn_handle == connection && !closing)
    {
        if (input_reports++ < 8)
            RG_LOGI("BLE notification: handle=%u bytes=%u", event->notify_rx.attr_handle,
                    (unsigned)OS_MBUF_PKTLEN(event->notify_rx.om));
        for (size_t i = 0; i < characteristic_count; ++i)
        {
            if (subscribed[i] && characteristics[i].val_handle == event->notify_rx.attr_handle)
            {
                uint8_t data[64];
                size_t length = OS_MBUF_PKTLEN(event->notify_rx.om);
                if (length > sizeof(data) || os_mbuf_copydata(event->notify_rx.om, 0, length, data) != 0)
                    RG_LOGW("BLE input report invalid or too large: %u bytes", (unsigned)length);
                else
                    handle_gamepad_report(report_ids[i], data, length);
                break;
            }
        }
    }
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE && !connecting && !ble_connected)
    {
        start_scan();
    }
    return 0;
}

static void start_scan(void)
{
    if (connecting || ble_connected)
        return;

    struct ble_gap_disc_params params = {
        .filter_duplicates = 1,
        .passive = 0,
    };
    int rc = ble_gap_disc(own_address_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        RG_LOGE("BLE scan failed: %d", rc);
    else
        RG_LOGI("BLE HID scan started");
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    int addr_rc = ble_hs_id_infer_auto(0, &own_address_type);
    if (rc != 0 || addr_rc != 0)
    {
        RG_LOGE("NimBLE address setup failed: %d/%d", rc, addr_rc);
        return;
    }
    start_scan();
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

void rg_ble_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        RG_LOGE("BLE requires initialized NVS for pairing storage: %s", esp_err_to_name(err));
        return;
    }

    err = nimble_port_init();
    if (err != ESP_OK)
    {
        RG_LOGE("NimBLE initialization failed: %s", esp_err_to_name(err));
        return;
    }

    ble_npl_callout_init(&setup_timeout, nimble_port_get_dflt_eventq(), setup_timed_out, NULL);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

#else

typedef enum { BT_SCAN, BT_CONNECT, BT_ENABLE, BT_DISABLE } bt_command_type_t;
typedef struct
{
    bt_command_type_t type;
    rg_bt_device_t device;
} bt_command_t;
static QueueHandle_t classic_commands;
static volatile bool inquiry_active;
static volatile bool radio_enabled = true;
static volatile bool host_ready;
static volatile bool hid_started;
static volatile bool hid_start_failed;
static volatile bool accepting_reconnect;
static volatile bool saved_acl_connected;
static volatile bool teardown_pending;
static volatile TickType_t incoming_deadline;
static volatile bool shutting_down;
static volatile bool shutdown_complete;
static volatile bool scan_requested;
static volatile bool initial_pairing;
static volatile unsigned reconnect_attempts;
static volatile TickType_t reconnect_at;
static volatile bool operation_failed;
static portMUX_TYPE bt_lock = portMUX_INITIALIZER_UNLOCKED;
static rg_bt_device_t discovered[RG_BT_MAX_DEVICES];
static int discovered_count;
static rg_bt_device_t saved_device, active_device;
static bool saved_valid, active_valid;

#if RG_BT_RESTORE_METADATA
static esp_hh_cb_t sdk_hid_callback;
static volatile bool metadata_pending, metadata_complete;
static volatile esp_hidh_status_t metadata_status;
static volatile uint8_t metadata_handle;

esp_err_t __real_esp_bt_hid_host_register_callback(esp_hh_cb_t callback);

static void metadata_hid_callback(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param)
{
    if (event == ESP_HIDH_SET_INFO_EVT && metadata_pending &&
        !memcmp(param->set_info.bd_addr, saved_device.address, 6))
    {
        metadata_status = param->set_info.status;
        metadata_handle = param->set_info.handle;
        RG_LOGI("Bonded HID metadata completion: status=%d handle=%u",
                param->set_info.status, param->set_info.handle);
        metadata_complete = true;
    }
    // Keep esp_hidh's device ownership and all other callbacks intact.
    if (sdk_hid_callback)
        sdk_hid_callback(event, param);
}

esp_err_t __wrap_esp_bt_hid_host_register_callback(esp_hh_cb_t callback)
{
    sdk_hid_callback = callback;
    return __real_esp_bt_hid_host_register_callback(callback ? metadata_hid_callback : NULL);
}

static bool parse_hid_metadata(char *text, const char *address, esp_hidh_hid_info_t *info)
{
    const char *fields[] = {
        "HidAttrMask", "HidSubClass", "HidAppId", "HidVendorId",
        "HidProductId", "HidVersion", "HidCountryCode",
    };
    int values[7] = {0};
    unsigned found = 0;
    bool selected = false, descriptor = false;
    char *context;
    for (char *line = strtok_r(text, "\n", &context); line; line = strtok_r(NULL, "\n", &context))
    {
        while (isspace((unsigned char)*line)) ++line;
        size_t length = strlen(line);
        while (length && isspace((unsigned char)line[length - 1])) line[--length] = 0;
        if (*line == '[')
        {
            selected = length == 19 && line[18] == ']' && !strncasecmp(line + 1, address, 17);
            continue;
        }
        if (!selected) continue;
        char *value = strchr(line, '=');
        if (!value) continue;
        *value++ = 0;
        length = strlen(line);
        while (length && isspace((unsigned char)line[length - 1])) line[--length] = 0;
        while (isspace((unsigned char)*value)) ++value;
        if (!strcmp(line, "HidDescriptor"))
        {
            length = strlen(value);
            if (!length || (length & 1) || length / 2 > sizeof(info->dsc_list) || descriptor)
                return false;
            for (size_t i = 0; i < length; i += 2)
            {
                if (!isxdigit((unsigned char)value[i]) || !isxdigit((unsigned char)value[i + 1]))
                    return false;
                char hex[3] = {value[i], value[i + 1], 0};
                info->dsc_list[i / 2] = strtoul(hex, NULL, 16);
            }
            info->dl_len = length / 2;
            descriptor = true;
        }
        for (size_t i = 0; i < RG_COUNT(fields); ++i)
        {
            if (strcmp(line, fields[i])) continue;
            char *end;
            errno = 0;
            long number = strtol(value, &end, 10);
            int limit = i == 1 || i == 2 || i == 6 ? 255 : 65535;
            if (errno || end == value || *end || number < 0 || number > limit || (found & (1u << i)))
                return false;
            values[i] = number;
            found |= 1u << i;
        }
    }
    if (found != 0x7f || !descriptor) return false;
    info->attr_mask = values[0];
    info->sub_class = values[1];
    info->app_id = values[2];
    info->vendor_id = values[3];
    info->product_id = values[4];
    info->version = values[5];
    info->ctry_code = values[6];
    return true;
}

static esp_err_t classic_restore_metadata(void)
{
    int count = esp_bt_gap_get_bond_device_num();
    RG_LOGI("Bonded controller count: %d", count);
    if (!saved_valid || count <= 0) return ESP_OK;
    esp_bd_addr_t *bonds = calloc(count, sizeof(esp_bd_addr_t));
    if (!bonds) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&count, bonds);
    if (err != ESP_OK)
        RG_LOGE("Bonded controller list read failed: %s", esp_err_to_name(err));
    bool bonded = false;
    for (int i = 0; err == ESP_OK && i < count; ++i)
        if (!memcmp(bonds[i], saved_device.address, 6)) bonded = true;
    free(bonds);
    if (err != ESP_OK) return err;
    if (!bonded)
    {
        RG_LOGW("Saved controller is not bonded; HID metadata restore skipped");
        return ESP_OK;
    }
    // IDF 5.2 stores textual configuration across 1536-byte NVS chunks.
    // Read only; never log the configuration, which also contains pairing keys.
    nvs_handle_t handle;
    err = nvs_open("bt_config.conf", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    char *text = NULL;
    size_t total = 0;
    for (unsigned chunk = 0; chunk < 43; ++chunk)
    {
        char key[24];
        snprintf(key, sizeof(key), "bt_cfg_key%u", chunk);
        size_t size = 0;
        err = nvs_get_blob(handle, key, NULL, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND && total) { err = ESP_OK; break; }
        if (err != ESP_OK) break;
        if (!size && total) break;
        if (!size || size > 1536 || total + size > 65536) { err = ESP_ERR_INVALID_SIZE; break; }
        char *resized = realloc(text, total + size + 1);
        if (!resized) { err = ESP_ERR_NO_MEM; break; }
        text = resized;
        err = nvs_get_blob(handle, key, text + total, &size);
        if (err != ESP_OK) break;
        total += size;
        text[total] = 0;
        if (size < 1536) break;
    }
    nvs_close(handle);
    esp_hidh_hid_info_t *info = calloc(1, sizeof(*info));
    if (err == ESP_OK && !info) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK)
    {
        char address[18];
        snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
                 saved_device.address[0], saved_device.address[1], saved_device.address[2],
                 saved_device.address[3], saved_device.address[4], saved_device.address[5]);
        if (!text || !parse_hid_metadata(text, address, info))
            err = ESP_ERR_INVALID_RESPONSE;
    }
    free(text);
    if (err == ESP_ERR_INVALID_RESPONSE)
    {
        RG_LOGW("Saved HID metadata missing or invalid; Scan/connect is required to refresh it");
        free(info);
        return ESP_OK;
    }
    if (err == ESP_OK)
    {
        RG_LOGI("Loading bonded HID metadata: attr=0x%x subclass=0x%x app=%u descriptor=%d",
                info->attr_mask, info->sub_class, info->app_id, info->dl_len);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            metadata_complete = false;
            metadata_pending = true;
            err = esp_bt_hid_host_set_info(saved_device.address, info);
            if (err != ESP_OK)
            {
                RG_LOGE("HID metadata submission failed: %s", esp_err_to_name(err));
                break;
            }
            for (int i = 0; !metadata_complete && i < 50; ++i)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (!metadata_complete) { err = ESP_ERR_TIMEOUT; break; }
            if (metadata_status == ESP_HIDH_OK) break;
            // IDF 5.2 bta_hh_maint_dev_act assigns the new handle but leaves
            // status=ERR after a successful first add. An idempotent second
            // SET_INFO takes its already-added branch and reports real OK.
            if (attempt == 0 && metadata_status == ESP_HIDH_ERR && metadata_handle != 255)
            {
                RG_LOGW("SDK reported ERR with an allocated HID handle; confirming metadata with a second SET_INFO");
                continue;
            }
            err = ESP_FAIL;
            break;
        }
        metadata_pending = false;
        if (err == ESP_OK)
            RG_LOGI("Restored bonded HID metadata: descriptor=%d bytes", info->dl_len);
    }
    free(info);
    return err;
}
#endif

bool rg_bt_enabled(void) { return radio_enabled; }

const char *rg_bt_status(void)
{
    if (!host_ready) return "Unavailable";
    if (!radio_enabled) return "Off";
    if (ble_connected) return "Connected";
    if (teardown_pending) return "Waiting for link teardown";
    if (connecting) return "Connecting";
    if (scan_requested || inquiry_active) return "Scanning";
    if (reconnect_at) return "Retry pending";
    if (operation_failed) return "Failed; retry manually";
    return "Idle";
}

bool rg_bt_device_get(int index, rg_bt_device_t *device)
{
    portENTER_CRITICAL(&bt_lock);
    bool valid = index >= 0 && index < discovered_count;
    if (valid) *device = discovered[index];
    portEXIT_CRITICAL(&bt_lock);
    return valid;
}

bool rg_bt_saved_get(rg_bt_device_t *device)
{
    portENTER_CRITICAL(&bt_lock);
    bool valid = saved_valid;
    if (valid) *device = saved_device;
    portEXIT_CRITICAL(&bt_lock);
    return valid;
}

static bool bt_submit(bt_command_t command)
{
    if (shutting_down)
    {
        RG_LOGW("Bluetooth is shutting down");
        return false;
    }
    if ((command.type == BT_CONNECT || command.type == BT_SCAN) && (!radio_enabled || connecting))
    {
        RG_LOGW("Bluetooth action unavailable while off or connecting");
        return false;
    }
    if (!host_ready || xQueueSend(classic_commands, &command, 0) != pdTRUE)
    {
        RG_LOGE("Bluetooth command unavailable or queue full");
        return false;
    }
    return true;
}

bool rg_bt_set_enabled(bool enabled)
{
    if (!bt_submit((bt_command_t){.type = enabled ? BT_ENABLE : BT_DISABLE}))
        return false;
    radio_enabled = enabled;
    if (!enabled) ble_keys = 0;
    return true;
}

bool rg_bt_scan(void)
{
    if (!radio_enabled || connecting)
    {
        RG_LOGW("Bluetooth scan unavailable while off or connecting");
        return false;
    }
    return bt_submit((bt_command_t){.type = BT_SCAN});
}

bool rg_bt_connect(int index)
{
    bt_command_t command = {.type = BT_CONNECT};
    if (!rg_bt_device_get(index, &command.device))
    {
        RG_LOGW("Bluetooth device no longer available");
        return false;
    }
    return bt_submit(command);
}

bool rg_bt_reconnect(void)
{
    bt_command_t command = {.type = BT_CONNECT};
    if (!rg_bt_saved_get(&command.device))
    {
        RG_LOGW("No saved Bluetooth controller");
        return false;
    }
    return bt_submit(command);
}

bool rg_bt_save(void)
{
    rg_bt_device_t device;
    portENTER_CRITICAL(&bt_lock);
    bool valid = active_valid && ble_connected;
    device = active_device;
    portEXIT_CRITICAL(&bt_lock);
    if (!valid)
    {
        RG_LOGE("No connected Bluetooth controller to save");
        return false;
    }
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
             device.address[0], device.address[1], device.address[2],
             device.address[3], device.address[4], device.address[5]);
    rg_settings_set_string(NS_GLOBAL, "BluetoothAddress", address);
    rg_settings_set_string(NS_GLOBAL, "BluetoothName", device.name);
    if (!rg_settings_commit())
    {
        RG_LOGE("Bluetooth controller could not be saved to SD");
        return false;
    }
    portENTER_CRITICAL(&bt_lock);
    saved_device = device;
    saved_valid = true;
    portEXIT_CRITICAL(&bt_lock);
    return true;
}

static void classic_retry(void)
{
    portENTER_CRITICAL(&bt_lock);
    if (!shutting_down && radio_enabled && saved_valid && reconnect_attempts < 5)
        reconnect_at = xTaskGetTickCount() + pdMS_TO_TICKS(RG_MIN(5 << reconnect_attempts, 60) * 1000);
    else
        reconnect_at = 0;
    portEXIT_CRITICAL(&bt_lock);
}

static bool classic_listen_reconnect(void)
{
    esp_err_t err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (err != ESP_OK)
    {
        RG_LOGE("Classic reconnect listener failed: %s", esp_err_to_name(err));
        operation_failed = true;
        return false;
    }
    accepting_reconnect = true;
    RG_LOGI("Classic saved-controller reconnect listener enabled");
    return true;
}

static bool classic_accept_reconnect(void)
{
    if (!classic_listen_reconnect())
        return false;
    reconnect_at = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    RG_LOGI("Classic saved-controller incoming reconnect window: 10 seconds");
    return true;
}

static void classic_hid_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_data_t *event = event_data;
    switch (id)
    {
    case ESP_HIDH_START_EVENT:
        hid_start_failed = event->start.status != ESP_OK;
        hid_started = !hid_start_failed;
        RG_LOGI("Classic HID host start status: %d", event->start.status);
        break;
    case ESP_HIDH_OPEN_EVENT:
        if (event->open.status != ESP_OK || !event->open.dev)
        {
            connecting = false;
            incoming_deadline = 0;
            operation_failed = true;
            RG_LOGE("Classic HID open failed: %d", event->open.status);
            classic_retry();
            break;
        }
        if (saved_valid && accepting_reconnect &&
            memcmp(saved_device.address, esp_hidh_dev_bda_get(event->open.dev), 6))
        {
            RG_LOGW("Rejecting incoming HID connection from an unsaved controller");
            esp_err_t close_err = esp_hidh_dev_close(event->open.dev);
            if (close_err != ESP_OK)
                RG_LOGE("Unsaved incoming HID close failed: %s", esp_err_to_name(close_err));
            break;
        }
        memset(&hid_layout, 0, sizeof(hid_layout));
        size_t count = 0;
        esp_hid_raw_report_map_t *maps = NULL;
        esp_err_t err = esp_hidh_dev_report_maps_get(event->open.dev, &count, &maps);
        if (err == ESP_OK)
        {
            for (size_t i = 0; i < count && !hid_layout.valid; ++i)
                parse_report_map(maps[i].data, maps[i].len);
        }
        if (!hid_layout.valid)
        {
            RG_LOGE("Classic HID has no supported gamepad report map: %s", esp_err_to_name(err));
            err = esp_hidh_dev_close(event->open.dev);
            if (err != ESP_OK)
                RG_LOGE("Classic HID close failed: %s", esp_err_to_name(err));
            break;
        }
        if (!radio_enabled)
        {
            esp_err_t close_err = esp_hidh_dev_close(event->open.dev);
            if (close_err != ESP_OK)
                RG_LOGE("Bluetooth disabled close failed: %s", esp_err_to_name(close_err));
            break;
        }

        portENTER_CRITICAL(&bt_lock);
        memcpy(active_device.address, esp_hidh_dev_bda_get(event->open.dev), 6);
        const char *name = esp_hidh_dev_name_get(event->open.dev);
        if (name && *name)
            snprintf(active_device.name, sizeof(active_device.name), "%s", name);
        else if (!active_device.name[0])
            snprintf(active_device.name, sizeof(active_device.name), "%s", "Controller");
        active_valid = true;
        ble_connected = true;
        connecting = false;
        portEXIT_CRITICAL(&bt_lock);
        operation_failed = false;
        reconnect_attempts = 0;
        reconnect_at = 0;
        incoming_deadline = 0;
        accepting_reconnect = false;
        err = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        if (err != ESP_OK)
            RG_LOGE("Classic connected scan-mode update failed: %s", esp_err_to_name(err));
        RG_LOGI("Classic gamepad ready: %s", esp_hidh_dev_name_get(event->open.dev));
        break;
    case ESP_HIDH_INPUT_EVENT:
        #if RG_RUNTIME_DIAGNOSTICS
        {
            static int64_t next_report_log;
            int64_t now = rg_system_timer();
            if (now >= next_report_log)
            {
                next_report_log = now + 1000000;
                log_hid_bytes("input", event->input.report_id, event->input.data,
                              RG_MIN(event->input.length, 16));
            }
        }
        #endif
        #if RG_ENABLE_DEV_MODE
        RG_LOGD("Classic input: report=%u bytes=%u", event->input.report_id,
                (unsigned)event->input.length);
        #endif
        if (ble_connected && radio_enabled)
            handle_gamepad_report(event->input.report_id, event->input.data, event->input.length);
        break;
    case ESP_HIDH_CLOSE_EVENT:
        RG_LOGW("Classic HID disconnected: %d", event->close.reason);
        ble_connected = false;
        connecting = false;
        incoming_deadline = 0;
        ble_keys = 0;
        memset(&hid_layout, 0, sizeof(hid_layout));
        classic_retry();
        break;
    default:
        break;
    }
}

static void classic_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    esp_err_t err = ESP_OK;
    if (event == ESP_BT_GAP_DISC_RES_EVT && radio_enabled && scan_requested)
    {
        const uint8_t *name = NULL;
        size_t length = 0;
        for (int i = 0; i < param->disc_res.num_prop; ++i)
        {
            esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
            if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME)
            {
                name = prop->val;
                length = prop->len;
            }
            else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR && !name)
            {
                uint8_t name_length = 0;
                name = esp_bt_gap_resolve_eir_data(prop->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                                                  &name_length);
                if (!name)
                    name = esp_bt_gap_resolve_eir_data(prop->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
                                                      &name_length);
                length = name_length;
            }
        }
        RG_LOGI("Classic discovery %02x:%02x:%02x:%02x:%02x:%02x name='%.*s'",
                param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
                param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5],
                (int)length, name ? (const char *)name : "");
        if (name && (name_contains(name, length, "controller") ||
                     name_contains(name, length, "q36") ||
                     name_contains(name, length, "gamepad") ||
                     name_contains(name, length, "joystick")))
        {
            portENTER_CRITICAL(&bt_lock);
            int index = 0;
            while (index < discovered_count && memcmp(discovered[index].address, param->disc_res.bda, 6))
                ++index;
            if (index < RG_BT_MAX_DEVICES)
            {
                memcpy(discovered[index].address, param->disc_res.bda, 6);
                size_t size = RG_MIN(length, sizeof(discovered[index].name) - 1);
                memcpy(discovered[index].name, name, size);
                discovered[index].name[size] = 0;
                if (index == discovered_count) ++discovered_count;
            }
            portEXIT_CRITICAL(&bt_lock);
            if (initial_pairing && !ble_connected && !connecting)
            {
                bt_command_t command = {.type = BT_CONNECT};
                if (rg_bt_device_get(index, &command.device) && bt_submit(command))
                    initial_pairing = false;
            }
        }
    }
    else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT)
    {
        inquiry_active = param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
        if (!inquiry_active)
        {
            scan_requested = false;
            initial_pairing = false;
        }
    }
    else if (event == ESP_BT_GAP_AUTH_CMPL_EVT)
        RG_LOGI("Classic authentication status=%d", param->auth_cmpl.stat);
    else if (event == ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT)
    {
        RG_LOGI("Classic ACL connection status=%d", param->acl_conn_cmpl_stat.stat);
        if (saved_valid && !memcmp(saved_device.address, param->acl_conn_cmpl_stat.bda, 6))
            saved_acl_connected = param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS;
        if (accepting_reconnect && radio_enabled && saved_valid && !ble_connected &&
            param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS &&
            !memcmp(saved_device.address, param->acl_conn_cmpl_stat.bda, 6))
        {
            portENTER_CRITICAL(&bt_lock);
            active_device = saved_device;
            active_valid = true;
            portEXIT_CRITICAL(&bt_lock);
            // An ACL link alone is not a HID connection. Give the controller a
            // brief chance to open HID, then let the host open it over this ACL.
            incoming_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        }
    }
    else if (event == ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT)
    {
        RG_LOGI("Classic ACL disconnected: reason=%d", param->acl_disconn_cmpl_stat.reason);
        if (saved_valid && !memcmp(saved_device.address, param->acl_disconn_cmpl_stat.bda, 6))
        {
            saved_acl_connected = false;
            teardown_pending = false;
        }
        if (active_valid && !ble_connected &&
            !memcmp(active_device.address, param->acl_disconn_cmpl_stat.bda, 6))
        {
            connecting = false;
            incoming_deadline = 0;
            ble_keys = 0;
            operation_failed = true;
            classic_retry();
        }
    }
    else if (event == ESP_BT_GAP_CFM_REQ_EVT)
        err = esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    else if (event == ESP_BT_GAP_PIN_REQ_EVT)
    {
        // No-input controllers use the standard legacy PIN.
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        if (param->pin_req.min_16_digit)
        {
            RG_LOGE("Classic controller requires unsupported 16-digit PIN entry");
            err = esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, pin);
        }
        else
            err = esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
    }
    if (err != ESP_OK)
        RG_LOGE("Classic GAP response failed: %s", esp_err_to_name(err));
}

static void classic_scan_task(void *arg)
{
    for (;;)
    {
        if (shutting_down)
        {
            radio_enabled = false;
            ble_keys = 0;
            reconnect_at = 0;
            scan_requested = false;
            initial_pairing = false;
            if (inquiry_active)
            {
                esp_err_t err = esp_bt_gap_cancel_discovery();
                if (err != ESP_OK)
                    RG_LOGE("Bluetooth shutdown inquiry cancellation failed: %s", esp_err_to_name(err));
            }
            if (active_valid && (ble_connected || connecting))
            {
                esp_err_t err = esp_bt_hid_host_disconnect(active_device.address);
                if (err != ESP_OK)
                    RG_LOGE("Bluetooth shutdown disconnect failed: %s", esp_err_to_name(err));
            }
            for (int i = 0; (ble_connected || connecting || inquiry_active) && i < 50; ++i)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (ble_connected || connecting || inquiry_active)
                RG_LOGW("Bluetooth shutdown timed out waiting for link closure");
            else
                RG_LOGI("Bluetooth link closed before app restart");
            // Allow the controller to observe the ACL teardown before reboot.
            vTaskDelay(pdMS_TO_TICKS(500));
            shutdown_complete = true;
            vTaskDelete(NULL);
            return;
        }
        bt_command_t command;
        bool automatic = false;
        if (xQueueReceive(classic_commands, &command, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            // Outbound retries are bounded, but the saved controller must still
            // be able to initiate recovery during backoff or after exhaustion.
            if (radio_enabled && saved_valid && !ble_connected && !connecting &&
                !teardown_pending && !inquiry_active && !accepting_reconnect &&
                (!reconnect_at || (int32_t)(xTaskGetTickCount() - reconnect_at) < 0))
            {
                if (!classic_listen_reconnect())
                    vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (incoming_deadline && !ble_connected && !connecting &&
                (int32_t)(xTaskGetTickCount() - incoming_deadline) >= 0)
            {
                incoming_deadline = 0;
                reconnect_at = xTaskGetTickCount();
                RG_LOGI("Classic ACL ready without HID; starting host HID setup");
            }
            if (!radio_enabled || ble_connected || connecting || teardown_pending || inquiry_active || !reconnect_at ||
                (incoming_deadline && (int32_t)(xTaskGetTickCount() - incoming_deadline) < 0) ||
                (int32_t)(xTaskGetTickCount() - reconnect_at) < 0)
                continue;
            automatic = true;
            command.type = BT_CONNECT;
            if (!rg_bt_saved_get(&command.device)) continue;
            ++reconnect_attempts;
        }
        reconnect_at = 0;
        esp_err_t err;
        if (command.type == BT_DISABLE)
        {
            radio_enabled = false;
            accepting_reconnect = false;
            ble_keys = 0;
            err = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            if (err != ESP_OK) RG_LOGE("Bluetooth disable failed: %s", esp_err_to_name(err));
            if (inquiry_active)
            {
                err = esp_bt_gap_cancel_discovery();
                if (err != ESP_OK) RG_LOGE("Bluetooth cancel failed: %s", esp_err_to_name(err));
            }
            if (active_valid && (ble_connected || connecting))
            {
                err = esp_bt_hid_host_disconnect(active_device.address);
                if (err != ESP_OK) RG_LOGE("Bluetooth disconnect failed: %s", esp_err_to_name(err));
            }
            continue;
        }
        if (command.type == BT_ENABLE)
        {
            radio_enabled = true;
            reconnect_attempts = 0;
            if (saved_valid)
                classic_accept_reconnect();
            else
            {
                err = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                if (err != ESP_OK) RG_LOGE("Bluetooth enable failed: %s", esp_err_to_name(err));
            }
            continue;
        }
        if (!radio_enabled || connecting || teardown_pending)
        {
            RG_LOGW("Bluetooth must be enabled and not connecting for scan/connect");
            continue;
        }
        if (accepting_reconnect)
        {
            err = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            if (err != ESP_OK)
            {
                RG_LOGE("Classic reconnect listener stop failed: %s", esp_err_to_name(err));
                operation_failed = true;
                continue;
            }
            // Allow an incoming ACL already in flight to reach the GAP callback.
            vTaskDelay(pdMS_TO_TICKS(250));
            accepting_reconnect = false;
            if (connecting || ble_connected)
                continue;
        }
        if (command.type == BT_SCAN)
        {
            if (inquiry_active) continue;
            portENTER_CRITICAL(&bt_lock);
            discovered_count = 0;
            portEXIT_CRITICAL(&bt_lock);
            scan_requested = true;
            operation_failed = false;
            inquiry_active = true;
            err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, initial_pairing ? 24 : 8, 0);
            if (err != ESP_OK)
            {
                scan_requested = inquiry_active = false;
                operation_failed = true;
                RG_LOGE("Classic inquiry failed: %s", esp_err_to_name(err));
            }
            continue;
        }
        if (command.type == BT_CONNECT)
        {
            if (shutting_down) continue;
            if (!automatic) reconnect_attempts = 0;
            operation_failed = false;
            if (automatic && !saved_acl_connected && !ble_connected)
            {
                RG_LOGI("Refreshing Classic discovery before saved-controller retry");
                portENTER_CRITICAL(&bt_lock);
                discovered_count = 0;
                portEXIT_CRITICAL(&bt_lock);
                scan_requested = inquiry_active = true;
                err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0);
                if (err != ESP_OK)
                {
                    scan_requested = inquiry_active = false;
                    operation_failed = true;
                    RG_LOGE("Classic reconnect inquiry failed: %s", esp_err_to_name(err));
                    classic_retry();
                    continue;
                }
                for (int i = 0; inquiry_active && !shutting_down &&
                     !uxQueueMessagesWaiting(classic_commands) && i < 130; ++i)
                    vTaskDelay(pdMS_TO_TICKS(100));
                if (shutting_down || uxQueueMessagesWaiting(classic_commands))
                {
                    err = esp_bt_gap_cancel_discovery();
                    if (err != ESP_OK)
                        RG_LOGE("Classic reconnect inquiry cancellation failed: %s", esp_err_to_name(err));
                    continue;
                }
                rg_bt_device_t found;
                bool matched = false;
                for (int i = 0; rg_bt_device_get(i, &found); ++i)
                {
                    if (!memcmp(found.address, command.device.address, 6))
                        matched = true;
                    else if (!strcmp(found.name, command.device.name))
                        RG_LOGW("Controller name matches but address differs; select Scan/connect and save it");
                }
                RG_LOGI("Saved controller seen during inquiry: %d", matched);
            }
            scan_requested = false;
            if (inquiry_active)
            {
                err = esp_bt_gap_cancel_discovery();
                if (err != ESP_OK)
                    RG_LOGW("Classic inquiry cancellation: %s", esp_err_to_name(err));
            }
            for (int i = 0; inquiry_active && i < 30; ++i)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (inquiry_active)
            {
                RG_LOGE("Classic inquiry did not stop; delaying connection");
                connecting = false;
                operation_failed = true;
                continue;
            }
            if (ble_connected)
            {
                if (!memcmp(active_device.address, command.device.address, 6))
                    continue;
                err = esp_bt_hid_host_disconnect(active_device.address);
                if (err != ESP_OK)
                {
                    RG_LOGE("Bluetooth switch disconnect failed: %s", esp_err_to_name(err));
                    continue;
                }
                for (int i = 0; ble_connected && i < 50; ++i)
                    vTaskDelay(pdMS_TO_TICKS(100));
                if (ble_connected)
                {
                    RG_LOGE("Bluetooth switch timed out");
                    continue;
                }
                reconnect_at = 0;
            }
            portENTER_CRITICAL(&bt_lock);
            active_device = command.device;
            active_valid = true;
            portEXIT_CRITICAL(&bt_lock);
            connecting = true;
            RG_LOGI("Connecting Classic HID %02x:%02x:%02x:%02x:%02x:%02x name='%s'",
                    command.device.address[0], command.device.address[1], command.device.address[2],
                    command.device.address[3], command.device.address[4], command.device.address[5],
                    command.device.name);
            if (!esp_hidh_dev_open(command.device.address, ESP_HID_TRANSPORT_BT, 0))
            {
                operation_failed = true;
                RG_LOGE("Classic HID connection request failed");
                connecting = false;
                classic_retry();
            }
            else
            {
                // Classic SDP can take about a minute to report failure. Do not
                // manufacture a close and retry while its transaction is alive.
                for (int i = 0; connecting && i < 900 && !shutting_down &&
                     !uxQueueMessagesWaiting(classic_commands); ++i)
                    vTaskDelay(pdMS_TO_TICKS(100));
                if (connecting && !shutting_down)
                {
                    operation_failed = true;
                    RG_LOGE("Classic HID connection setup timed out");
                    teardown_pending = saved_acl_connected;
                    err = esp_bt_hid_host_disconnect(command.device.address);
                    if (err != ESP_OK)
                        RG_LOGE("Classic HID disconnect request failed: %s", esp_err_to_name(err));
                    for (int i = 0; (connecting || saved_acl_connected) &&
                         !shutting_down && i < 100; ++i)
                        vTaskDelay(pdMS_TO_TICKS(100));
                    if ((connecting || saved_acl_connected) && !shutting_down)
                    {
                        RG_LOGE("Classic HID teardown produced no close event; connection recovery stopped");
                        connecting = false;
                        // Do not open another device while the SDK may still own the old one.
                        reconnect_at = 0;
                        reconnect_attempts = 5;
                    }
                    if (!saved_acl_connected)
                        teardown_pending = false;
                }
            }
        }
    }
}

void rg_bt_shutdown(void)
{
    if (!host_ready) return;
    shutting_down = true;
    radio_enabled = false;
    ble_keys = 0;
    for (int i = 0; !shutdown_complete && i < 80; ++i)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!shutdown_complete)
        RG_LOGE("Bluetooth shutdown task did not complete before restart");
    host_ready = false;
}

void rg_ble_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        RG_LOGE("Classic HID NVS initialization failed: %s", esp_err_to_name(err));
        return;
    }
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&config);
    if (err == ESP_OK)
        err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err == ESP_OK)
        err = esp_bluedroid_init();
    if (err == ESP_OK)
        err = esp_bluedroid_enable();
    if (err != ESP_OK)
    {
        RG_LOGE("Classic Bluetooth initialization failed: %s", esp_err_to_name(err));
        return;
    }
    char *address = rg_settings_get_string(NS_GLOBAL, "BluetoothAddress", NULL);
    if (address)
    {
        unsigned bda[6];
        int used = 0;
        saved_valid = sscanf(address, "%2x:%2x:%2x:%2x:%2x:%2x%n",
                             &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5], &used) == 6 &&
                      used == 17 && address[used] == 0;
        if (saved_valid)
        {
            for (int i = 0; i < 6; ++i) saved_device.address[i] = bda[i];
            char *name = rg_settings_get_string(NS_GLOBAL, "BluetoothName", "Controller");
            snprintf(saved_device.name, sizeof(saved_device.name), "%s", name ? name : "Controller");
            free(name);
        }
        else RG_LOGE("Invalid Bluetooth address in SD config");
        free(address);
    }
    classic_commands = xQueueCreate(4, sizeof(bt_command_t));
    if (!classic_commands)
    {
        RG_LOGE("Classic HID connection queue allocation failed");
        return;
    }
    err = esp_bt_gap_register_callback(classic_gap_event);
    esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
    if (err == ESP_OK)
        err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability));
    esp_hidh_config_t hid_config = {
        .callback = classic_hid_event,
        .event_stack_size = 4096,
    };
    if (err == ESP_OK)
        err = esp_hidh_init(&hid_config);
    if (err == ESP_OK)
    {
        for (int i = 0; !hid_started && !hid_start_failed && i < 80; ++i)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (!hid_started)
        {
            RG_LOGE("Classic HID host did not become ready");
            return;
        }
    }
    if (err == ESP_OK)
    {
        #if RG_BT_RESTORE_METADATA
        err = classic_restore_metadata();
        if (err != ESP_OK)
            RG_LOGE("Bonded HID metadata restore failed: %s", esp_err_to_name(err));
        #endif
    }
    if (err == ESP_OK)
        // Only initiate connections; simultaneous incoming/outgoing HID opens can collide.
        err = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (err != ESP_OK)
    {
        RG_LOGE("Classic HID host initialization failed: %s", esp_err_to_name(err));
        return;
    }
    classic_retry();
    if (saved_valid && !classic_accept_reconnect())
        classic_retry();
    host_ready = true;
    if (!saved_valid)
    {
        // One bounded first-use inquiry preserves pairing without local navigation keys.
        initial_pairing = true;
        rg_bt_scan();
    }
    if (xTaskCreate(classic_scan_task, "bt_scan", 4096, NULL, 5, NULL) != pdPASS)
    {
        host_ready = false;
        RG_LOGE("Classic HID scan task creation failed");
    }
}
#endif

uint32_t rg_ble_gamepad_state(void)
{
    return ble_keys;
}

#if RG_RUNTIME_DIAGNOSTICS
uint32_t rg_ble_gamepad_buttons(void)
{
    return ble_connected ? diagnostic_buttons : 0;
}
#endif

bool rg_ble_gamepad_connected(void)
{
    return ble_connected;
}

#endif
