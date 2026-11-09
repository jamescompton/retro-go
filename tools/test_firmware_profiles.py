"""Check CYD firmware selection and production bookmark path isolation."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
env_source = (root / "components/retro-go/targets/esp32-3248s035r/env.py").read_text()
profiles = {"core": "retro-core", "md": "gwenesis", "doom": "prboom-go"}
original_profile = os.environ.get("RG_3248_PROFILE")
try:
    for profile, app in profiles.items():
        os.environ["RG_3248_PROFILE"] = profile
        state = {"os": os, "PROJECT_APPS": {name: [0, 16, 0] for name in
                 ("launcher", "retro-core", "gwenesis", "prboom-go", "fmsx")}}
        exec(compile(env_source, "env.py", "exec"), state)
        assert state["DEFAULT_APPS"] == f"launcher {app}"
        assert state["PROJECT_APPS"]["launcher"][2] == 0x170000
        assert state["PROJECT_APPS"][app][2] == 0x170000
        assert 0x180000 + state["PROJECT_APPS"][app][2] <= 0x400000
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
    check("retro-core", "recent", "/sd/retro-go/config/recent.txt");
    check("retro-core", "favorite", "/sd/retro-go/config/favorite.txt");
#if RG_PROFILE_BOOKMARKS
    check("gwenesis", "recent", "/sd/retro-go/config/recent-md.txt");
    check("gwenesis", "favorite", "/sd/retro-go/config/favorite-md.txt");
    check("prboom-go", "recent", "/sd/retro-go/config/recent-doom.txt");
    check("prboom-go", "favorite", "/sd/retro-go/config/favorite-doom.txt");
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
    for enabled in (0, 1):
        executable = path / ("test.exe" if os.name == "nt" else "test")
        command = (["cl", "/nologo", "/W3", f"/DRG_PROFILE_BOOKMARKS={enabled}",
                    str(c_file), f"/Fe:{executable}"] if os.name == "nt" else
                   ["cc", "-Wall", "-Wextra", f"-DRG_PROFILE_BOOKMARKS={enabled}",
                    str(c_file), "-o", str(executable)])
        subprocess.run(command, cwd=path, check=True)
        subprocess.run([str(executable)], check=True)
print("Firmware profiles, invalid selection and bookmark isolation passed")
