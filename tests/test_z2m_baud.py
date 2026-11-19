from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from set_z2m_baud import change_baud

class BaudEditTests(unittest.TestCase):
    def test_replaces_only_scalar_preserving_comments_and_line_endings(self):
        original = b"serial:\r\n  port: /dev/serial/test\r\n  baudrate: 115200 # comment\r\nadvanced:\r\n  channel: 25\r\n"
        self.assertEqual(change_baud(original, 1000000), original.replace(b"115200", b"1000000"))
    def test_inserts_explicit_rate_without_reformatting(self):
        original = b"# untouched\nserial:\n  port: /dev/serial/test\n  adapter: zstack\nadvanced:\n  pan_id: 1234\n"
        self.assertEqual(change_baud(original,1000000), original.replace(b"serial:\n",b"serial:\n  baudrate: 1000000\n"))
    def test_rejects_ambiguous_or_unsupported_yaml(self):
        for raw in [b"serial: {baudrate: 115200}", b"serial:\n  baudrate: 115200\n  baudrate: 460800\n",
                    b"serial:\n  port: a\nserial:\n  port: b\n"]:
            with self.assertRaises(ValueError): change_baud(raw,1000000)

if __name__ == "__main__": unittest.main()
