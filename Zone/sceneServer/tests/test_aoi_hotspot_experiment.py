#!/usr/bin/env python3
import copy
import importlib.util
import pathlib
import tempfile
import unittest

MODULE = pathlib.Path(__file__).with_name('aoi_hotspot_experiment.py')


class ExperimentTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), 'AOI manifest and measurement gate not implemented')
        spec = importlib.util.spec_from_file_location('aoi_experiment', MODULE)
        self.experiment = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.experiment)

    def test_exact_manifest(self):
        manifest = self.experiment.make_manifest()
        self.experiment.validate_manifest(manifest)
        self.assertEqual(len(manifest['cases']), 22)
        for bad in (manifest['cases'][:-1], manifest['cases'] + [manifest['cases'][0]],
                    manifest['cases'] + [dict(id='extra')]):
            altered = copy.deepcopy(manifest)
            altered['cases'] = bad
            with self.assertRaises(ValueError):
                self.experiment.validate_manifest(altered)
        altered = copy.deepcopy(manifest)
        altered['cases'][4]['rps'] = 21
        with self.assertRaises(ValueError):
            self.experiment.validate_manifest(altered)

    def test_frozen_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'binary'
            path.write_bytes(b'frozen')
            frozen = self.experiment.freeze_files([path])
            self.experiment.verify_files(frozen)
            path.write_bytes(b'changed')
            with self.assertRaises(ValueError):
                self.experiment.verify_files(frozen)

    def test_refuse_nonempty_or_symlink_workdir(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'work'
            self.experiment.prepare_workdir(path)
            (path / 'old').touch()
            with self.assertRaises(ValueError):
                self.experiment.prepare_workdir(path)
            link = pathlib.Path(tmp) / 'link'
            link.symlink_to(path, target_is_directory=True)
            with self.assertRaises(ValueError):
                self.experiment.prepare_workdir(link)

    def test_phase_gate_and_overlap(self):
        gate = self.experiment.PhaseGate()
        with gate.activity('build'):
            with self.assertRaises(ValueError):
                with gate.measurement('case'):
                    pass
        with gate.measurement('case'):
            with self.assertRaises(ValueError):
                with gate.activity('test'):
                    pass
        self.assertFalse(self.experiment.overlaps(gate.intervals))
        self.assertTrue(self.experiment.overlaps([
            dict(kind='measurement', name='case', begin=10, end=20),
            dict(kind='activity', name='build', begin=19, end=21)]))

    def test_no_profiler_or_diagnostics_formal(self):
        plan = self.experiment.runtime_options(False)
        self.assertFalse(plan['profile'])
        self.assertNotIn('SCENE_QUEUE_DIAGNOSTICS', plan['environment'])
        self.assertEqual(plan['defer_postprocess'], True)

    def test_case_translation(self):
        self.assertTrue(hasattr(self.experiment, 'matrix_case'), 'new runner translation missing')
        case = self.experiment.make_manifest()['cases'][0]
        self.assertEqual(self.experiment.matrix_case(case), dict(gateways=3, scenes=2,
            connections=256, rps=20, persistence=True, duration=30))


if __name__ == '__main__':
    unittest.main()