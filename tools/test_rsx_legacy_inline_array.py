#!/usr/bin/env python3
"""INLINE_ARRAY (0x1818) primitives reach a vtable backend as draws whose
vertices read back from the FIFO's words (libs/video/tests/
test_rsx_legacy_inline_array.c, built against rsx_commands.c and
rsx_vertex_fetch.c, the reader the D3D12 backend's vertex-program path uses)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: builds with a POSIX cc')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rsx-legacy-inline-') as tmp:
    binary = Path(tmp) / 'test'
    extra = [] if sys.platform == 'darwin' else ['-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_rsx_legacy_inline_array.c'),
                    str(root / 'libs/video/rsx_commands.c'),
                    str(root / 'libs/video/rsx_vertex_fetch.c'), *extra,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=60, capture_output=True, text=True)
    out = result.stdout + result.stderr
    print('\n'.join(l for l in out.splitlines() if l.startswith(('[PASS]', '[FAIL]'))))
    if result.returncode:
        print(out[-3000:])
        raise SystemExit('test_rsx_legacy_inline_array failed')
