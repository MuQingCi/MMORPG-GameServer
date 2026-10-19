#!/usr/bin/env python3
"""第二轮 AOI 实验契约，不隐式重试或启动采样分析。"""
import contextlib
import hashlib
import os
import pathlib
import time
import json
import subprocess


def make_manifest():
    cases = []
    for variant in ('A', 'B'):
        for connections in (256, 512):
            cases.append(dict(id=f'diagnostic-{variant}-c{connections}', variant=variant,
                              connections=connections, duration=30, diagnostic=True, rps=20))
    for connections, duration in ((256, 30), (512, 30), (512, 60)):
        for rep in range(1, 4):
            for variant in (('A', 'B') if rep % 2 else ('B', 'A')):
                cases.append(dict(id=f'{variant}-c{connections}-d{duration}-rep{rep}',
                                  variant=variant, connections=connections, duration=duration,
                                  diagnostic=False, rps=20))
    return dict(schemaVersion=1, cases=cases)


def validate_manifest(manifest):
    if manifest != make_manifest():
        raise ValueError('manifest must match exact approved ordered case set and configuration')


def freeze_files(paths):
    result = {}
    for path in paths:
        path = pathlib.Path(path)
        if not path.is_absolute() or path.is_symlink() or not path.is_file():
            raise ValueError(f'not an absolute regular file: {path}')
        result[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def verify_files(frozen):
    if freeze_files(frozen) != frozen:
        raise ValueError('frozen binary/config/source mismatch')


def prepare_workdir(path):
    path = pathlib.Path(path)
    if not path.is_absolute() or path.is_symlink() or (path.exists() and
            (not path.is_dir() or any(path.iterdir()))):
        raise ValueError(f'refusing unsafe or nonempty workdir: {path}')
    path.mkdir(parents=True, exist_ok=True)


def runtime_options(diagnostic):
    env = os.environ.copy()
    env.pop('SCENE_QUEUE_DIAGNOSTICS', None)
    if diagnostic:
        env['SCENE_QUEUE_DIAGNOSTICS'] = '1'
    return dict(environment=env, profile=False, defer_postprocess=True)


def overlaps(intervals):
    return [(m['name'], a['name']) for m in intervals for a in intervals
            if m['kind'] == 'measurement' and a['kind'] == 'activity'
            and max(m['begin'], a['begin']) < min(m['end'], a['end'])]


class PhaseGate:
    """单协调器门控，无法约束未登记的外部活动。"""
    def __init__(self):
        self.intervals = []
        self.active = []

    @contextlib.contextmanager
    def _interval(self, kind, name):
        if any(x['kind'] == 'measurement' or kind == 'measurement' for x in self.active):
            raise ValueError('measurement must be exclusive of owned activity')
        item = dict(kind=kind, name=name, begin=time.monotonic_ns(), end=None)
        self.active.append(item)
        try:
            yield
        finally:
            item['end'] = time.monotonic_ns()
            self.active.remove(item)
            self.intervals.append(item)

    def activity(self, name):
        return self._interval('activity', name)

    def measurement(self, name):
        return self._interval('measurement', name)


def matrix_case(case):
    return dict(gateways=3, scenes=2, connections=case['connections'], rps=case['rps'],
                persistence=True, duration=case['duration'])


def run_case(build, batch, case, gate, verify_aoi=False):
    """整个用例独占执行，该路径不运行聚合/压缩/分析器。"""
    import benchmark_matrix as matrix
    build, batch = pathlib.Path(build), pathlib.Path(batch)
    if batch.exists():
        raise ValueError(f'refusing previous case: {batch}')
    prepare_workdir(batch)
    config = matrix_case(case)
    frozen = freeze_files([build/'bin'/name for name in
                          ('sceneServer', 'gatewayServer', 'gateway_load_client')])
    matrix.write_json(batch/'matrix.json', [config])
    matrix.write_json(batch/'environment.json', dict(build=str(build), case=case,
        diagnostic=case['diagnostic'], binaries={pathlib.Path(k).name:v for k,v in frozen.items()}))
    original = matrix.Runner.start
    options = runtime_options(case['diagnostic'])

    def start(runner, name, args):
        handle = (runner.work/(name+'.stdout.log')).open('w')
        runner.handles.append(handle)
        env = options['environment'].copy()
        env.pop('AOI_VERIFY_TOTAL', None)
        if verify_aoi and name.startswith('client'):
            env['AOI_VERIFY_TOTAL'] = str(case['connections'])
        if not name.startswith('scene'):
            env.pop('SCENE_QUEUE_DIAGNOSTICS', None)
        process = subprocess.Popen(args, cwd=runner.work, stdout=handle,
                                   stderr=subprocess.STDOUT, env=env)
        runner.processes.append((name, process))
        return process

    verify_files(frozen)
    matrix.Runner.start = start
    print('START', case['id'], flush=True)
    try:
        with gate.measurement(case['id']):
            result = matrix.run_case(build, batch/matrix.identity(config), config, 0)
    finally:
        matrix.Runner.start = original
        matrix.write_json(batch/'phase-intervals.json', gate.intervals)
    verify_files(frozen)
    matrix.write_json(batch/'completion.json', dict(completed=1))
    print('END', case['id'], 'passed=', result['passed'], flush=True)
    return result