"""Linux /proc sampling; counters are cumulative, not instantaneous CPU %."""
import os
import pathlib
import time


def sample_processes(roots):
    processes = {}
    for path in pathlib.Path('/proc').glob('[0-9]*/stat'):
        try:
            text = path.read_text()
            end = text.rindex(')')
            values = text[end+2:].split()
            pid = int(path.parent.name)
            processes[pid] = dict(pid=pid, ppid=int(values[1]), name=text[text.index('(')+1:end],
                                  cpuTicks=int(values[11])+int(values[12]),
                                  startTicks=int(values[19]), rssBytes=int(values[21])*os.sysconf('SC_PAGE_SIZE'))
        except (OSError, ValueError, IndexError):
            continue
    selected = set(roots)
    while True:
        children = {pid for pid, row in processes.items() if row['ppid'] in selected}
        enlarged = selected | children
        if enlarged == selected:
            break
        selected = enlarged
    return dict(monotonicMs=int(time.monotonic()*1000), clockTicks=os.sysconf('SC_CLK_TCK'),
                processes=[processes[p] for p in sorted(selected) if p in processes])