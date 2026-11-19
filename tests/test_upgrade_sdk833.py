import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from test_production_migration import image
import upgrade_sdk833 as trial

class SDK833TrialTests(unittest.TestCase):
    def exercise(self, action='install', wrong_program=False, fail_after_boot=False, candidate_revision=trial.CANDIDATE, source_revision=trial.STABLE):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); folder=root/'private/session'; folder.mkdir(parents=True)
            stable,candidate=image(1),image(2); candidate[0x200]=0x77
            fixed=image(3); fixed[0x200]=0x88
            profiles={trial.STABLE:stable,trial.CANDIDATE:candidate,2026111802:fixed}
            current=bytearray(profiles[candidate_revision] if action=='rollback' else profiles[source_revision])
            if wrong_program: current[0x201]=0x11
            folder.joinpath('z2m-snapshot.json').write_text(json.dumps(dict(ieee='0x1234',configured_baudrate=1000000,sha256={})))
            folder.joinpath('network-before.json.report.json').write_text(json.dumps(dict(passed=True,revision=source_revision,database_devices_absent_from_backup=0)))
            if action=='rollback':
                (folder/'install').mkdir()
                (folder/'install/report.json').write_text(json.dumps(dict(passed=True,destination_hex_sha256=trial.PROFILES[trial.CANDIDATE][1])))
            writes=[]; revisions=[]; closes=[]
            class Command:
                def close(self): closes.append(True)
            def snapshot(args,folder,output,revision,baud,compare):
                revisions.append(revision)
                if revision==candidate_revision and fail_after_boot:
                    current[trial.NV_START:trial.NV_END]=b'\x33'*(trial.NV_END-trial.NV_START)
                    raise IOError('Injected post-boot verification failure')
                return dict(passed=True,security_counters_non_decreasing=True)
            def write(command,device,target,before,folder,prefix):
                self.assertEqual(target[trial.NV_START:],before[trial.NV_START:])
                writes.append((prefix,bytes(target)))
                current[:]=target
                return dict(full_readback_verified=True,sha256=trial.sha(target))
            def profile(revision): return profiles[revision],{}
            argv=['upgrade_sdk833.py',action,'--port','MOCK','--usb-serial','MOCK','--expect-ieee','0x1234',
                '--session-dir',str(folder),'--z2m-stopped','--candidate-revision',str(candidate_revision),'--source-revision',str(source_revision)]
            with patch.object(trial,'ROOT',root),patch.object(trial,'load_profile',side_effect=profile),\
                 patch.object(trial,'qualify_candidate') as qualify,patch.object(trial,'network_snapshot',side_effect=snapshot),\
                 patch.object(trial,'open_rom',return_value=(Command(),object())),\
                 patch.object(trial,'capture_twice',side_effect=lambda *args:bytes(current)),\
                 patch.object(trial,'write_verified',side_effect=write),patch.object(trial,'boot'),patch.object(sys,'argv',argv),\
                 patch('builtins.print'):
                if wrong_program or fail_after_boot:
                    with self.assertRaises((ValueError,IOError)): trial.main()
                else: trial.main()
                self.assertEqual(qualify.call_count,0 if action=='rollback' else 1)
            report=json.loads((folder/action/'report.json').read_text())
            return writes,revisions,report,stable,candidate

    def test_unknown_application_never_reaches_erase(self):
        writes,_,report,_,_=self.exercise(wrong_program=True)
        self.assertEqual(writes,[])
        self.assertFalse(report['passed'])
        self.assertNotIn('automatic_20261117_recovery',report)

    def test_failed_candidate_recovers_stable_with_advanced_current_NV(self):
        writes,revisions,report,stable,_=self.exercise(fail_after_boot=True)
        self.assertEqual(revisions,[trial.STABLE,trial.CANDIDATE,trial.STABLE])
        self.assertEqual([p for p,_ in writes],['target','recovery'])
        recovered=writes[-1][1]
        self.assertEqual(recovered[:trial.NV_START],stable[:trial.NV_START])
        self.assertEqual(recovered[trial.NV_START:trial.NV_END],b'\x33'*(trial.NV_END-trial.NV_START))
        self.assertTrue(report['automatic_20261117_recovery']['full_readback_verified'])

    def test_explicit_rollback_preserves_NV_without_requalifying_old_sources(self):
        writes,_,report,stable,candidate=self.exercise(action='rollback')
        self.assertEqual(len(writes),1)
        self.assertEqual(writes[0][1][:trial.NV_START],stable[:trial.NV_START])
        self.assertEqual(writes[0][1][trial.NV_START:trial.NV_END],candidate[trial.NV_START:trial.NV_END])
        self.assertTrue(report['passed'])

    def test_failed_fix_recovers_20261117_instead_of_faulty_source_candidate(self):
        writes,revisions,report,stable,_=self.exercise(fail_after_boot=True,candidate_revision=2026111802,source_revision=2026111801)
        self.assertEqual(revisions,[2026111801,2026111802,20261117])
        self.assertEqual(writes[-1][1][:trial.NV_START],stable[:trial.NV_START])
        self.assertEqual(writes[-1][1][trial.NV_START:trial.NV_END],b'\x33'*(trial.NV_END-trial.NV_START))
        self.assertTrue(report['automatic_20261117_recovery']['full_readback_verified'])

if __name__=='__main__': unittest.main()
