"""Fault-injected transactions only. Never connects to a phone."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("hotswap_test_target", ROOT / "tools/hotswap.py")
hotswap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hotswap)


def elf(payload):
    header=bytearray(64);header[:6]=b"\x7fELF\x02\x01";header[16:20]=b"\x03\x00\xb7\x00"
    return bytes(header)+payload


class Phone:
    base="/data/app/~~mock/com.unknownworlds.subnautica-mock/base.apk"
    directory=base.removesuffix("/base.apk")+"/lib/arm64"
    remote=directory+"/libmain.so"
    original=directory+"/libmain_orig.so"
    def __init__(self):
        self.old=elf(b"old libmain_orig.so\0")
        self.files={self.remote:self.old,self.original:elf(b"original")}
        self.running=False;self.calls=[];self.fail_stage=False;self.bad_hash=False
        self.fail_after_rename=False;self.fail_launch=False;self.fail_rollback=False
        self.fail_pidof=False
        self.renamed=False
    def failure(self,command):
        raise subprocess.CalledProcessError(1,command,output=b"")
    def adb(self,serial,*args,**kwargs):
        self.calls.append(("adb",args))
        if args[:3]==("shell","pm","path"):return ("package:"+self.base+"\n").encode()
        if len(args)==2 and args[0]=="shell" and args[1].startswith("pidof "):
            if self.fail_pidof:self.failure("mock transport failure")
            return b"42\n" if self.running else b"SNMP_NO_GAME_PROCESS\n"
        if args[:3]==("shell","cmd","package"):return b"com.unknownworlds.subnautica/com.unity3d.player.UnityPlayerActivity\n"
        if args[0]=="push":self.files[args[2]]=Path(args[1]).read_bytes();return b""
        if args[:3]==("shell","am","force-stop"):self.running=False;return b""
        if args[:3]==("shell","am","start"):
            self.running=not self.fail_launch
            return b"Error: launch failed" if self.fail_launch else b"Status: ok\n"
        raise AssertionError(args)
    def su(self,serial,command,**kwargs):
        self.calls.append(("su",command));result=b""
        for clause in command.split(" && "):
            words=shlex.split(clause);op=words[0]
            if op=="id":result=b"0\n"
            elif op=="test":
                if words[1]=="!":continue
                if words[-1] not in self.files or not self.files[words[-1]]:self.failure(command)
            elif op=="head":result=self.files[words[-1]][:20]
            elif op=="cat":result=self.files[words[-1]]
            elif op=="stat":result=b"1000 1000 755\n"
            elif op=="ls":result=b"u:object_r:apk_data_file:s0 " + self.remote.encode()
            elif op=="sha256sum":
                value=hashlib.sha256(self.files[words[1]]).hexdigest()
                if self.bad_hash and self.renamed and words[1]==self.remote:
                    value="0"*64;self.bad_hash=False
                result=(value+"  "+words[1]+"\n").encode()
            elif op=="cp":
                if self.fail_stage:self.files[words[2]]=b"partial";self.failure(command)
                self.files[words[2]]=self.files[words[1]]
            elif op in ("chmod","chown","chcon"):pass
            elif op=="ln":self.files[words[2]]=self.files[words[1]]
            elif op=="mv":
                source,target=words[-2:]
                if ".snmp-before-" in source and self.fail_rollback:self.failure(command)
                self.files[target]=self.files.pop(source)
                if ".snmp-pending-" in source:
                    self.renamed=True
                    if self.fail_after_rename:
                        self.fail_after_rename=False;self.failure(command)
            elif op=="rm":
                for path in words[2:]:self.files.pop(path,None)
            elif op=="tail":result=b"mock log\n"
            else:raise AssertionError(command)
        return result


class HotswapTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.library=self.root/"new.so"
        self.new=elf(b"new libmain_orig.so\0");self.library.write_bytes(self.new)
        self.phone=Phone();self.backup=self.root/"backup"
        for name,value in (("adb",self.phone.adb),("su",self.phone.su)):
            p=patch.object(hotswap,name,value);p.start();self.addCleanup(p.stop)
        p=patch.object(hotswap.time,"sleep",lambda seconds:None);p.start();self.addCleanup(p.stop)
    def swap(self,**kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            return hotswap.swap_library("MOCK",self.library,self.backup,restart=False,**kwargs)
    def phase(self):return json.loads((self.backup/"transaction.json").read_text())["phase"]
    def test_success_keeps_verified_backup_and_original(self):
        original=self.phone.files[self.phone.original];r=self.swap()
        self.assertEqual(self.phone.files[self.phone.remote],self.new)
        self.assertEqual((self.backup/"libmain.before.so").read_bytes(),self.phone.old)
        self.assertEqual(self.phone.files[self.phone.original],original)
        self.assertEqual(r["phase"],"complete")
        self.assertFalse(any(".snmp-" in name for name in self.phone.files))
    def test_stock_game_refused_before_stop_or_copy(self):
        self.phone.files.pop(self.phone.original)
        with self.assertRaises(subprocess.CalledProcessError):self.swap()
        self.assertEqual(self.phone.files[self.phone.remote],self.phone.old)
        self.assertFalse(self.backup.exists())
        self.assertFalse(any(c[0]=="adb" and "force-stop" in c[1] for c in self.phone.calls))
    def test_fake_original_does_not_make_stock_wrapper_eligible(self):
        self.phone.files[self.phone.remote]=elf(b"stock original")
        with self.assertRaisesRegex(RuntimeError,"not an SNMP"):self.swap()
        self.assertFalse(self.backup.exists())
    def test_running_game_requires_saved_acknowledgement(self):
        self.phone.running=True
        with self.assertRaisesRegex(RuntimeError,"game is running"):self.swap()
        self.assertTrue(self.phone.running)
        self.assertFalse(self.backup.exists())
    def test_running_game_can_update_after_saved_acknowledgement(self):
        self.phone.running=True;self.swap(saved_and_exited=True)
        self.assertEqual(self.phone.files[self.phone.remote],self.new)
    def test_adb_failure_is_not_treated_as_game_closed(self):
        self.phone.fail_pidof=True
        with self.assertRaises(subprocess.CalledProcessError):self.swap()
        self.assertFalse(self.backup.exists())
        self.assertEqual(self.phone.files[self.phone.remote],self.phone.old)
    def test_partial_stage_cannot_damage_current_library(self):
        self.phone.fail_stage=True
        with self.assertRaises(subprocess.CalledProcessError):self.swap()
        self.assertEqual(self.phone.files[self.phone.remote],self.phone.old)
        self.assertEqual(self.phase(),"aborted_before_replacement")
    def test_failed_verification_restores_previous_library(self):
        self.phone.bad_hash=True
        with self.assertRaisesRegex(RuntimeError,"installed library failed"):self.swap()
        self.assertEqual(self.phone.files[self.phone.remote],self.phone.old)
        self.assertEqual(self.phase(),"rolled_back")
    def test_uncertain_rename_result_still_rolls_back(self):
        self.phone.fail_after_rename=True
        with self.assertRaises(subprocess.CalledProcessError):self.swap()
        self.assertEqual(self.phone.files[self.phone.remote],self.phone.old)
        self.assertEqual(self.phase(),"rolled_back")
    def test_failed_rollback_retains_both_recovery_copies(self):
        self.phone.bad_hash=True;self.phone.fail_rollback=True
        with self.assertRaisesRegex(RuntimeError,"automatic rollback failed"):self.swap()
        report=json.loads((self.backup/"transaction.json").read_text())
        self.assertEqual(self.phone.files[report["remote_backup"]],self.phone.old)
        self.assertEqual((self.backup/"libmain.before.so").read_bytes(),self.phone.old)
    def test_failed_start_restores_previous_library(self):
        self.phone.fail_launch=True
        with self.assertRaisesRegex(RuntimeError,"did not confirm"):
            hotswap.swap_library("MOCK",self.library,self.backup,restart=True)
        self.assertEqual(self.phone.files[self.phone.remote],self.phone.old)
        self.assertEqual(self.phase(),"rolled_back")
    def test_non_arm64_payload_refused_without_phone_access(self):
        self.library.write_bytes(b"not an ELF")
        with self.assertRaisesRegex(RuntimeError,"ARM64"):self.swap()
        self.assertFalse(self.phone.calls)
    def test_unexpected_package_path_refused(self):
        self.phone.base="/data/app/../other/base.apk"
        with self.assertRaisesRegex(RuntimeError,"unexpected game APK path"):self.swap()
        self.assertFalse(self.backup.exists())


if __name__ == "__main__":unittest.main()
