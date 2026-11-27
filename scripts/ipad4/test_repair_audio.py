"""Exercise the production move on isolated directories. No device or firmware needed."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import ipad4


class RepairAudioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        root = Path(cls.work.name)
        main = root / "fixture.c"
        main.write_text("""
#define P101_AUDIO_FIXTURE
#include "repair_audio.c"
int main(int argc, char **argv) {
    /* FNV-1a and length of the one-byte fixture 'a'. */
    const struct fingerprint files[3] = {{1, 0xe40c292c}, {1, 0xe40c292c}, {1, 0xe40c292c}};
    enum audio_state state = repair(stderr, argc == 2 && !strcmp(argv[1], "apply"), files);
    if (state == FAILED) return 1;
    puts(state == RESTORED ? "restored" : "enabled");
    return 0;
}
""")
        cls.bin = root / ("fixture.exe" if os.name == "nt" else "fixture")
        cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang")
        subprocess.run([cc, "-std=gnu11", "-I", str(HERE), str(main), "-o", str(cls.bin)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.system = Path(tmp.name) / "SystemVolume"
        self.version = self.system / "System/Library/CoreServices/SystemVersion.plist"
        self.version.parent.mkdir(parents=True)
        self.version.write_bytes(b"a")
        base = self.system / "Library/Audio/Plug-Ins"
        self.off = base / "VirtualAudio.plugin"
        self.on = base / "HAL/VirtualAudio.plugin"
        self.on.parent.mkdir(parents=True)
        self.off.mkdir()
        (self.off / "VirtualAudio").write_bytes(b"a")
        (self.off / "Info.plist").write_bytes(b"a")

    def invoke(self, mode):
        return subprocess.run([self.bin, mode], cwd=self.system, text=True, capture_output=True)

    def test_restores_once_and_verifies(self):
        self.assertEqual(self.invoke("apply").stdout.strip(), "restored")
        self.assertFalse(self.off.exists())
        self.assertEqual((self.on / "VirtualAudio").read_bytes(), b"a")
        self.assertEqual(self.invoke("verify").stdout.strip(), "enabled")
        self.assertEqual(self.invoke("apply").stdout.strip(), "enabled")

    def test_readonly_verification_never_moves_a_bundle(self):
        self.assertNotEqual(self.invoke("verify").returncode, 0)
        self.assertTrue(self.off.exists())

    def test_conflicting_bundles_are_preserved(self):
        self.on.mkdir()
        self.assertNotEqual(self.invoke("apply").returncode, 0)
        self.assertTrue(self.off.is_dir())
        self.assertTrue(self.on.is_dir())

    def test_missing_bundles_fail(self):
        shutil.rmtree(self.off)
        self.assertNotEqual(self.invoke("apply").returncode, 0)
        self.assertFalse(self.on.exists())

    def test_unexpected_driver_or_system_refuses_to_move(self):
        for path in (self.off / "VirtualAudio", self.off / "Info.plist", self.version):
            with self.subTest(path=path):
                path.write_bytes(b"x")
                self.assertNotEqual(self.invoke("apply").returncode, 0)
                self.assertTrue(self.off.exists())
                self.assertFalse(self.on.exists())
                path.write_bytes(b"a")

    def test_simultaneous_host_boot_is_rejected(self):
        with ipad4.use_device(self.system):
            with self.assertRaises((ValueError, OSError)):
                with ipad4.use_device(self.system):
                    self.fail("The second caller obtained a lock")
        with ipad4.use_device(self.system):
            pass

    def test_host_rejects_stale_incomplete_or_failed_guest_log(self):
        args = SimpleNamespace(device=self.system, repair_boot=self.system / "repair.kboot",
                               qemu=self.bin, bios=self.system, timeout=30)
        args.repair_boot.touch()
        (self.system / "normal.kboot").touch()
        (self.system / "nand").mkdir()
        (self.system / "nand/geometry.json").touch()
        log = self.system / "repair-audio.log"
        for report in ("", "P101AUDIO OK restored\n", "pmu go stdby\nP101AUDIO OK already-enabled\n",
                       "P101AUDIO OK restored\nP101AUDIO FAILED\npmu go stdby\n"):
            with self.subTest(report=report):
                log.write_text("P101AUDIO OK restored\npmu go stdby\n")
                def fake_qemu(*args, **kwargs):
                    self.assertEqual(log.read_bytes(), b"")
                    log.write_text(report)
                with patch.object(ipad4.subprocess, "run", side_effect=fake_qemu):
                    with self.assertRaisesRegex(ValueError, "did not verify"):
                        ipad4.repair_audio(args)

    def test_interactive_boot_has_host_audio_and_maintenance_is_silent(self):
        options = SimpleNamespace(qemu=self.bin, bios=self.system, retina=True)
        for display, driver in (("sdl", "sdl"), ("cocoa", "coreaudio"), ("none", "none")):
            with self.subTest(display=display):
                args = ipad4.engine(options, self.system / "boot", self.system / "nand", display,
                                    self.system / "serial", restore_stream="system" if display == "none" else None)
                self.assertEqual(args[args.index("-audio") + 1], "driver=" + driver)
                machine = args[args.index("-M") + 1].split(",")
                for flag in ("audio-ports-probe=on", "ae2-probe=on", "tristar-probe=on"):
                    self.assertEqual(flag in machine, display != "none")


if __name__ == "__main__":
    unittest.main()
