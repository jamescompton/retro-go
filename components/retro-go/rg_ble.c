// BLE HID gamepad/keyboard host. NimBLE is used for scanning, and ESP-IDF's esp_hidh component
// handles connecting, pairing, discovery and report notifications. Reports are exposed as RG_KEY_* bits.
#include "rg_system.h"
#include "rg_ble.h"

#ifdef RG_ENABLE_BLE_GAMEPAD

#include <string.h>
#include <nvs_flash.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <esp_hidh.h>

#define TAG "ble"

#define UUID_HID_SERVICE 0x1812
#define APPEARANCE_JOYSTICK 0x03C3
#define APPEARANCE_GAMEPAD 0x03C4

#ifndef RG_BLE_BUTTON_MAP
// HID button index -> key. Index 0 is "Button 1". Matches the common Xbox/8BitDo layout.
#define RG_BLE_BUTTON_MAP {RG_KEY_A, RG_KEY_B, RG_KEY_OPTION, RG_KEY_MENU, 0, 0, RG_KEY_SELECT, RG_KEY_START}
#endif

typedef struct
{
    bool valid;
    int report_id;
    int btn_off, btn_count;
    int hat_off, hat_size, hat_min;
    int x_off, x_size, x_min, x_max;
    int y_off, y_size, y_min, y_max;
} hid_layout_t;

typedef struct
{
    uint8_t bda[6];
    uint8_t addr_type;
} connect_req_t;

static const uint32_t button_map[] = RG_BLE_BUTTON_MAP;
static volatile uint32_t ble_keys = 0;
static volatile bool ble_connected = false;
static uint8_t own_addr_type;
static hid_layout_t layout;
static esp_hidh_dev_t *hid_dev;

static void start_scan(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static int get_bits(const uint8_t *data, size_t len, int offset, int size, bool is_signed)
{
    int32_t value = 0;
    for (int i = 0; i < size && i < 32; ++i)
    {
        size_t byte = (offset + i) >> 3;
        if (byte < len && (data[byte] >> ((offset + i) & 7)) & 1)
            value |= 1 << i;
    }
    if (is_signed && size < 32 && (value & (1 << (size - 1))))
        value |= ~((1 << size) - 1);
    return value;
}

static void parse_report_map(const uint8_t *d, size_t len)
{
    hid_layout_t l = {.hat_off = -1, .x_off = -1, .y_off = -1, .btn_off = -1};
    int usage_page = 0, report_size = 0, report_count = 0, log_min = 0, log_max = 0, bit_off = 0;
    int usages[16], nusages = 0, usage_min = -1, usage_max = -1;

    for (size_t i = 0; i < len;)
    {
        uint8_t b = d[i];
        if (b == 0xFE) // long item
        {
            i += (i + 1 < len ? d[i + 1] : 0) + 3;
            continue;
        }
        int size = b & 3;
        if (size == 3)
            size = 4;
        int type = (b >> 2) & 3, tag = b >> 4;
        int32_t value = 0;
        for (int k = 0; k < size && i + 1 + k < len; ++k)
            value |= d[i + 1 + k] << (8 * k);
        if (size == 1)
            value = (int8_t)value;
        else if (size == 2)
            value = (int16_t)value;
        uint32_t uvalue = (uint32_t)value & (size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF);
        i += 1 + size;

        if (type == 1) // Global
        {
            if (tag == 0) usage_page = uvalue;
            else if (tag == 1) log_min = value;
            else if (tag == 2) log_max = value;
            else if (tag == 7) report_size = uvalue;
            else if (tag == 9) report_count = uvalue;
            else if (tag == 8) // Report ID
            {
                if (l.btn_off >= 0)
                    break; // Already found the gamepad report
                memset(&l, 0, sizeof(l));
                l.hat_off = l.x_off = l.y_off = l.btn_off = -1;
                l.report_id = uvalue;
                bit_off = 0;
            }
        }
        else if (type == 2) // Local
        {
            if (tag == 0 && nusages < 16) usages[nusages++] = uvalue;
            else if (tag == 1) usage_min = uvalue;
            else if (tag == 2) usage_max = uvalue;
        }
        else if (type == 0 && tag == 8) // Input
        {
            if (!(uvalue & 1)) // not constant
            {
                if (usage_page == 9 && l.btn_off < 0)
                {
                    l.btn_off = bit_off;
                    l.btn_count = RG_MIN(report_count * report_size, 32);
                }
                else if (usage_page == 1 && (uvalue & 2))
                {
                    for (int k = 0; k < report_count; ++k)
                    {
                        int u = k < nusages ? usages[k] : (usage_min >= 0 ? usage_min + k : -1);
                        int off = bit_off + k * report_size;
                        if (u == 0x39 && l.hat_off < 0)
                            l.hat_off = off, l.hat_size = report_size, l.hat_min = log_min;
                        else if (u == 0x30 && l.x_off < 0)
                            l.x_off = off, l.x_size = report_size, l.x_min = log_min, l.x_max = log_max;
                        else if (u == 0x31 && l.y_off < 0)
                            l.y_off = off, l.y_size = report_size, l.y_min = log_min, l.y_max = log_max;
                    }
                }
            }
            bit_off += report_size * report_count;
        }
        if (type == 0) // main item clears locals
            nusages = 0, usage_min = usage_max = -1;
    }

    l.valid = l.btn_off >= 0;
    layout = l;
    RG_LOGI("HID map parsed: valid=%d buttons=%d@%d hat@%d x@%d y@%d", l.valid, l.btn_count, l.btn_off, l.hat_off, l.x_off, l.y_off);
}

static uint32_t axis_keys(const uint8_t *data, size_t len, int off, int size, int min, int max, uint32_t neg, uint32_t pos)
{
    if (off < 0)
        return 0;
    int v = get_bits(data, len, off, size, min < 0);
    int range = max - min;
    if (range <= 0)
        return 0;
    int pct = (v - min) * 100 / range;
    return pct < 25 ? neg : pct > 75 ? pos : 0;
}

static void handle_keyboard_report(const uint8_t *data, size_t len)
{
    // Boot keyboard layout: modifiers, reserved, then up to 6 pressed keycodes
    uint32_t keys = 0;
    for (size_t i = 2; i < len && i < 8; ++i)
    {
        switch (data[i])
        {
        case 0x4F: keys |= RG_KEY_RIGHT; break;
        case 0x50: keys |= RG_KEY_LEFT; break;
        case 0x51: keys |= RG_KEY_DOWN; break;
        case 0x52: keys |= RG_KEY_UP; break;
        case 0x28: case 0x2C: case 0x1D: case 0x0E: keys |= RG_KEY_A; break; // Enter, Space, Z, K
        case 0x1B: case 0x0D: keys |= RG_KEY_B; break; // X, J
        case 0x1A: case 0x2B: keys |= RG_KEY_SELECT; break; // W, Tab
        case 0x14: case 0x15: keys |= RG_KEY_START; break; // Q, R
        case 0x29: keys |= RG_KEY_MENU; break; // Esc
        case 0x3A: keys |= RG_KEY_OPTION; break; // F1
        }
    }
    ble_keys = keys;
}

static void handle_report(int report_id, const uint8_t *data, size_t len)
{
    if (!layout.valid || (layout.report_id && layout.report_id != report_id))
        return;
    uint32_t keys = 0;
    uint32_t buttons = get_bits(data, len, layout.btn_off, layout.btn_count, false);
    for (size_t i = 0; i < RG_COUNT(button_map); ++i)
        if ((buttons >> i) & 1)
            keys |= button_map[i];

    if (layout.hat_off >= 0)
    {
        static const uint32_t hat_keys[8] = {
            RG_KEY_UP, RG_KEY_UP | RG_KEY_RIGHT, RG_KEY_RIGHT, RG_KEY_DOWN | RG_KEY_RIGHT,
            RG_KEY_DOWN, RG_KEY_DOWN | RG_KEY_LEFT, RG_KEY_LEFT, RG_KEY_UP | RG_KEY_LEFT,
        };
        int h = get_bits(data, len, layout.hat_off, layout.hat_size, false) - layout.hat_min;
        if (h >= 0 && h < 8)
            keys |= hat_keys[h];
    }
    keys |= axis_keys(data, len, layout.x_off, layout.x_size, layout.x_min, layout.x_max, RG_KEY_LEFT, RG_KEY_RIGHT);
    keys |= axis_keys(data, len, layout.y_off, layout.y_size, layout.y_min, layout.y_max, RG_KEY_UP, RG_KEY_DOWN);
    ble_keys = keys;
}

static bool is_gamepad(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, disc->data, disc->length_data) != 0)
    {
        // Some pads send adverts NimBLE refuses to parse, so walk the AD structures by hand
        for (int i = 0; i + 1 < disc->length_data;)
        {
            int l = disc->data[i];
            if (l == 0 || i + 1 + l > disc->length_data)
                break;
            const uint8_t *d = &disc->data[i + 2];
            int n = l - 1;
            uint8_t t = disc->data[i + 1];
            if ((t == 0x02 || t == 0x03) && n >= 2)
                for (int j = 0; j + 1 < n; j += 2)
                    if ((d[j] | d[j + 1] << 8) == UUID_HID_SERVICE)
                        return true;
            if ((t == 0x08 || t == 0x09) && n >= 3 && memcmp(d, "Q36", 3) == 0)
                return true;
            i += l + 1;
        }
        return false;
    }
    if (f.appearance_is_present && (f.appearance == APPEARANCE_GAMEPAD || f.appearance == APPEARANCE_JOYSTICK))
        return true;
    for (int i = 0; i < f.num_uuids16; ++i)
        if (ble_uuid_u16(&f.uuids16[i].u) == UUID_HID_SERVICE)
            return !f.appearance_is_present || f.appearance >> 6 == 0x0F; // HID-like appearance categories
    return false;
}

static void log_advertiser(const struct ble_gap_disc_desc *disc)
{
    static uint8_t seen[48][7];
    static int seen_count;
    uint8_t key[7];
    memcpy(key, disc->addr.val, 6);
    key[6] = disc->event_type;
    for (int i = 0; i < seen_count; ++i)
        if (memcmp(seen[i], key, 7) == 0)
            return;
    if (seen_count < 48)
        memcpy(seen[seen_count++], key, 7);

    struct ble_hs_adv_fields f;
    char name[32] = "";
    if (ble_hs_adv_parse_fields(&f, disc->data, disc->length_data) == 0)
    {
        if (f.name && f.name_len)
            snprintf(name, sizeof(name), "%.*s", RG_MIN(f.name_len, 31), (const char *)f.name);
        RG_LOGI("BLE adv %02x:%02x:%02x:%02x:%02x:%02x type=%d name='%s' appearance=%04x(%d) uuid16s=%d first=%04x",
                disc->addr.val[5], disc->addr.val[4], disc->addr.val[3], disc->addr.val[2], disc->addr.val[1],
                disc->addr.val[0], disc->event_type, name, f.appearance, f.appearance_is_present, f.num_uuids16,
                f.num_uuids16 ? ble_uuid_u16(&f.uuids16[0].u) : 0);
    }
    else
    {
        char hex[100] = "";
        for (int i = 0; i < disc->length_data && i < 32; ++i)
            snprintf(hex + i * 3, 4, "%02x ", disc->data[i]);
        RG_LOGI("BLE adv %02x:%02x:%02x:%02x:%02x:%02x type=%d UNPARSED: %s", disc->addr.val[5], disc->addr.val[4],
                disc->addr.val[3], disc->addr.val[2], disc->addr.val[1], disc->addr.val[0], disc->event_type, hex);
    }
}

static void connect_task(void *arg)
{
    connect_req_t req = *(connect_req_t *)arg;
    free(arg);
    RG_LOGI("BLE connecting to %02x:%02x:%02x:%02x:%02x:%02x", req.bda[5], req.bda[4], req.bda[3], req.bda[2],
            req.bda[1], req.bda[0]);
    hid_dev = esp_hidh_dev_open(req.bda, ESP_HID_TRANSPORT_BLE, req.addr_type);
    RG_LOGI("BLE esp_hidh_dev_open returned %p", hid_dev);
    if (!hid_dev)
    {
        RG_LOGW("BLE HID connect failed, rescanning");
        rg_task_delay(1000);
        start_scan();
    }
    vTaskDelete(NULL);
}

static void hidh_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)event_data;
    switch ((esp_hidh_event_t)id)
    {
    case ESP_HIDH_OPEN_EVENT:
    {
        hid_dev = p->open.dev;
        RG_LOGI("BLE HID open: %s", esp_hidh_dev_name_get(hid_dev) ?: "?");
        esp_hid_raw_report_map_t *maps = NULL;
        size_t nmaps = 0;
        if (esp_hidh_dev_report_maps_get(hid_dev, &nmaps, &maps) == ESP_OK && nmaps > 0)
            parse_report_map(maps[0].data, maps[0].len);
        RG_LOGI("BLE gamepad ready");
        ble_connected = true;
        break;
    }
    case ESP_HIDH_INPUT_EVENT:
    {
        static int logged = 0;
        if (logged++ < 20)
        {
            const uint8_t *d = p->input.data;
            int n = p->input.length;
            RG_LOGI("BLE report id=%d usage=%d len=%d: %02x %02x %02x %02x %02x %02x %02x %02x", p->input.report_id,
                    p->input.usage, n, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0, n > 2 ? d[2] : 0, n > 3 ? d[3] : 0,
                    n > 4 ? d[4] : 0, n > 5 ? d[5] : 0, n > 6 ? d[6] : 0, n > 7 ? d[7] : 0);
        }
        if (p->input.usage == ESP_HID_USAGE_KEYBOARD)
            handle_keyboard_report(p->input.data, p->input.length);
        else if (p->input.usage != ESP_HID_USAGE_MOUSE && p->input.usage != ESP_HID_USAGE_CCONTROL)
            handle_report(p->input.report_id, p->input.data, p->input.length);
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
        RG_LOGW("BLE HID closed, reason=%d (0x%x)", p->close.reason, p->close.reason);
        hid_dev = NULL;
        ble_connected = false;
        ble_keys = 0;
        layout.valid = false;
        start_scan();
        break;
    default:
        break;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
        log_advertiser(&event->disc);
        if (event->disc.event_type <= BLE_HCI_ADV_RPT_EVTYPE_DIR_IND && is_gamepad(&event->disc))
        {
            RG_LOGI("Found BLE HID device, connecting (addr type %d)...", event->disc.addr.type);
            ble_gap_disc_cancel();
            connect_req_t *req = malloc(sizeof(*req));
            if (req)
            {
                memcpy(req->bda, event->disc.addr.val, 6);
                req->addr_type = event->disc.addr.type;
                if (xTaskCreate(connect_task, "ble_connect", 6144, req, 5, NULL) == pdPASS)
                    break;
                free(req);
            }
            start_scan();
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!hid_dev)
            start_scan();
        break;
    default:
        break;
    }
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {.filter_duplicates = 0, .passive = 0, .itvl = 0, .window = 0};
    int rc = ble_gap_disc(own_addr_type, 10000, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        RG_LOGE("Scan failed: %d", rc);
    else
        RG_LOGI("BLE scan started (rc=%d, addr_type=%d)", rc, own_addr_type);
}

static void on_sync(void)
{
    ble_store_clear();
    int rc = ble_hs_util_ensure_addr(0);
    int rc2 = ble_hs_id_infer_auto(0, &own_addr_type);
    RG_LOGI("BLE host synced (addr rc=%d/%d)", rc, rc2);
    start_scan();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

void rg_ble_init(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (nimble_port_init() != ESP_OK)
    {
        RG_LOGE("NimBLE init failed");
        return;
    }
    esp_hidh_config_t hidh_cfg = {.callback = hidh_event, .event_stack_size = 4096, .callback_arg = NULL};
    if (esp_hidh_init(&hidh_cfg) != ESP_OK)
    {
        RG_LOGE("esp_hidh init failed");
        return;
    }
    // esp_hidh_init() replaces the host callbacks, so ours must be set afterwards
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

uint32_t rg_ble_gamepad_state(void)
{
    return ble_keys;
}

bool rg_ble_gamepad_connected(void)
{
    return ble_connected;
}

#endif
