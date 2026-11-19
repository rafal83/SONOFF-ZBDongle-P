from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from audit_distribution import check_source, audit_archive
from download_ti_toolchain import device_support_source


class DistributionAuditTests(unittest.TestCase):
    def test_rejects_sensitive_and_retired_files_but_allows_synthetic_fixture(self):
        for name in ('private/backup.bin', 'vendor-sdk/lib.a', 'firmware/copy.hex',
                     'tools/decompile_cc2652p.py', 'tools/ghidra/analysis.java',
                     'data/coordinator_backup.json', 'data/private.pem'):
            with self.subTest(name=name):
                self.assertTrue(check_source(name, b''))
        self.assertEqual(check_source('tests/fixtures/freertos_store_sim.bin', b'fixture'), [])
        self.assertEqual(check_source('firmware/freertos/app/main.c', b'/* SPDX-License-Identifier: MIT */'), [])

    def test_detects_credentials_without_returning_their_values(self):
        values = ['ghp_' + 'a' * 40, 'github_pat_' + 'a' * 70,
                  '-----BEGIN ' + 'PRIVATE KEY-----', 'AKIA' + 'A' * 16]
        for value in values:
            findings = check_source('source.txt', value.encode())
            self.assertTrue(findings)
            self.assertNotIn(value, '\n'.join(findings))

    def test_minimal_sdk_selection_excludes_stacks_and_binary_libraries(self):
        sdk = 'simplelink_cc13xx_cc26xx_sdk_8_33_00_16'
        for relative in ('source/ti/devices/DeviceFamily.h',
                         'source/ti/devices/cc13x2_cc26x2/driverlib/setup.c',
                         'source/ti/devices/cc13x2_cc26x2/rf_patches/rf_patch_cpe_ieee_802_15_4.c',
                         f'license_{sdk}.txt', f'manifest_{sdk}.html'):
            self.assertTrue(device_support_source(sdk + '/' + relative, sdk), relative)
        for relative in ('source/ti/zstack/lib/libZStack_nwk_all.a',
                         'source/ti/devices/cc13x2_cc26x2/driverlib/bin/driverlib.lib',
                         'source/ti/devices/cc13x2_cc26x2/../../zstack/file.c',
                         'source/third_party/ecc/lib.a'):
            self.assertFalse(device_support_source(sdk + '/' + relative, sdk), relative)

    def test_release_requires_notices_and_rejects_private_members_and_path_traversal(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'release.zip'
            with zipfile.ZipFile(path, 'w') as z:
                z.writestr('../escape.txt', 'bad path')
                z.writestr('sonoff-freertos/private/backup.bin', b'private')
                z.writestr('sonoff-freertos/docs/leaked.txt', 'ghp_' + 'a' * 40)
            findings = audit_archive(path)['findings']
            self.assertTrue(any('licence missing' in item for item in findings))
            self.assertTrue(any('unsafe release path' in item for item in findings))
            self.assertTrue(any('unexpected release member' in item for item in findings))
            self.assertTrue(any('GitHub token' in item for item in findings))
