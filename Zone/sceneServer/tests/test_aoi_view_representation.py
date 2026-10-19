#!/usr/bin/env python3
"""分配结构回归检查，运行时语义由参考基准验证。"""
import pathlib
import unittest


class RepresentationTests(unittest.TestCase):
    def test_no_full_entity_hash_map_temporary(self):
        source = (pathlib.Path(__file__).resolve().parents[1] /
                  'src-cpp/service/aoi/aoiClientView.cc').read_text()
        self.assertNotIn('using EntityMap = std::unordered_map<EntityId, AoiEntity>', source,
                         'CurrentView still allocates one hash node per visible entity')


if __name__ == '__main__':
    unittest.main()