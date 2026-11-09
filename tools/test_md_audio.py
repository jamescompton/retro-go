"""Test the production MD mixer without emulator or ESP32 dependencies."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "gwenesis/main/main.c").read_text()
start = source.index("static int audio_pending_sample;")
end = source.index("\ntypedef struct", start)
code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define AUDIO_BUFFER_LENGTH 1062
#define RG_GWENESIS_RAM_ATTR
typedef struct { int16_t left, right; } rg_audio_frame_t;
static bool yfm_enabled, sn76489_enabled;
static int16_t gwenesis_ym2612_buffer[AUDIO_BUFFER_LENGTH];
static int16_t gwenesis_sn76489_buffer[AUDIO_BUFFER_LENGTH];
""" + source[start:end] + r"""
int main(void) {
    yfm_enabled = true;
    gwenesis_ym2612_buffer[0] = 100;
    gwenesis_ym2612_buffer[1] = 300;
    assert(mix_audio_frame(mixed_audio, 2) == 1);
    assert(mixed_audio[0].left == 200 && mixed_audio[0].right == 200);
    sn76489_enabled = true;
    gwenesis_sn76489_buffer[0] = 200;
    gwenesis_sn76489_buffer[1] = 400;
    assert(mix_audio_frame(mixed_audio, 2) == 1);
    assert(mixed_audio[0].left == 500);
    yfm_enabled = false;
    assert(mix_audio_frame(mixed_audio, 2) == 1);
    assert(mixed_audio[0].left == 300);
    yfm_enabled = true;
    for (int i = 0; i < AUDIO_BUFFER_LENGTH; ++i) {
        gwenesis_ym2612_buffer[i] = INT16_MAX;
        gwenesis_sn76489_buffer[i] = INT16_MAX;
    }
    assert(mix_audio_frame(mixed_audio, 1) == 0);
    assert(audio_pending);
    assert(mix_audio_frame(mixed_audio, 1) == 1);
    assert(!audio_pending && mixed_audio[0].left == INT16_MAX);
    for (int i = 0; i < AUDIO_BUFFER_LENGTH; ++i) {
        gwenesis_ym2612_buffer[i] = INT16_MIN;
        gwenesis_sn76489_buffer[i] = INT16_MIN;
    }
    assert(mix_audio_frame(mixed_audio, AUDIO_BUFFER_LENGTH) == AUDIO_BUFFER_LENGTH / 2);
    assert(mixed_audio[0].left == INT16_MIN);
    yfm_enabled = sn76489_enabled = false;
    assert(mix_audio_frame(mixed_audio, 2) == 1);
    assert(mixed_audio[0].left == 0);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="md-audio-") as directory:
    path = Path(directory)
    test = path / "test.c"
    test.write_text(code)
    executable = path / ("test.exe" if os.name == "nt" else "test")
    command = (["cl", "/nologo", "/W3", str(test), f"/Fe:{executable}"]
               if os.name == "nt" else
               ["cc", "-Wall", "-Wextra", str(test), "-o", str(executable)])
    subprocess.run(command, cwd=path, check=True)
    subprocess.run([str(executable)], check=True)
print("MD mixing, PSG-only, disabled chips, clipping and odd sample carry passed")
