#!/usr/bin/env python3
import asyncio
import os
import sys
sys.dont_write_bytecode = True
import compare as c

os.sched_setaffinity(0, c.CLIENT_CPUS)
c.OUT /= 'diagnostics'
c.BINARIES = {'B0': c.Path('/tmp/redlet-b0-diag'), 'O': c.Path('/tmp/redlet-o-diag')}
for version in ['B0', 'O']:
    for case in ['memory', 'memory64', 'mixed']:
        with c.server(version, case) as process:
            if case in ['memory', 'memory64']:
                group = 64 if case == 'memory64' else 16
                data = asyncio.run(c.memory_run(process.pid, group, False))
            else:
                data = asyncio.run(c.mixed_run(process.pid, True, diagnostics=True))
            c.save(c.OUT / f'{case}-{version}.json.gz', data)
            print('diagnostic', case, version, 'complete', flush=True)
