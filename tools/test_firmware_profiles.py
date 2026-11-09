"""Check CYD firmware selection and production bookmark path isolation."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
env_source = (root / "components/retro-go/targets/esp32-3248s035r/env.py").read_text()
profiles = {"8bit": "retro-core", "16bit": "retro-core gwenesis",
            "core": "retro-core", "md": "gwenesis", "doom": "prboom-go"}
original_profile = os.environ.get("RG_3248_PROFILE")
try:
    for profile, app in profiles.items():
        os.environ["RG_3248_PROFILE"] = profile
        state = {"os": os, "PROJECT_APPS": {name: [0, 16, 0] for name in
                 ("launcher", "retro-core", "gwenesis", "prboom-go", "fmsx")}}
        exec(compile(env_source, "env.py", "exec"), state)
        assert state["DEFAULT_APPS"] == f"launcher {app}"
        assert state["PROJECT_APPS"]["launcher"][2] == 0x170000
        assert state["emulator_set"] == {"8bit": 1, "16bit": 2}.get(profile, 0)
        apps = state["DEFAULT_APPS"].split()
        assert state["PROJECT_APPS"]["retro-core"][2] == (0x110000 if profile == "16bit" else 0x170000)
        assert 0x10000 + sum(state["PROJECT_APPS"][app][2] for app in apps) <= 0x400000
    os.environ.pop("RG_3248_PROFILE")
    exec(compile(env_source, "env.py", "exec"), state)
    assert state["DEFAULT_APPS"] == "launcher retro-core"
    assert state["emulator_set"] == 1
    os.environ["RG_3248_PROFILE"] = "invalid"
    try:
        exec(compile(env_source, "env.py", "exec"), {"os": os, "PROJECT_APPS": {}})
    except ValueError:
        pass
    else:
        raise AssertionError("Invalid profile accepted")
finally:
    if original_profile is None:
        os.environ.pop("RG_3248_PROFILE", None)
    else:
        os.environ["RG_3248_PROFILE"] = original_profile

source = (root / "launcher/main/bookmarks.c").read_text()
start = source.index("#if RG_PROFILE_BOOKMARKS")
end = source.index("    book->name", start)
code = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define RG_BASE_PATH_CONFIG "/sd/retro-go/config"
static const char *installed;
static int rg_system_have_app(const char *app) {
    return strcmp(installed, app) == 0;
}
static void check(const char *app, const char *name, const char *expected) {
    installed = app;
    char path[256];
""" + source[start:end] + r"""
    assert(strcmp(path, expected) == 0);
}
int main(void) {
#if !RG_PROFILE_BOOKMARKS || RG_3248_EMULATOR_SET == 0
    check("retro-core", "recent", "/sd/retro-go/config/recent.txt");
    check("retro-core", "favorite", "/sd/retro-go/config/favorite.txt");
#endif
#if RG_PROFILE_BOOKMARKS
#if RG_3248_EMULATOR_SET == 1
    check("retro-core", "recent", "/sd/retro-go/config/recent-8bit.txt");
    check("retro-core", "favorite", "/sd/retro-go/config/favorite-8bit.txt");
#elif RG_3248_EMULATOR_SET == 2
    check("gwenesis", "recent", "/sd/retro-go/config/recent-16bit.txt");
    check("retro-core", "favorite", "/sd/retro-go/config/favorite-16bit.txt");
#else
    check("gwenesis", "recent", "/sd/retro-go/config/recent-md.txt");
    check("gwenesis", "favorite", "/sd/retro-go/config/favorite-md.txt");
    check("prboom-go", "recent", "/sd/retro-go/config/recent-doom.txt");
    check("prboom-go", "favorite", "/sd/retro-go/config/favorite-doom.txt");
#endif
#else
    check("gwenesis", "recent", "/sd/retro-go/config/recent.txt");
    check("prboom-go", "favorite", "/sd/retro-go/config/favorite.txt");
#endif
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="firmware-profiles-") as directory:
    path = Path(directory)
    c_file = path / "test.c"
    c_file.write_text(code)
    for enabled, emulator_set in ((0, 0), (1, 0), (1, 1), (1, 2)):
        executable = path / ("test.exe" if os.name == "nt" else "test")
        command = (["cl", "/nologo", "/W3", f"/DRG_3248_EMULATOR_SET={emulator_set}", f"/DRG_PROFILE_BOOKMARKS={enabled}",
                    str(c_file), f"/Fe:{executable}"] if os.name == "nt" else
                   ["cc", "-Wall", "-Wextra", f"-DRG_3248_EMULATOR_SET={emulator_set}", f"-DRG_PROFILE_BOOKMARKS={enabled}",
                    str(c_file), "-o", str(executable)])
        subprocess.run(command, cwd=path, check=True)
        subprocess.run([str(executable)], check=True)
print("Firmware profiles, invalid selection and bookmark isolation passed")

dispatch = (root / "retro-core/main/main.c").read_text().replace('#include "shared.h"', "")
stub = r"""
#include <assert.h>
#include <stdbool.h>
#include <string.h>
typedef struct { const char *configNs; } rg_app_t;
static rg_app_t app;
static const char *selected;
#define AUDIO_SAMPLE_RATE 32000
#define RG_LOGI(...) ((void)0)
#define RG_PANIC(...) ((void)0)
static rg_app_t *rg_system_init(int rate, void *a, void *b) { return &app; }
static void gbc_main(void) { selected = "gb"; }
static void nes_main(void) { selected = "nes"; }
static void pce_main(void) { selected = "pce"; }
static void sms_main(void) { selected = "sms"; }
static void snes_main(void) { selected = "snes"; }
static void lynx_main(void) { selected = "lnx"; }
static void gw_main(void) { selected = "gw"; }
static void launcher_main(void) { selected = "launcher"; }
"""
checks = r"""
int main(void) {
    const char *names[] = {"gb", "gbc", "nes", "pce", "sms", "gg", "snes", "lnx", "gw", "col", "invalid"};
    const char *cores[] = {"gb", "gb", "nes", "pce", "sms", "sms", "snes", "lnx", "launcher", "launcher", "launcher"};
    for (int i = 0; i < 11; ++i) {
        app.configNs = names[i];
        selected = NULL;
        app_main();
        const char *expected = cores[i];
        if ((RG_3248_EMULATOR_SET == 1 && i == 6) ||
            (RG_3248_EMULATOR_SET == 2 && i != 6))
            expected = "launcher";
        assert(selected && strcmp(selected, expected) == 0);
    }
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="firmware-dispatch-") as directory:
    path = Path(directory)
    c_file = path / "test.c"
    c_file.write_text(stub + dispatch + checks)
    for emulator_set in (0, 1, 2):
        executable = path / ("test.exe" if os.name == "nt" else "test")
        defines = ["RG_ENABLE_GAME_AND_WATCH=0", "RG_ENABLE_COLECOVISION=0",
                   f"RG_3248_EMULATOR_SET={emulator_set}"]
        command = (["cl", "/nologo", *[f"/D{x}" for x in defines],
                    str(c_file), f"/Fe:{executable}"] if os.name == "nt" else
                   ["cc", *[f"-D{x}" for x in defines], str(c_file), "-o", str(executable)])
        subprocess.run(command, cwd=path, check=True)
        subprocess.run([str(executable)], check=True)
print("Production emulator dispatch passed for legacy, 8-bit and 16-bit sets")
