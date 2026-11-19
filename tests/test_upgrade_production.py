import unittest
from test_production_migration import image
from upgrade_production import ROOT, PROFILES, NV_START, NV_END, load_profile, upgrade_target

class UpgradeTests(unittest.TestCase):
    def test_upgrade_and_rollback_keep_current_counters(self):
        old, new = image(1), image(2); new[0x200] = 0x77
        target = upgrade_target(old, image(3), new)
        self.assertEqual(target[NV_START:], old[NV_START:])
        self.assertEqual(target[0x200], 0x77)
        advanced = bytearray(target); advanced[NV_START:NV_END] = b'\x44' * (NV_END-NV_START)
        restored = upgrade_target(advanced, new, old)
        self.assertEqual(restored[NV_START:], advanced[NV_START:])
        self.assertEqual(restored[0x200], old[0x200])
    def test_refuses_unknown_current_application(self):
        wrong = image(1); wrong[0x200] = 0x33
        with self.assertRaises(ValueError): upgrade_target(wrong, image(2), image(3))
    def check_pinned_image(self, revision):
        folder = ROOT / 'build' / PROFILES[revision][0]
        if not folder.exists():
            self.skipTest(f'Build firmware {revision} first')
        # A partial or changed build must still fail verification.
        _, manifest = load_profile(revision)
        self.assertEqual(manifest['znp_version']['revision'], revision)

    def test_pinned_20261114_passes_recovery_checks_when_built(self):
        self.check_pinned_image(20261114)

    def test_pinned_20261117_passes_recovery_checks_when_built(self):
        self.check_pinned_image(20261117)

if __name__ == '__main__': unittest.main()
