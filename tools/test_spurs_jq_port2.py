#!/usr/bin/env python3
"""cellSpursJq port2 push/sync through the real libs/spurs/cellSpursJq.c: both exports registered, the pushed job dispatched before the push returns."""
import os
from pathlib import Path
import subprocess
import tempfile
from gen_hle_nids import scan
from nid_database import compute_nid
root=Path(__file__).resolve().parents[1]
regs=scan(root/'libs/spurs/cellSpursJq.c')
for name, nid in (('_cellSpursJobQueuePort2PushJobBody',0xCF89F218),('cellSpursJobQueuePort2Sync',0xFACB3CED)):
    assert compute_nid(name)==nid, name
    assert (nid,name) in regs, f'Missing {name} export (an unregistered import fakes CELL_OK and drops the job)'
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking; registration checked')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spurs-jq-port2-') as tmp:
    out=Path(tmp)/'test'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),'-I'+str(root/'runtime/platform'),
        '-I'+str(root/'libs/spurs'),
        str(root/'tests/test_spurs_jq_port2.c'),str(root/'libs/spurs/cellSpursJq.c'),
        '-o',str(out)],check=True)
    subprocess.run([str(out)],check=True,timeout=15)
