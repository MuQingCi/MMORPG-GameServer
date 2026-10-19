import copy
import unittest

import busy_placement as placement


class PlacementTests(unittest.TestCase):
    def setUp(self):
        self.players = [dict(playerId=10000000+i, gateway=1,
                             sceneId=10 if i % 2 == 0 else 20, x=5+i)
                        for i in range(4)]

    def test_exact_mapping_and_initial_state(self):
        self.assertTrue(placement.validate(self.players, copy.deepcopy(self.players))['matched'])

    def test_route_coordinate_and_gateway_changes_rejected(self):
        for field, value in [('sceneId', 20), ('x', 90), ('gateway', 2)]:
            actual = copy.deepcopy(self.players)
            actual[0][field] = value
            self.assertFalse(placement.validate(self.players, actual)['matched'])

    def test_missing_duplicate_and_extra_rejected(self):
        for actual in [self.players[:-1], self.players + [self.players[0]],
                       self.players[1:] + [self.players[1]]]:
            self.assertFalse(placement.validate(self.players, actual)['matched'])

    def test_order_does_not_change_fingerprint(self):
        value = placement.validate(self.players, list(reversed(self.players)))
        self.assertTrue(value['matched'])
        self.assertEqual(value['expectedFingerprint'], value['actualFingerprint'])

    def test_explicit_worker_mismatch_rejected(self):
        actual = copy.deepcopy(self.players)
        actual[0]['worker'] = (placement.actor_worker(actual[0]['playerId']) + 1) % 4
        self.assertFalse(placement.validate(self.players, actual)['matched'])

    def test_y_and_initial_version_changes_rejected(self):
        for field, value in [('y', 9), ('saveVersion', 8)]:
            actual = copy.deepcopy(self.players)
            actual[0][field] = value
            self.assertFalse(placement.validate(self.players, actual)['matched'])


if __name__ == '__main__':
    unittest.main()