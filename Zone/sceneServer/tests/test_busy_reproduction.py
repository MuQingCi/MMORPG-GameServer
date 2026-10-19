import json
import pathlib
import tempfile
import unittest
from busy_reproduction import Safety, last_json, verify
from busy_resources import task_sample


class BusyRunnerTests(unittest.TestCase):
    def test_safety_uses_deltas_and_waits_ten_seconds(self):
        safety=Safety()
        def rows(s,e,c): return [dict(elapsedMs=1000,client_sent=s,client_errors=e,client_business_succeeded=c)]*3
        self.assertIsNone(safety.check(rows(100,0,100),0))
        for t in range(1,11): self.assertIsNone(safety.check(rows(100+t*100,t*20,100+t*80),t))
        self.assertEqual(safety.check(rows(1200,220,980),11),'error-rate')

    def test_no_success_and_missing_samples(self):
        safety=Safety()
        row=dict(elapsedMs=1000,client_sent=1,client_errors=0,client_business_succeeded=1)
        self.assertIsNone(safety.check([row]*3,0))
        self.assertEqual(safety.check([row]*3,30),'no-success')
        self.assertIsNone(Safety().check([None]*3,100))

    def test_last_json_and_live_resource_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path=pathlib.Path(directory)/'sample'
            path.write_text('{"n":1}\n{"n":2}\n')
            self.assertEqual(last_json(path),dict(n=2))
        import os
        row=task_sample(pathlib.Path('/proc/self/task')/str(os.getpid()))
        self.assertGreaterEqual(row['cpuTicks'],0)
        self.assertIn('runqueueWaitNs',row)

    def test_verify_requires_all_players_and_matching_db(self):
        with tempfile.TemporaryDirectory() as directory:
            work=pathlib.Path(directory)
            def write(name,value): (work/name).write_text(json.dumps(value))
            players=[dict(playerId=i,sceneId=10) for i in range(256)]
            write('players.json',players)
            (work/'load-gw1.json.players.jsonl').write_text(''.join(json.dumps(dict(playerId=i,x=3,y=4))+'\n' for i in range(256)))
            (work/'final-db.tsv').write_text(''.join(f'{i}\t3\t4\t8\t10\n' for i in range(256)))
            (work/'final-bags.tsv').write_text(''.join(f'{i}\t101\t0\t9\n' for i in range(256)))
            write('result.json',dict(onlineSaveEvidence=dict(advancedPlayers=256),shutdown=dict(scene1=0)))
            write('load-gw1.json',dict(sent=10,completed=10,unresolved=0))
            row=dict(valid=True,logoutSucceeded=256,unresolved=0,serviceOk=True)
            self.assertTrue(verify(work,row))
            (work/'final-db.tsv').write_text('')
            self.assertFalse(verify(work,row))


if __name__=='__main__': unittest.main()