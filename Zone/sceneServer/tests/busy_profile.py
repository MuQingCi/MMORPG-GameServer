#!/usr/bin/env python3
"""有界单次负载，包含两个运行态 CPU 调用栈采集窗口。"""
import argparse
import os
import pathlib
import signal
import subprocess
import time

import busy_reproduction as reproduction

matrix = reproduction.matrix


class Capture:
    def __init__(self, directory):
        self.directory = directory
        self.records = []
        self.running = []
        self.seen = set()

    def persist(self):
        matrix.write_json(self.directory/'profile-windows.json', self.records)

    def tick(self, start, now, pids):
        for process, handle, record in self.running:
            code = process.poll()
            if code is not None and record['status'] == 'running':
                status = 'failed' if code != 0 else ('captured' if now >= record['requestedEnd'] else 'incomplete')
                record.update(status=status, exitCode=code, observedExit=now)
                handle.close()
        if start is None or not pids:
            self.persist()
            return
        for label, begin, end in [('early', 10, 30), ('late', 90, 110)]:
            if label in self.seen or now < start+begin:
                continue
            self.seen.add(label)
            record = dict(label=label, requestedStart=start+begin, requestedEnd=start+end,
                          actualLaunch=now, pids=pids, status='missed')
            self.records.append(record)
            if now >= start+end:
                continue
            handle = (self.directory/f'{label}.perf.log').open('w')
            argv = ['/usr/sbin/perf', 'record', '-e', 'cpu-clock:u', '-F', '49',
                    '--call-graph', 'dwarf,8192', '-p', ','.join(map(str, pids)),
                    '-o', str(self.directory/f'{label}.perf.data'), '--', 'sleep',
                    f'{start+end-now:g}']
            try:
                process = subprocess.Popen(argv, stdout=handle, stderr=subprocess.STDOUT,
                                           start_new_session=True)
                record.update(status='running', command=argv)
                self.running.append((process, handle, record))
            except OSError as error:
                handle.close()
                record.update(status='failed', error=str(error))
        self.persist()

    def close(self):
        for process, handle, record in self.running:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGINT)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
                record.update(status='interrupted', exitCode=process.returncode)
            elif record['status'] == 'running':
                record.update(status='captured' if process.returncode == 0 else 'failed',
                              exitCode=process.returncode)
            handle.close()
        self.persist()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--batch', type=pathlib.Path, required=True)
    parser.add_argument('--label', choices=('before', 'after'), required=True)
    args = parser.parse_args()
    build, batch = args.build.resolve(), args.batch.resolve()
    batch.mkdir(parents=True, exist_ok=True)
    directory = batch/(args.label+'-profile')
    directory.mkdir(exist_ok=False)
    signal.signal(signal.SIGTERM, matrix.interrupted)
    signal.signal(signal.SIGINT, matrix.interrupted)
    os.environ.pop('SCENE_EXPERIMENT_ACTIVE_DB_INDEX', None)
    if args.label == 'after':
        os.environ['SCENE_EXPERIMENT_ACTIVE_DB_INDEX'] = '1'
    capture = Capture(directory)
    original = matrix.Runner.sample

    def sample(runner, output):
        original(runner, output)
        barrier = runner.work/'start-barrier/START'
        start = int(barrier.read_text())/1000+20 if barrier.exists() else None
        pids = [p.pid for name, p in runner.processes if name.startswith('scene') and p.poll() is None]
        capture.tick(start, time.monotonic(), pids)

    matrix.Runner.sample = sample
    try:
        value = reproduction.run_one(build, batch, args.label, True, 120)
        matrix.write_json(batch/(args.label+'-outcome.json'), value)
    finally:
        matrix.Runner.sample = original
        capture.close()


if __name__ == '__main__':
    main()