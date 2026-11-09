#!/usr/bin/env python3
"""Run inside the Redlet Linux container. Each measured run gets a fresh server."""
import argparse
import asyncio
import csv
import hashlib
import gzip
import io
import json
import os
from pathlib import Path
import resource
import socket
import statistics
import subprocess
import time
from contextlib import contextmanager
from collections import Counter

ROOT = Path(__file__).resolve().parents[3]
NODELAY = os.environ.get('REDLET_BENCH_NODELAY', '0') == '1'
SUFFIX = '-nodelay' if NODELAY else ''
OUT = ROOT / ('benchmarks/3-chunk-pool/results/2026-11-09' + SUFFIX)
BINARIES = {'B0': ROOT / ('redlet-64kb-buffer' + SUFFIX),
            'O': ROOT / ('redlet-chunk-pool' + SUFFIX)}
SIZES = {'small': 64, 'medium': 65536, 'large': 1048576}
SERVER_CPU = 0
CLIENT_CPUS = {2, 3, 4, 5}
LOAD_BINARY = '/tmp/redlet-load'


def command(*parts):
    parts = [p.encode() if isinstance(p, str) else p for p in parts]
    return b'*%d\r\n' % len(parts) + b''.join(b'$%d\r\n' % len(p) + p + b'\r\n' for p in parts)


def connect():
    sock = socket.create_connection(('127.0.0.1', 6379), timeout=30)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def exact(sock, size):
    chunks = []
    while size:
        part = sock.recv(size)
        if not part:
            raise RuntimeError('unexpected EOF')
        chunks.append(part)
        size -= len(part)
    return b''.join(chunks)


def proc_stats(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1].split()
    status = {}
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if ':' in line:
            key, value = line.split(':', 1)
            if key in ('VmSize', 'VmHWM', 'VmPTE', 'VmRSS'):
                status[key] = int(value.split()[0]) * 1024
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines():
        if line.startswith('Rss:'):
            status['rss'] = int(line.split()[1]) * 1024
        if line.startswith('AnonHugePages:'):
            status['anon_huge_bytes'] = int(line.split()[1]) * 1024
    ticks = os.sysconf('SC_CLK_TCK')
    status['monotonic_ms'] = int(time.monotonic() * 1000)
    status.update(cpu_s=(int(fields[11]) + int(fields[12])) / ticks,
                  minflt=int(fields[7]), majflt=int(fields[9]))
    return status


def delta(after, before, key):
    return after[key] - before[key]


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    value = value.copy()
    for key in ['latencies_ms', 'service_ms', 'schedule_lag_ms']:
        if key in value and len(value[key]) > 1000:
            counts = Counter(round(sample * 1000) for sample in value.pop(key))
            value[key + '_hist_us'] = sorted(counts.items())
    data = json.dumps(value, indent=2) + '\n'
    if path.suffix == '.gz':
        with gzip.open(path, 'wt') as stream:
            stream.write(data)
    else:
        path.write_text(data)


@contextmanager
def server(version, label):
    with socket.socket() as probe:
        if probe.connect_ex(('127.0.0.1', 6379)) == 0:
            raise RuntimeError('port 6379 already in use')
    log_path = OUT / 'logs' / f'{label}-{version}.log'
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open('w') as log:
        process = subprocess.Popen(['taskset', '-c', str(SERVER_CPU), str(BINARIES[version])],
                                   stdout=log, stderr=log)
        try:
            for _ in range(200):
                if process.poll() is not None:
                    raise RuntimeError(log_path.read_text())
                try:
                    with connect() as sock:
                        sock.sendall(command('PING'))
                        assert exact(sock, 7) == b'+PONG\r\n'
                    break
                except ConnectionRefusedError:
                    time.sleep(.01)
            else:
                raise RuntimeError('server startup timeout')
            with connect() as sock:
                for key, size in SIZES.items():
                    sock.sendall(command('SET', key, b'x' * size))
                    assert exact(sock, 5) == b'+OK\r\n'
                    sock.sendall(command('GET', key))
                    expected = b'$%d\r\n' % size + b'x' * size + b'\r\n'
                    assert exact(sock, len(expected)) == expected
            time.sleep(.1)
            yield process
            if process.poll() is not None:
                raise RuntimeError('server exited during experiment')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def benchmark_args(key, connections, pipeline, count):
    unit = connections * pipeline
    count = max(unit, count // unit * unit)
    return [LOAD_BINARY, key, str(connections), str(pipeline), str(count)]



def throughput(repeats):
    cases = [('S1-c1', 'small', 1, 1), ('S1-c64', 'small', 64, 1),
             ('S1-c256', 'small', 256, 1), ('S1-p32', 'small', 8, 32),
             ('S2-large-c1', 'large', 1, 1), ('S2-large-c8', 'large', 8, 1),
             ('S2-medium-c1-p16', 'medium', 1, 16), ('S2-medium-c8-p16', 'medium', 8, 16)]
    for name, key, connections, pipeline in cases:
        count = None
        for repeat in range(repeats):
            for version in (['B0', 'O'] if repeat % 2 == 0 else ['O', 'B0']):
                label = f'{name}-r{repeat}'
                path = OUT / 'throughput' / f'{label}-{version}.json'
                if path.exists():
                    count = json.loads(path.read_text())['requests']
                    continue
                with server(version, label) as process:
                    if count is None:
                        calibration = subprocess.run(benchmark_args(key, connections, pipeline, 2000),
                                                     capture_output=True, text=True, check=True)
                        rows = list(csv.reader(io.StringIO(calibration.stdout)))
                        rate = float(rows[-1][1])
                        calibration_count = max(connections * pipeline, int(rate))
                        calibration = subprocess.run(benchmark_args(key, connections, pipeline, calibration_count),
                                                     capture_output=True, text=True, check=True)
                        rate = float(list(csv.reader(io.StringIO(calibration.stdout)))[-1][1])
                        count = max(connections * pipeline, int(rate * 5) // (connections * pipeline) * connections * pipeline)

                    # Finish every warm-up reply so B0 never sees a reset from an aborted client.
                    warm_count = max(connections * pipeline, count // 2 // (connections * pipeline) * connections * pipeline)
                    subprocess.run(benchmark_args(key, connections, pipeline, warm_count),
                                   capture_output=True, text=True, check=True, timeout=120)
                    before = proc_stats(process.pid)
                    child_before = resource.getrusage(resource.RUSAGE_CHILDREN)
                    started = time.perf_counter()
                    result = subprocess.run(benchmark_args(key, connections, pipeline, count),
                                            capture_output=True, text=True, timeout=300, check=True)
                    wall = time.perf_counter() - started
                    child_after = resource.getrusage(resource.RUSAGE_CHILDREN)
                    after = proc_stats(process.pid)
                    rows = list(csv.reader(io.StringIO(result.stdout)))
                    header, values = rows[0], rows[-1]
                    measurements = dict(zip(header, values))
                    record = dict(case=name, version=version, repeat=repeat, requests=count,
                                  key=key, payload=SIZES[key], connections=connections, pipeline=pipeline,
                                  wall_s=wall, benchmark=measurements, stdout=result.stdout, stderr=result.stderr,
                                  before=before, after=after, errors=0,
                                  server_cpu_s=delta(after, before, 'cpu_s'),
                                  client_cpu_s=child_after.ru_utime + child_after.ru_stime
                                               - child_before.ru_utime - child_before.ru_stime)
                    save(path, record)
                    print(label, version, measurements, flush=True)


async def async_connection(receive_buffer=None):
    sock = socket.socket()
    sock.setblocking(False)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if receive_buffer:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
    await asyncio.get_running_loop().sock_connect(sock, ('127.0.0.1', 6379))
    return await asyncio.open_connection(sock=sock, limit=32768 if receive_buffer else 2 * 1024 * 1024)


async def get(reader, writer, key='large'):
    size = SIZES[key]
    expected = b'x' * size
    start = time.perf_counter()
    writer.write(command('GET', key))
    await writer.drain()
    assert await reader.readline() == b'$%d\r\n' % size
    assert await reader.readexactly(size) == expected
    assert await reader.readexactly(2) == b'\r\n'
    return (time.perf_counter() - start) * 1000


async def close_connections(connections):
    for _, writer in connections:
        writer.close()
    await asyncio.gather(*(writer.wait_closed() for _, writer in connections))


async def memory_run(pid, group, active):
    connections = [await async_connection() for _ in range(256)]
    try:
        deadline = time.perf_counter() + 5
        while time.perf_counter() < deadline:
            await asyncio.gather(*(get(*connection, 'small') for connection in connections))
            await asyncio.sleep(.05)
        samples = [dict(phase='before', t=0, **proc_stats(pid))]
        latencies = []
        begin = time.perf_counter()
        for offset in range(0, 256, group):
            latencies.extend(await asyncio.gather(*(get(*connection) for connection in connections[offset:offset+group])))
            samples.append(dict(phase='burst', t=time.perf_counter()-begin, **proc_stats(pid)))
        # All full replies have arrived. A small round-trip on every connection is a drain barrier.
        await asyncio.gather(*(get(*connection, 'small') for connection in connections))
        drained = time.perf_counter()
        marks = {0, .5, 1, 2, 5, 10, 15}
        for index in range(61):
            point = index * .25
            await asyncio.sleep(max(0, drained + point - time.perf_counter()))
            if active and index % 4 == 0:
                await get(*connections[index % len(connections)], 'small')
            samples.append(dict(phase='idle', t=time.perf_counter()-drained, mark=point if point in marks else None,
                                **proc_stats(pid)))
        await asyncio.gather(*(get(*connection, 'small') for connection in connections))
        return dict(samples=samples, latencies_ms=latencies, alive_connections=256, errors=0)
    finally:
        await close_connections(connections)


def memory(repeats):
    for group, active in [(16, False), (16, True), (64, False)]:
        name = f'S3-g{group}-' + ('active' if active else 'idle')
        for repeat in range(repeats):
            for version in (['B0', 'O'] if repeat % 2 == 0 else ['O', 'B0']):
                label = f'{name}-r{repeat}'
                path = OUT / 'memory' / f'{label}-{version}.json'
                if path.exists():
                    continue
                with server(version, label) as process:
                    record = asyncio.run(memory_run(process.pid, group, active))
                    record.update(case=name, version=version, repeat=repeat)
                    save(path, record)
                    print(label, version, 'RSS before/end MiB',
                          round(record['samples'][0]['rss']/2**20, 2),
                          round(record['samples'][-1]['rss']/2**20, 2), flush=True)


async def cycles_run(pid, interval):
    connections = [await async_connection() for _ in range(16)]
    rounds = []
    try:
        for index in range(11):
            before = proc_stats(pid)
            first = await asyncio.gather(*(get(*c) for c in connections))
            first_after = proc_stats(pid)
            steady = []
            for _ in range(4):
                steady.extend(await asyncio.gather(*(get(*c) for c in connections)))
            after = proc_stats(pid)
            rounds.append(dict(round=index, first_ms=first, steady_ms=steady,
                               before=before, first_after=first_after, after=after))
            if index != 10:
                await asyncio.sleep(interval)
        return dict(rounds=rounds, errors=0)
    finally:
        await close_connections(connections)


def cycles(repeats):
    for interval in [.5, 5, 11]:
        name = f'S6-gap{interval}'
        for repeat in range(repeats):
            for version in (['B0', 'O'] if repeat % 2 == 0 else ['O', 'B0']):
                label = f'{name}-r{repeat}'
                path = OUT / 'cycles' / f'{label}-{version}.json'
                if path.exists():
                    continue
                with server(version, label) as process:
                    record = asyncio.run(cycles_run(process.pid, interval))
                    record.update(case=name, version=version, repeat=repeat)
                    save(path, record)
                    print(label, version, 'complete', flush=True)


async def mixed_run(pid, slow, rate=5000, seconds=20, diagnostics=False):
    normal = [await async_connection() for _ in range(64)]
    slow_connections = [await async_connection(4096) for _ in range(4)] if slow else []
    latencies, service, schedule_lag = [], [], []
    slow_completed = 0
    start = time.perf_counter() + .1
    before = proc_stats(pid)
    async def worker(index, connection):
        for request_index in range(index, int(rate * seconds), len(normal)):
            scheduled = start + request_index / rate
            await asyncio.sleep(max(0, scheduled - time.perf_counter()))
            actual = time.perf_counter()
            duration = await get(*connection, 'small')
            latencies.append((time.perf_counter() - scheduled) * 1000)
            service.append(duration)
            schedule_lag.append((actual - scheduled) * 1000)
    async def slow_worker(connection):
        nonlocal slow_completed
        reader, writer = connection
        while time.perf_counter() < start + seconds:
            writer.write(command('GET', 'large') * 3)
            await writer.drain()
            for _ in range(3):
                assert await reader.readline() == b'$1048576\r\n'
                remaining = SIZES['large']
                while remaining:
                    size = min(32768, remaining)
                    assert await reader.readexactly(size) == b'x' * size
                    remaining -= size
                    await asyncio.sleep(size / 1048576)
                assert await reader.readexactly(2) == b'\r\n'
                slow_completed += 1
    samples = []
    async def sample_resources():
        while True:
            tcp_tx = 0
            for line in Path('/proc/net/tcp').read_text().splitlines()[1:]:
                fields = line.split()
                if fields[1].endswith(':18EB'):
                    tcp_tx += int(fields[4].split(':')[0], 16)
            memory = dict(line.split() for line in Path('/sys/fs/cgroup/memory.stat').read_text().splitlines())
            samples.append(dict(t=time.perf_counter()-start, tcp_tx_bytes=tcp_tx,
                                cgroup_sock_bytes=int(memory.get('sock', 0)), **proc_stats(pid)))
            await asyncio.sleep(.25)
    sampler = asyncio.create_task(sample_resources()) if diagnostics else None
    try:
        await asyncio.gather(*(worker(i, c) for i, c in enumerate(normal)),
                             *(slow_worker(c) for c in slow_connections))
        return dict(latencies_ms=latencies, service_ms=service, schedule_lag_ms=schedule_lag,
                    slow_completed=slow_completed, requests=len(latencies), before=before, samples=samples,
                    after=proc_stats(pid), errors=0, rate=rate, seconds=seconds)
    finally:
        if sampler:
            sampler.cancel()
            await asyncio.gather(sampler, return_exceptions=True)
        await close_connections(normal + slow_connections)


def mixed(repeats):
    for slow in [False, True]:
        name = 'S4-' + ('slow' if slow else 'control')
        for repeat in range(repeats):
            for version in (['B0', 'O'] if repeat % 2 == 0 else ['O', 'B0']):
                label = f'{name}-r{repeat}'
                path = OUT / 'mixed' / f'{label}-{version}.json.gz'
                if path.exists():
                    continue
                with server(version, label) as process:
                    record = asyncio.run(mixed_run(process.pid, slow))
                    record.update(case=name, version=version, repeat=repeat)
                    save(path, record)
                    print(label, version, 'p99 ms', sorted(record['latencies_ms'])[int(.99*len(record['latencies_ms']))], flush=True)


def manifest():
    commands = {'uname': ['uname', '-a'], 'compiler': ['g++', '--version'],
                'cpu': ['lscpu'], 'memory': ['free', '-m'], 'benchmark': ['redis-benchmark', '--version'],
                'commit': ['git', 'rev-parse', 'HEAD'], 'worktree': ['git', 'status', '--short']}
    result = {key: subprocess.check_output(args, text=True).strip() for key, args in commands.items()}
    result.update(server_tcp_nodelay=NODELAY, baseline_commit='e281a562ce5cae6e6f858b6772de2df7540cda69',
                  optimized_commit='e259da431380485633f2ce5dd5d42e61bc42388b',
                  server_build_flags='-std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror',
                  load_client='load.cpp, min(4,C) epoll threads on CPU 2..5; complete-batch latency; full byte validation',
                  throughput_repeats=5, target_duration_s=5, warmup_requests='half measured request count',
                  server_cpu=SERVER_CPU, client_cpus=sorted(CLIENT_CPUS),
                  page_size=os.sysconf('SC_PAGESIZE'), iov_max=os.sysconf('SC_IOV_MAX'),
                  fd_limit=resource.getrlimit(resource.RLIMIT_NOFILE),
                  binaries={key: {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                            for key, path in BINARIES.items()})
    for path in ['/sys/kernel/mm/transparent_hugepage/enabled', '/sys/fs/cgroup/cpu.max',
                 '/sys/fs/cgroup/memory.max', '/proc/sys/net/ipv4/tcp_rmem',
                 '/proc/sys/net/ipv4/tcp_wmem', '/proc/sys/net/ipv4/tcp_autocorking']:
        result[path] = Path(path).read_text().strip()
    result['working_server_header_sha256'] = hashlib.sha256((ROOT / 'src/server.hpp').read_bytes()).hexdigest()
    result['scripts_sha256'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in Path(__file__).parent.iterdir() if path.is_file()}
    save(OUT / 'manifest.json', result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('suite', choices=['manifest', 'throughput', 'memory', 'cycles', 'mixed'])
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--out', type=Path, default=OUT)
    args = parser.parse_args()
    OUT = args.out.resolve()
    os.sched_setaffinity(0, CLIENT_CPUS)
    OUT.mkdir(parents=True, exist_ok=True)
    if args.suite == 'manifest':
        manifest()
    else:
        globals()[args.suite](args.repeats)
