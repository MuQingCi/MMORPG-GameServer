"""只读采集 Linux task/cgroup 样本，明确保留不可用字段的状态。"""
import pathlib


def task_sample(task):
    text=(task/'stat').read_text()
    v=text[text.rindex(')')+2:].split()
    row=dict(tid=int(task.name),name=text[text.index('(')+1:text.rindex(')')],
             startTicks=int(v[19]),cpuTicks=int(v[11])+int(v[12]),
             minorFaults=int(v[7]),majorFaults=int(v[9]),state=v[0])
    for name in ('schedstat','status'):
        try:
            text=(task/name).read_text()
            if name=='schedstat':
                fields=text.split()
                row.update(runtimeNs=int(fields[0]),runqueueWaitNs=int(fields[1]),timeslices=int(fields[2]))
            else:
                row.update({k:int(val.strip()) for k,val in (line.split(':',1) for line in text.splitlines())
                            if k in ('voluntary_ctxt_switches','nonvoluntary_ctxt_switches')})
        except (OSError,ValueError,IndexError): row[name+'Unavailable']=True
    return row


def enrich(row):
    for process in row['processes']:
        proc=pathlib.Path('/proc')/str(process['pid'])
        tasks=[]
        for task in (proc/'task').glob('[0-9]*'):
            try: tasks.append(task_sample(task))
            except (OSError,ValueError,IndexError): continue
        process['tasks']=tasks
        try: process['affinity']=next(line for line in (proc/'status').read_text().splitlines() if line.startswith('Cpus_allowed_list:')).split(':')[1].strip()
        except (OSError,StopIteration): process['affinity']=None
    row['host']={}
    for name,path in (('loadavg','/proc/loadavg'),('cpuPressure','/proc/pressure/cpu'),
                      ('cpuStat','/sys/fs/cgroup/cpu.stat'),('cpuMax','/sys/fs/cgroup/cpu.max'),
                      ('memoryEvents','/sys/fs/cgroup/memory.events')):
        try: row['host'][name]=pathlib.Path(path).read_text()
        except OSError: row['host'][name]=None
    return row