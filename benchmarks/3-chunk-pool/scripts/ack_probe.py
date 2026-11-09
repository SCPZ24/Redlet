#!/usr/bin/env python3
import csv
import io
import os
import subprocess
import sys
sys.dont_write_bytecode = True
import compare as c

os.sched_setaffinity(0, c.CLIENT_CPUS)
c.OUT /= 'ack-diagnostic'
# Same byte validation and complete-reply timer; re-arm QUICKACK after each successful recv.
c.LOAD_BINARY = '/tmp/redlet-load-quickack'
for key, pipeline, count in [('large', 1, 6000), ('medium', 16, 96000)]:
    for repeat in range(5):
        for version in (['B0','O'] if repeat%2==0 else ['O','B0']):
            label=f'{key}-p{pipeline}-r{repeat}'
            path=c.OUT/f'{label}-{version}.json'
            if path.exists(): continue
            with c.server(version, label) as process:
                subprocess.run(c.benchmark_args(key,1,pipeline,count//2),capture_output=True,check=True)
                before=c.proc_stats(process.pid)
                result=subprocess.run(c.benchmark_args(key,1,pipeline,count),capture_output=True,text=True,check=True)
                after=c.proc_stats(process.pid)
                rows=list(csv.reader(io.StringIO(result.stdout)))
                data=dict(case=key, pipeline=pipeline, repeat=repeat, version=version, requests=count,
                          benchmark=dict(zip(rows[0], rows[-1])), before=before,after=after, errors=0)
                c.save(path,data)
                print('QUICKACK',label,version,data['benchmark'],flush=True)
