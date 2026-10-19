import unittest
from busy_analysis import classify, stop_reason, correlate


class BusyAnalysisTests(unittest.TestCase):
    def test_counts_are_rejections_not_attempts(self):
        event=dict(monotonicUs=100,worker=0,reason=0,playerId=2,seq=3)
        rows=[dict(monotonicUs=200,busy=dict(events=[event],counts=[1,0,0],dropped=0)),
              dict(monotonicUs=300,busy=dict(events=[],counts=[1,0,0],dropped=0))]
        result=classify(rows,50,200)
        self.assertEqual(result['windowCounts'],[1,0,0])
        self.assertEqual(result['firstOffsetUs'],50)
        self.assertEqual(result['finalCounts'],[1,0,0])

    def test_drops_not_silently_complete(self):
        row=dict(monotonicUs=200,busy=dict(events=[],counts=[2,0,0],dropped=2))
        self.assertFalse(classify([row],0,300)['complete'])

    def test_timeout_not_busy_and_safety_windows(self):
        self.assertIsNone(stop_reason(100,11,1,0))
        self.assertEqual(stop_reason(100,11,10,0),'error-rate')
        self.assertEqual(stop_reason(0,0,0,30),'no-success')

    def test_event_time_not_collection_time(self):
        row=dict(monotonicUs=1000,busy=dict(events=[dict(monotonicUs=100,reason=2)],counts=[0,0,1],dropped=0))
        self.assertEqual(classify([row],200,800)['windowCounts'],[0,0,0])

    def test_counter_reset_rejected(self):
        def row(t,c): return dict(monotonicUs=t,busy=dict(events=[],counts=[c,0,0],dropped=0))
        with self.assertRaises(ValueError): classify([row(1,2),row(2,1)],0,3)

    def test_gateway_maps_internal_seq_to_client_request(self):
        event=dict(gatewayId=2,clientSession=3,seq=100,playerId=4,monotonicUs=10)
        mapping=[dict(gatewayId=2,clientSession=3,seq=100,requestId=7)]
        client=[dict(playerId=4,requestId=7,receivedUs=20)]
        self.assertEqual(correlate([event],mapping,client)['matched'],1)
        self.assertEqual(correlate([event],mapping,[dict(playerId=4,requestId=100,receivedUs=20)])['matched'],0)

    def test_bad_order_and_duplicate_are_not_complete(self):
        event=dict(gatewayId=2,clientSession=3,seq=100,playerId=4,monotonicUs=10)
        mapping=[dict(gatewayId=2,clientSession=3,seq=100,requestId=7)]
        client=dict(playerId=4,requestId=7,receivedUs=9)
        self.assertFalse(correlate([event],mapping,[client])['complete'])
        client['receivedUs']=20
        self.assertFalse(correlate([event],mapping,[client,client])['complete'])


if __name__=='__main__': unittest.main()