#!/usr/bin/env python3
import csv
import os
import gzip
import json
from pathlib import Path
from statistics import median

suffix = '-nodelay' if os.environ.get('REDLET_BENCH_NODELAY', '0') == '1' else ''
ROOT = Path(__file__).resolve().parents[1] / ('results/2026-11-09' + suffix)


def load(path):
    if path.suffix == '.gz':
        with gzip.open(path, 'rt') as stream:
            return json.load(stream)
    return json.loads(path.read_text())


def percentile(values, fraction):
    return sorted(values)[int((len(values) - 1) * fraction)]


def histogram_percentile(histogram, fraction):
    target = int((sum(count for _, count in histogram) - 1) * fraction)
    seen = 0
    for microseconds, count in histogram:
        seen += count
        if seen > target:
            return microseconds / 1000
    raise ValueError('empty histogram')


def write_csv(name, rows):
    if rows:
        with (ROOT / name).open('w') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)


throughput = []
for path in sorted((ROOT / 'throughput').glob('*.json')):
    data = load(path)
    row = {key: data[key] for key in ['case', 'version', 'repeat', 'requests', 'connections', 'pipeline', 'errors']}
    row.update({key: float(value) for key, value in data['benchmark'].items() if key != 'test'})
    row['server_us_per_request'] = data['server_cpu_s'] * 1e6 / data['requests']
    row['server_cpu_pct'] = data['server_cpu_s'] / data['wall_s'] * 100
    row['client_cpu_pct'] = data['client_cpu_s'] / data['wall_s'] * 100
    row['minor_faults'] = data['after']['minflt'] - data['before']['minflt']
    row['payload_mib_s'] = row['rps'] * data['payload'] / 2**20
    throughput.append(row)
write_csv('throughput.csv', throughput)

memory, samples = [], []
for path in sorted((ROOT / 'memory').glob('*.json')):
    data = load(path)
    prefix = {key: data[key] for key in ['case', 'version', 'repeat']}
    marks = {s['mark']: s for s in data['samples'] if s.get('mark') is not None}
    row = dict(prefix, before_mib=data['samples'][0]['rss']/2**20,
               sampled_peak_mib=max(s['rss'] for s in data['samples'])/2**20,
               alive=data['alive_connections'], errors=data['errors'])
    for point in [0, .5, 1, 2, 5, 10, 15]:
        row[f'idle_{point}_mib'] = marks[point]['rss']/2**20
    row['idle_cpu_s'] = marks[15]['cpu_s'] - marks[0]['cpu_s']
    memory.append(row)
    for sample in data['samples']:
        samples.append(dict(prefix, phase=sample['phase'], t=sample['t'], rss=sample['rss'],
                            vmsize=sample['VmSize'], vmhwm=sample['VmHWM'],
                            minflt=sample['minflt'], cpu_s=sample['cpu_s']))
write_csv('memory-summary.csv', memory)
write_csv('memory.csv', samples)

mixed = []
for path in sorted((ROOT / 'mixed').glob('*.json.gz')):
    data = load(path)
    row = {key: data[key] for key in ['case', 'version', 'repeat', 'requests', 'slow_completed', 'errors']}
    for source, label in [('latencies_ms', 'scheduled'), ('service_ms', 'service'), ('schedule_lag_ms', 'lag')]:
        for quantile in [.5, .95, .99]:
            row[f'{label}_p{int(quantile*100)}_ms'] = histogram_percentile(data[source + '_hist_us'], quantile)
    row['server_cpu_s'] = data['after']['cpu_s'] - data['before']['cpu_s']
    mixed.append(row)
write_csv('mixed.csv', mixed)

cycles = []
for path in sorted((ROOT / 'cycles').glob('*.json')):
    data = load(path)
    row = {key: data[key] for key in ['case', 'version', 'repeat', 'errors']}
    rounds = data['rounds'][1:]
    first = [value for r in rounds for value in r['first_ms']]
    steady = [value for r in rounds for value in r['steady_ms']]
    row.update(first_samples=len(first), first_p50_ms=percentile(first, .5), first_p95_ms=percentile(first,.95),
               steady_p50_ms=percentile(steady,.5), steady_p95_ms=percentile(steady,.95),
               first_minor_faults_median=median(r['first_after']['minflt']-r['before']['minflt'] for r in rounds),
               before_rss_mib=median(r['before']['rss']/2**20 for r in rounds))
    cycles.append(row)
write_csv('cycles.csv', cycles)

ack = []
for path in sorted((ROOT / 'ack-diagnostic').glob('*.json')):
    data = load(path)
    row = {key: data[key] for key in ['case', 'version', 'repeat', 'pipeline', 'requests', 'errors']}
    row.update({key: float(value) for key, value in data['benchmark'].items() if key != 'test'})
    ack.append(row)
write_csv('ack-diagnostic.csv', ack)

for title, rows, metrics in [
    ('ACK diagnostic', ack, ['rps', 'p50_latency_ms', 'p99_latency_ms']),
    ('Throughput', throughput, ['rps', 'p99_latency_ms', 'server_us_per_request', 'server_cpu_pct', 'client_cpu_pct']),
    ('Memory', memory, ['before_mib', 'sampled_peak_mib', 'idle_5_mib', 'idle_15_mib']),
    ('Mixed', mixed, ['scheduled_p99_ms', 'service_p99_ms', 'lag_p99_ms']),
    ('Cycles', cycles, ['first_p50_ms', 'first_p95_ms', 'steady_p50_ms', 'first_minor_faults_median', 'before_rss_mib'])]:
    print('\n' + title)
    for case in sorted({r['case'] for r in rows}):
        for version in ['B0', 'O']:
            selected = [r for r in rows if r['case'] == case and r['version'] == version]
            if selected:
                print(case, version, len(selected),
                      {metric: round(median(r[metric] for r in selected), 4) for metric in metrics})
