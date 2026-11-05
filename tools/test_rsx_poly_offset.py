#!/usr/bin/env python3
"""The polygon offset methods (0x0A60/0x0A64/0x0A68 enables, 0x0A78 factor,
0x0A7C bias) are kept in rsx_state, and rsx_poly_offset_z24 converts them per
primitive for a 24-bit depth buffer (libs/video/tests/test_rsx_poly_offset.c,
built against rsx_commands.c)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: builds with a POSIX cc')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rsx-poly-offset-') as tmp:
    binary = Path(tmp) / 'test'
    extra = [] if sys.platform == 'darwin' else ['-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_rsx_poly_offset.c'),
                    str(root / 'libs/video/rsx_commands.c'), *extra,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=60, capture_output=True, text=True)
    print('\n'.join(l for l in result.stdout.splitlines() if not l.startswith('[RSX')))
    if result.returncode:
        raise SystemExit('test_rsx_poly_offset failed')
