"""Exercise installation layouts and failures without requiring a compiler."""

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "clice_install", Path(__file__).parents[1] / "install.py"
)
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "build tree"
        self.prefix = self.root / "install prefix"
        (self.source / "bin").mkdir(parents=True)
        (self.source / "bin/clice").write_bytes(b"unstripped executable")
        self.include = self.source / "lib/clang/23/include"
        self.include.mkdir(parents=True)
        for name in ("stddef.h", "stdarg.h", "__stddef_size_t.h"):
            (self.include / name).write_text(f"/* {name} */\n")

    def test_installs_independent_files_and_replaces_running_binary_path(self):
        external = self.root / "bazel cache"
        external.mkdir()
        (external / "header.h").write_text("/* builtin */\n")
        (self.include / "nested").symlink_to(external, target_is_directory=True)
        stripped = self.source / "bin/clice.stripped"
        stripped.write_bytes(b"stripped executable")
        stripped.chmod(0o444)
        (self.prefix / "bin").mkdir(parents=True)
        installed = self.prefix / "bin/clice"
        installed.write_bytes(b"old executable")
        with installed.open("rb") as old:
            installer.install(self.source, self.prefix)
            self.assertEqual(old.read(), b"old executable")
        self.assertEqual(installed.read_bytes(), b"stripped executable")
        self.assertEqual(installed.stat().st_mode & 0o777, 0o755)
        copied = self.prefix / "lib/clang/23/include/nested/header.h"
        (external / "header.h").unlink()
        self.assertEqual(copied.read_text(), "/* builtin */\n")
        self.assertFalse(any(p.is_symlink() for p in self.prefix.rglob("*")))

    def test_missing_header_leaves_installation_untouched(self):
        (self.include / "stddef.h").unlink()
        with self.assertRaisesRegex(ValueError, "Missing Clang resource header"):
            installer.install(self.source, self.prefix)
        self.assertFalse(self.prefix.exists())

    def test_dangling_resource_link_preserves_old_installation(self):
        (self.include / "broken.h").symlink_to(self.root / "missing")
        (self.prefix / "bin").mkdir(parents=True)
        installed = self.prefix / "bin/clice"
        installed.write_bytes(b"old executable")
        with self.assertRaises(shutil.Error):
            installer.install(self.source, self.prefix)
        self.assertEqual(installed.read_bytes(), b"old executable")
        self.assertFalse((self.prefix / "lib").exists())
        self.assertFalse(list(self.prefix.glob(".clice-install-*")))

    def test_preserves_runtime_files_and_other_resource_versions(self):
        runtime = self.prefix / "lib/clang/23/lib/linux/runtime.a"
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes(b"runtime")
        other = self.prefix / "lib/clang/22/include/stddef.h"
        other.parent.mkdir(parents=True)
        other.write_text("old version")
        installer.install(self.source, self.prefix)
        self.assertEqual(runtime.read_bytes(), b"runtime")
        self.assertEqual(other.read_text(), "old version")

    def test_rejects_installing_over_the_input(self):
        with self.assertRaisesRegex(ValueError, "outside the source tree"):
            installer.install(self.source, self.source)


if __name__ == "__main__":
    unittest.main()
