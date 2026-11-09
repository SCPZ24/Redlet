#!/usr/bin/env python3
import csv
import os
import json
from pathlib import Path
from statistics import median
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

suffix = '-nodelay' if os.environ.get('REDLET_BENCH_NODELAY', '0') == '1' else ''
root = Path(__file__).resolve().parents[1] / ('results/2026-11-09' + suffix)
colors = {'B0': '#64748b', 'O': '#0f766e'}
fig, axes = plt.subplots(1, 2, figsize=(12, 4.3))
fig.suptitle('Server TCP_NODELAY ' + ('on' if suffix else 'off'))
for group, active, style in [(16, 'idle', '-'), (16, 'active', '--'), (64, 'idle', ':')]:
    case = f'S3-g{group}-{active}'
    for version in ['B0', 'O']:
        runs = [json.loads(p.read_text()) for p in (root/'memory').glob(f'{case}-r*-{version}.json')]
        if not runs:
            continue
        values = [[s['rss']/2**20 for s in r['samples'] if s['phase']=='idle'] for r in runs]
        middle = [median(row[i] for row in values) for i in range(61)]
        times = [i*.25 for i in range(61)]
        axes[0].plot(times, middle, style, color=colors[version], label=f'{version}, group={group}, {active}')
        if version == 'O':
            axes[1].plot(times, middle, style, label=f'group={group}, {active}')
            axes[1].fill_between(times, [min(row[i] for row in values) for i in range(61)],
                                 [max(row[i] for row in values) for i in range(61)], alpha=.12)
for ax in axes:
    ax.set(xlabel='Seconds after all large replies drained', ylabel='Server RSS (MiB)')
    ax.grid(alpha=.2)
    ax.legend(fontsize=8)
axes[0].set_title('256 connections stay open — median of 5 runs')
axes[1].set_title('Chunk pool detail — shaded min/max')
fig.tight_layout()
fig.savefig(root/'memory.png', dpi=180)
plt.close(fig)

rows = list(csv.DictReader((root/'throughput.csv').open()))
cases = list(dict.fromkeys(r['case'] for r in rows))
fig, axes = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
fig.suptitle('Server TCP_NODELAY ' + ('on' if suffix else 'off'))
for ax, metric, title in [(axes[0], 'rps', 'Throughput ratio O / B0 (higher is better)'),
                           (axes[1], 'p99_latency_ms', 'Complete-reply / batch p99 ratio O / B0 (lower is better)')]:
    ratios = []
    for case in cases:
        values = {v: median(float(r[metric]) for r in rows if r['case']==case and r['version']==v) for v in ['B0','O']}
        ratios.append(values['O']/values['B0'])
    if metric == 'p99_latency_ms':
        ax.bar(cases, [value - 1 for value in ratios], bottom=1, color='#0f766e')
        ax.set_yscale('log')
        ax.set_ylim(min(ratios) * .7, max(ratios) * 1.5)
    else:
        ax.bar(cases, ratios, color='#0f766e')
    ax.axhline(1, color='#64748b', linestyle='--')
    ax.set_title(title)
    ax.grid(axis='y', alpha=.2)
    for i, value in enumerate(ratios):
        ax.text(i, value, f'{value:.2f}x', ha='center',
                va='top' if metric == 'p99_latency_ms' and value < 1 else 'bottom', fontsize=9)
axes[1].tick_params(axis='x', rotation=25)
fig.tight_layout()
fig.savefig(root/'performance.png', dpi=180)
