"""Compile the production Mini-Joystick decoder and test held/event semantics."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components" / "retro-go" / "rg_input.c").read_text()
start = source.index("static uint32_t mini_joystick_decode(")
end = source.index("\nstatic void mini_joystick_poll(", start)
code = """
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define RG_COUNT(x) (sizeof(x)/sizeof((x)[0]))
enum {RG_KEY_A=1, RG_KEY_B=2, RG_KEY_X=4, RG_KEY_Y=8, RG_KEY_MENU=16,
      RG_KEY_LEFT=32, RG_KEY_RIGHT=64, RG_KEY_UP=128, RG_KEY_DOWN=256,
      RG_KEY_SELECT=512, RG_KEY_START=1024};
""" + source[start:end] + """
int main(void) {
    uint8_t s[7] = {128,128,8,8,8,8,8};
    assert(mini_joystick_decode(s,0)==0);
    const uint32_t keys[]={RG_KEY_START,RG_KEY_B,RG_KEY_A,RG_KEY_SELECT,RG_KEY_MENU};
    for (int i=0;i<5;++i) {
        s[i+2]=0;
        uint32_t held=mini_joystick_decode(s,0);
        assert(held==keys[i]);
        s[i+2]=8;
        assert(mini_joystick_decode(s,held)==held);
        const int pressed[]={2,5,6};
        for (int j=0;j<3;++j) {
            s[i+2]=pressed[j];
            assert(mini_joystick_decode(s,0)==keys[i]);
        }
        const int released[]={1,3,4,7,255};
        for (int j=0;j<5;++j) {
            s[i+2]=released[j];
            assert(mini_joystick_decode(s,held)==0);
        }
        s[i+2]=8;
    }
    s[0]=0; s[1]=0;
    assert(mini_joystick_decode(s,0)==(RG_KEY_LEFT|RG_KEY_DOWN));
    s[0]=255; s[1]=255;
    assert(mini_joystick_decode(s,RG_KEY_LEFT|RG_KEY_DOWN)==(RG_KEY_RIGHT|RG_KEY_UP));
    s[0]=80; s[1]=176;
    assert(mini_joystick_decode(s,0)==0);
    assert(mini_joystick_decode(s,RG_KEY_LEFT|RG_KEY_UP)==(RG_KEY_LEFT|RG_KEY_UP));
    s[0]=104; s[1]=152;
    assert(mini_joystick_decode(s,RG_KEY_LEFT|RG_KEY_UP)==0);
    s[0]=128; s[1]=128; s[2]=0; s[3]=0;
    assert(mini_joystick_decode(s,0)==(RG_KEY_START|RG_KEY_B));
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="mini-joystick-") as directory:
    path = Path(directory)
    c_file = path / "test.c"
    c_file.write_text(code)
    executable = path / ("test.exe" if os.name == "nt" else "test")
    command = (["cl", "/nologo", "/W3", str(c_file), f"/Fe:{executable}"]
               if os.name == "nt" else ["cc", "-Wall", "-Wextra", str(c_file), "-o", str(executable)])
    subprocess.run(command, cwd=path, check=True)
    subprocess.run([str(executable)], check=True)
print("Mini-Joystick mapping, held buttons, releases, diagonals and dead-zone hysteresis passed")
