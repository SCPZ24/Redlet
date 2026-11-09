#!/usr/bin/env python3
import asyncio
import difflib
import os
import re
import subprocess
import tempfile
import sys
sys.dont_write_bytecode = True
import compare as c

os.sched_setaffinity(0, c.CLIENT_CPUS)
c.OUT /= 'copy-diagnostic'
c.OUT.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='redlet-copy-') as directory:
    archive = subprocess.check_output(['git','archive','e281a562ce5cae6e6f858b6772de2df7540cda69'])
    subprocess.run(['tar','-x','-C',directory],input=archive,check=True)
    if c.NODELAY:
        subprocess.run(['patch', '-d', directory, '-p1', '-i',
                        str(c.ROOT / 'benchmarks/3-chunk-pool/scripts/nodelay.patch')], check=True)
    header=c.Path(directory)/'src/net/connection.hpp'
    before=header.read_text()
    after=before.replace('class Connection{', 'inline std::uint64_t output_moved_bytes = 0;\n\nclass Connection{')
    after=after.replace('            outcoming.erase(0, delivered_pos);',
                        '            output_moved_bytes += outcoming.size() - delivered_pos;\n            outcoming.erase(0, delivered_pos);')
    header.write_text(after)
    (c.OUT/'b0-instrumentation.patch').write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/src/net/connection.hpp',tofile='b/src/net/connection.hpp')))
    subprocess.run(['g++','-std=c++17','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-pthread',
                    '-DPOOL_VERSION=0','-DTRACK_OUTPUT_MOVES','-I'+directory,
                    str(c.ROOT/'benchmarks/3-chunk-pool/scripts/diagnostic.cpp'),
                    '-Wl,--wrap=epoll_wait,--wrap=send,--wrap=sendmsg,--wrap=memmove',
                    '-o','/tmp/redlet-b0-copy'],check=True)
    c.BINARIES['B0']=c.Path('/tmp/redlet-b0-copy')
    with c.server('B0','memory') as process:
        data=asyncio.run(c.memory_run(process.pid,16,False))
        c.save(c.OUT/'memory-B0.json',data)
    records = [{key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
               for line in (c.OUT/'logs/memory-B0.log').read_text().splitlines() if line.startswith('DIAG')]
    before = [r for r in records if r['t'] <= data['samples'][0]['monotonic_ms']][-1]
    after = [r for r in records if r['t'] <= data['samples'][-1]['monotonic_ms']][-1]
    moved = after['output_moved_bytes'] - before['output_moved_bytes']
    c.save(c.OUT/'summary.json', dict(before=before, after=after, burst_output_moved_bytes=moved,
                                     payload_bytes=256*1048576, move_to_payload_ratio=moved/(256*1048576)))
    print('output copy diagnostic complete',flush=True)
