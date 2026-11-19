from pathlib import Path
import unittest

import yaml


class WorkflowTests(unittest.TestCase):
    def test_workflows_are_yaml_and_shell_commands_are_scalars(self):
        folder = Path(__file__).resolve().parents[1] / '.github/workflows'
        workflows = sorted(folder.glob('*.yml'))
        self.assertTrue(workflows)
        for path in workflows:
            with self.subTest(workflow=path.name):
                # BaseLoader keeps the GitHub event key "on" as a string.
                data = yaml.load(path.read_text(encoding='utf-8'), Loader=yaml.BaseLoader)
                self.assertIn('on', data)
                for job in data['jobs'].values():
                    for step in job.get('steps', []):
                        if 'run' in step:
                            self.assertIsInstance(step['run'], str)


if __name__ == '__main__':
    unittest.main()
