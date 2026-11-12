"""Recording gate and coverage: exercise the production command macros with a fake driver."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm/android'


def harness_source():
    header = (DM / 'gfx_vk.h').read_text(encoding='utf-8')
    names = sorted(set(re.findall(r'X\((vkCmd\w+)\)', header)))
    wrappers = (DM / 'gfx_vk_sync_commands.h').read_text(encoding='utf-8')
    assert all('#define ' + n + '(' in wrappers for n in names)
    used = set()
    for path in DM.glob('gfx_vk*.c'):
        used.update(re.findall(r'\b(vkCmd\w+)\s*\(', path.read_text(encoding='utf-8')))
    assert used <= set(names), used - set(names)
    lifecycle = ['vkBeginCommandBuffer', 'vkEndCommandBuffer', 'vkResetCommandBuffer', 'vkResetCommandPool']
    text = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "vk_sync_gate.h"
typedef uintptr_t VkCommandBuffer;
static int g_vk_sync_active=1, calls;
static vk_sync_gate gate;
static void driver(VkCommandBuffer cb,...) {(void)cb;++calls;}
static void vk_sync_command(VkCommandBuffer cb,unsigned kind){(void)cb;(void)kind;vk_sync_gate_command(&gate);}
static void vk_sync_cancel(void){vk_sync_gate_command(&gate);}
static void vk_sync_barrier(VkCommandBuffer cb,...) {vk_sync_command(cb,0);}
'''
    for name in names + lifecycle:
        text += f'static void (*{name})(VkCommandBuffer,...) = driver;\n'
    text += '#include "gfx_vk_sync_commands.h"\nint main(void){\n'
    text += 'vk_sync_gate_mark(&gate,1,1);assert(vk_sync_gate_take(&gate,1));assert(!vk_sync_gate_take(&gate,1));\n'
    text += 'vk_sync_gate_mark(&gate,1,0);assert(!vk_sync_gate_take(&gate,1));\n'
    text += 'vk_sync_gate_mark(&gate,1,1);assert(!vk_sync_gate_take(&gate,2));\n'
    for name in names + lifecycle:
        args = '1,0,0,0,0,NULL,0,NULL,0,NULL' if name == 'vkCmdPipelineBarrier' else '1'
        text += f'vk_sync_gate_mark(&gate,1,1);{name}({args});assert(!vk_sync_gate_take(&gate,1));\n'
    text += 'vk_sync_gate_mark(&gate,1,1);vk_sync_cancel();assert(!vk_sync_gate_take(&gate,1));\n'
    text += f'assert(calls=={len(names)+len(lifecycle)});puts("recording gate: {len(names)+len(lifecycle)+4} cases passed");return 0;}}\n'
    return text


class RecordingGate(unittest.TestCase):
    def test_all_recording_entry_points_are_covered(self):
        self.assertIn('vkCmdWriteTimestamp', harness_source())

    @unittest.skipUnless(shutil.which('cc') or shutil.which('clang'), 'host compiler unavailable')
    def test_real_recording_macros_invalidate_adjacency(self):
        cc = shutil.which('cc') or shutil.which('clang')
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'gate.c'
            exe = Path(tmp) / ('gate.exe' if os.name == 'nt' else 'gate')
            source.write_text(harness_source(), encoding='utf-8')
            subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(DM), str(source), '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
