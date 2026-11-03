#!/usr/bin/env python3
"""Switch parsing, overlay precedence and rejected-input regression tests."""
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from feature_config import (Features, load_features, write_if_changed, overlay_path,
                            extra_conf_argument, verify_features, validate_build_dir)


class FeatureConfiguration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.path = self.root / 'features.json'

    def tearDown(self):
        self.temp.cleanup()

    def load(self, value):
        self.path.write_text(json.dumps(value))
        return load_features(self.path)

    def test_all_eight_boolean_combinations(self):
        for values in itertools.product((False, True), repeat=3):
            with self.subTest(values=values):
                data = dict(zip(('charging', 'imu', 'ppg'), values))
                features = self.load(data)
                self.assertEqual(features.as_dict(), data)
                verify_features(features.kconfig(), features)
                self.assertNotIn('PACK_APPROVED', features.kconfig())
                if not features.imu:
                    self.assertIn('CONFIG_SF_GYRO=n', features.kconfig())

    def test_missing_and_unknown_keys(self):
        for value in ({}, {'charging': False, 'imu': True},
                      {'charging': False, 'imu': True, 'ppg': True, 'gyro': True}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.load(value)

    def test_only_json_booleans_are_accepted(self):
        for name in ('charging', 'imu', 'ppg'):
            for value in (0, 1, -1, 1.0, 'true', 'false', 'yes', None, [], {}):
                data = {'charging': False, 'imu': True, 'ppg': True, name: value}
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    self.load(data)

    def test_non_objects_are_rejected(self):
        for value in ([], None, True, 1, 'true'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.load(value)

    def test_duplicate_keys_are_rejected(self):
        for text in ('{"charging": false, "imu": true, "ppg": true, "charging": true}',
                     '{"charging": false, "imu": false, "imu": true, "ppg": true}'):
            self.path.write_text(text)
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                load_features(self.path)

    def test_bad_json_and_missing_file(self):
        for text in ('{"charging": False}', '{broken', '{"charging": true,}'):
            self.path.write_text(text)
            with self.subTest(text=text), self.assertRaises(ValueError):
                load_features(self.path)
        self.path.unlink()
        with self.assertRaises(ValueError):
            load_features(self.path)

    def test_overlay_is_authoritative_last(self):
        overlay = self.root / 'feature overlay.conf'
        extra = self.root / 'extra.conf'
        self.assertEqual(extra_conf_argument(extra, overlay),
                         f'-DEXTRA_CONF_FILE={extra.resolve()};{overlay.resolve()}')
        # Passing no extra file still actively replaces a previous CMake value.
        self.assertEqual(extra_conf_argument(None, overlay),
                         f'-DEXTRA_CONF_FILE={overlay.resolve()}')

    def test_semicolon_path_is_rejected(self):
        with self.assertRaises(ValueError):
            extra_conf_argument(self.root / 'bad;name.conf', self.root / 'features.conf')

    def test_pristine_build_cannot_target_sources(self):
        firmware = self.root / 'firmware'
        for path in (firmware, self.root, firmware / 'src', firmware / 'src/build'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                validate_build_dir(firmware, path)
        validate_build_dir(firmware, firmware / 'build')
        validate_build_dir(firmware, firmware / 'build-custom/subdir')
        validate_build_dir(firmware, self.root / 'external-build')
        existing = self.root / 'existing-file'
        existing.write_text('keep')
        with self.assertRaises(ValueError):
            validate_build_dir(firmware, existing)

    def test_overlay_survives_pristine_and_changes_in_place(self):
        build = self.root / 'build'
        overlay = overlay_path(self.root, build)
        self.assertNotIn(build, overlay.parents)
        first = Features(False, True, True).kconfig()
        second = Features(True, False, False).kconfig()
        write_if_changed(overlay, first)
        self.assertEqual(overlay.read_text(), first)
        stat = overlay.stat().st_mtime_ns
        write_if_changed(overlay, first)
        self.assertEqual(overlay.stat().st_mtime_ns, stat)
        write_if_changed(overlay, second)
        self.assertEqual(overlay.read_text(), second)
        self.assertEqual(overlay_path(self.root, build), overlay)
        self.assertNotEqual(overlay_path(self.root, self.root / 'other-build'), overlay)

    def test_overlay_storage_cannot_be_pristine_build_target(self):
        firmware = self.root / 'firmware'
        for path in (firmware / 'build-config', firmware / 'build-config/1234',
                     firmware / 'build-config/1234/nested'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                validate_build_dir(firmware, path)

    def test_resolved_build_disagreement_is_rejected(self):
        features = Features(True, False, True)
        with self.assertRaises(ValueError):
            verify_features(Features(False, False, True).kconfig(), features)
        with self.assertRaises(ValueError):
            verify_features(features.kconfig() + 'CONFIG_SF_GYRO=y\n', features)

    def test_bad_config_cli_fails_before_west_or_build_directory(self):
        self.path.write_text('{"charging":"false","imu":true,"ppg":true}')
        build = self.root / 'build'
        result = subprocess.run([sys.executable, str(ROOT / 'tools/build.py'),
                                 '--features', str(self.path), '--build-dir', str(build),
                                 '--ncs', str(self.root / 'no-sdk')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('must be true or false', result.stderr)
        self.assertFalse(build.exists())


if __name__ == '__main__':
    unittest.main()
