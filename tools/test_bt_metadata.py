"""Compile and run production metadata parsing and preload using a host C compiler."""

import os
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / "components" / "retro-go" / "rg_ble.c").read_text()
start = source.index("static bool parse_hid_metadata(")
end = source.index("\nstatic esp_err_t classic_restore_metadata(", start)
parser = source[start:end]
start = source.index("        for (int attempt = 0; attempt < 2; ++attempt)")
end = source.index("        metadata_pending = false;", start)
preload = source[start:end]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#else
#include <strings.h>
#endif
#define RG_COUNT(x) (sizeof(x) / sizeof((x)[0]))
typedef struct {
    int attr_mask;
    uint8_t sub_class, app_id;
    int vendor_id, product_id, version;
    uint8_t ctry_code;
    int dl_len;
    uint8_t dsc_list[884];
} esp_hidh_hid_info_t;
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_TIMEOUT, ESP_HIDH_OK = 0, ESP_HIDH_ERR = 6 };
#define RG_LOGE(...) ((void)0)
#define RG_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
static bool metadata_complete, metadata_pending;
static int metadata_status;
static uint8_t metadata_handle;
static struct { uint8_t address[6]; } saved_device;
static int submissions, delays, reply_status[2], reply_handle[2], submit_error[2];
static bool no_reply[2];
static esp_err_t esp_bt_hid_host_set_info(uint8_t *address, esp_hidh_hid_info_t *info) {
    assert(address == saved_device.address && info != NULL);
    int i = submissions++;
    assert(i < 2);
    if (submit_error[i]) return submit_error[i];
    if (!no_reply[i]) {
        metadata_status = reply_status[i];
        metadata_handle = reply_handle[i];
        metadata_complete = true;
    }
    return ESP_OK;
}
static void vTaskDelay(int ticks) { assert(ticks == 100); ++delays; }
'''
preload = '''
static esp_err_t test_preload(void) {
    esp_hidh_hid_info_t storage = {0}, *info = &storage;
    esp_err_t err = ESP_OK;
''' + preload + '''
    metadata_pending = false;
    return err;
}
static void check_preload(int first, int handle, int second, int error_at,
                          int timeout_at, esp_err_t expected, int calls) {
    submissions = delays = 0;
    reply_status[0] = first; reply_status[1] = second;
    reply_handle[0] = handle; reply_handle[1] = handle;
    for (int i = 0; i < 2; ++i) {
        submit_error[i] = error_at == i ? 123 : ESP_OK;
        no_reply[i] = timeout_at == i;
    }
    assert(test_preload() == expected);
    assert(submissions == calls && !metadata_pending);
    assert(delays == (timeout_at >= 0 ? 50 : 0));
}
'''
tests = r'''
static void check(const char *data, bool expected) {
    char text[8192];
    esp_hidh_hid_info_t info = {0};
    assert(strlen(data) < sizeof(text));
    strcpy(text, data);
    assert(parse_hid_metadata(text, "83:39:03:11:48:10", &info) == expected);
    if (expected) {
        assert(info.attr_mask == 32855 && info.sub_class == 64 && info.app_id == 2);
        assert(info.vendor_id == 1118 && info.product_id == 765 && info.version == 65535);
        assert(info.ctry_code == 0 && info.dl_len == 2);
        assert(info.dsc_list[0] == 0x05 && info.dsc_list[1] == 0xAB);
    }
}
int main(void) {
    check_preload(ESP_HIDH_OK, 0, 0, -1, -1, ESP_OK, 1);
    check_preload(ESP_HIDH_ERR, 0, ESP_HIDH_OK, -1, -1, ESP_OK, 2);
    check_preload(ESP_HIDH_ERR, 255, ESP_HIDH_OK, -1, -1, ESP_FAIL, 1);
    check_preload(3, 0, ESP_HIDH_OK, -1, -1, ESP_FAIL, 1);
    check_preload(ESP_HIDH_ERR, 0, ESP_HIDH_ERR, -1, -1, ESP_FAIL, 2);
    check_preload(ESP_HIDH_ERR, 0, 3, -1, -1, ESP_FAIL, 2);
    check_preload(ESP_HIDH_ERR, 0, ESP_HIDH_OK, 0, -1, 123, 1);
    check_preload(ESP_HIDH_ERR, 0, ESP_HIDH_OK, 1, -1, 123, 2);
    check_preload(ESP_HIDH_ERR, 0, ESP_HIDH_OK, -1, 0, ESP_ERR_TIMEOUT, 1);
    check_preload(ESP_HIDH_ERR, 0, ESP_HIDH_OK, -1, 1, ESP_ERR_TIMEOUT, 2);
    puts("Metadata preload: success, bounded confirmation, invalid handle, other errors, submission failures and timeouts passed");
    const char *prefix =
        "[83:39:03:11:48:10]\nHidAttrMask = 32855\nHidSubClass = 64\n"
        "HidAppId = 2\nHidVendorId = 1118\nHidProductId = 765\n"
        "HidVersion = 65535\nHidCountryCode = 0\n";
    char text[8192];
    snprintf(text, sizeof(text), "%sHidDescriptor = 05aB\n", prefix);
    check(text, true);
    snprintf(text, sizeof(text), "[other]\nHidDescriptor = invalid\n%sHidDescriptor = 05ab\n", prefix);
    check(text, true);
    snprintf(text, sizeof(text), "%sHidDescriptor = 05ab\n[other]\nHidAppId = 500\n", prefix);
    check(text, true);
    const char *bad[] = {
        "", "0", "05xz", "05ab\nHidDescriptor = 05ab",
        "05ab\nHidAppId = 2", "05ab\nHidSubClass = -1",
        "05ab\nHidVendorId = 65536", "05ab\nHidVersion = 1junk"
    };
    for (size_t i = 0; i < RG_COUNT(bad); ++i) {
        snprintf(text, sizeof(text), "%sHidDescriptor = %s\n", prefix, bad[i]);
        check(text, false);
    }
    check("[83:38:03:11:48:10]\nHidDescriptor = 05ab\n", false);
    check("[83:39:03:11:48:10]\nHidDescriptor = 05ab\n", false);
    snprintf(text, sizeof(text), "%sHidDescriptor = ", prefix);
    size_t offset = strlen(text);
    memset(text + offset, 'a', 1770);
    text[offset + 1770] = '\0';
    check(text, false);
    puts("Metadata parser: valid, unrelated-section, duplicate, missing, bounds and malformed-input tests passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="bt-metadata-") as directory:
    path = Path(directory)
    c_file = path / "test.c"
    c_file.write_text(harness + parser + preload + tests)
    executable = path / ("test.exe" if os.name == "nt" else "test")
    if os.name == "nt":
        command = ["cl", "/nologo", "/W3", str(c_file), f"/Fe:{executable}"]
    else:
        command = ["cc", "-Wall", "-Wextra", str(c_file), "-o", str(executable)]
    subprocess.run(command, cwd=path, check=True)
    subprocess.run([str(executable)], check=True)
