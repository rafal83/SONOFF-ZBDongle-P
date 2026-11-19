import unittest
from check_production_logs import firmware_revisions

class FirmwareLogTests(unittest.TestCase):
    def test_complete_revision_without_dates_or_truncation(self):
        lines=['[2026-11-18] Coordinator firmware version: '+
            ''''{"type":"zStack3x0","meta":{"revision":2026111801,"maintrel":1}}' ''',
            '[2026-11-17] Coordinator firmware version: '+
            ''''{"meta":{"revision":20261117}}' ''',
            'Other message: {"revision":20261114}']
        self.assertEqual(firmware_revisions(lines),['2026111801','20261117'])

    def test_ignores_dates_and_missing_revision(self):
        self.assertEqual(firmware_revisions(['[2026-11-18] Coordinator firmware version: unavailable 20261118']),[])

if __name__=='__main__': unittest.main()
