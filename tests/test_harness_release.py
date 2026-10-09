"""Release safeguards using isolated checkouts and real AUR metadata generation."""

import argparse
from contextlib import redirect_stdout
import hashlib
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest
from unittest.mock import patch


REPOSITORY = Path(__file__).resolve().parent.parent
LOADER = importlib.machinery.SourceFileLoader("omascribe_harness", str(REPOSITORY / "bin" / "harness"))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
HARNESS_MODULE = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(HARNESS_MODULE)


class ReleaseChecks(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="omascribe-release-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.files = {}
        for name in ("src/appversion.h", "pkgbuild/PKGBUILD", "pkgbuild/omascribe.install", "README.md"):
            content = (REPOSITORY / name).read_bytes()
            self.files[name] = content
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        root_patch = patch.object(HARNESS_MODULE, "ROOT", self.root)
        output_patch = patch.object(HARNESS_MODULE, "HARNESS", self.root / ".harness")
        root_patch.start()
        output_patch.start()
        self.addCleanup(root_patch.stop)
        self.addCleanup(output_patch.stop)
        self.version = HARNESS_MODULE.release_metadata()["version"]

    def archive(self, overrides=None, symlink=None):
        files = {**self.files, **(overrides or {})}
        archive = self.root / "source.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            for name, content in files.items():
                member = tarfile.TarInfo(f"omascribe-{self.version}/{name}")
                if name == symlink:
                    member.type = tarfile.SYMTYPE
                    member.linkname = "/etc/passwd"
                    output.addfile(member)
                else:
                    member.size = len(content)
                    output.addfile(member, io.BytesIO(content))
        return archive

    def prepare(self, archive):
        with redirect_stdout(io.StringIO()):
            return HARNESS_MODULE.prepare_aur(argparse.Namespace(archive=str(archive)))

    def test_application_and_package_versions_must_match(self):
        recipe = self.root / "pkgbuild/PKGBUILD"
        recipe.write_text(recipe.read_text().replace(f"pkgver={self.version}", "pkgver=999.0.0"))
        with self.assertRaisesRegex(RuntimeError, "disagree"):
            HARNESS_MODULE.release_metadata()

    def test_readme_must_identify_release(self):
        (self.root / "README.md").write_text("Omascribe\n")
        with self.assertRaisesRegex(RuntimeError, "README"):
            HARNESS_MODULE.release_metadata()

    def test_archive_with_other_application_version_is_rejected(self):
        archive = self.archive({"src/appversion.h": b'inline constexpr char version[] = "999.0.0";\n'})
        with self.assertRaisesRegex(RuntimeError, "application version"):
            self.prepare(archive)
        self.assertFalse((self.root / ".harness").exists())

    def test_archive_metadata_cannot_be_a_symlink(self):
        with self.assertRaisesRegex(RuntimeError, "Invalid release archive member"):
            self.prepare(self.archive(symlink="pkgbuild/PKGBUILD"))
        self.assertFalse((self.root / ".harness").exists())

    def test_changed_recipe_requires_packaging_revision(self):
        archive = self.archive()
        recipe = self.root / "pkgbuild/PKGBUILD"
        recipe.write_text(recipe.read_text().replace("options=('!debug')", "options=('!debug' '!strip')"))
        with self.assertRaisesRegex(RuntimeError, "pkgrel bump"):
            self.prepare(archive)

    @unittest.skipUnless(shutil.which("makepkg"), "Arch makepkg is needed for real AUR metadata")
    def test_standalone_recipe_uses_archive_checksum_and_reports_unpublished(self):
        archive = self.archive()
        self.assertEqual(self.prepare(archive), 0)
        report_path = next((self.root / ".harness/releases").glob("*/report.json"))
        report = json.loads(report_path.read_text())
        aur = Path(report["aurDirectory"])
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual(report["sha256"], digest)
        self.assertFalse(report["aurPublished"])
        self.assertEqual(report["publishFiles"], ["PKGBUILD", ".SRCINFO", "omascribe.install"])
        recipe = (aur / "PKGBUILD").read_text()
        self.assertIn(digest, recipe)
        self.assertIn('$srcdir/$pkgname-$pkgver', recipe)
        self.assertNotIn("$startdir/..", recipe)
        self.assertIn(f"pkgver = {self.version}", (aur / ".SRCINFO").read_text())
        self.assertEqual((aur / "omascribe.install").read_bytes(), self.files["pkgbuild/omascribe.install"])

    @unittest.skipUnless(shutil.which("makepkg"), "Arch makepkg is needed for real AUR metadata")
    def test_packaging_fix_keeps_published_source_and_uses_updated_install_script(self):
        archive = self.archive()
        metadata = HARNESS_MODULE.release_metadata()
        revision = int(metadata["pkgrel"]) + 1
        recipe = self.root / "pkgbuild/PKGBUILD"
        recipe.write_text(recipe.read_text().replace(f'pkgrel={metadata["pkgrel"]}', f"pkgrel={revision}"))
        install = self.root / "pkgbuild/omascribe.install"
        install.write_bytes(install.read_bytes() + b"\n# Packaging fix\n")
        self.assertEqual(self.prepare(archive), 0)
        report = json.loads(next((self.root / ".harness/releases").glob("*/report.json")).read_text())
        aur = Path(report["aurDirectory"])
        self.assertEqual(report["pkgrel"], str(revision))
        self.assertEqual((aur / "omascribe.install").read_bytes(), install.read_bytes())
        self.assertIn(f"pkgrel = {revision}", (aur / ".SRCINFO").read_text())
        self.assertEqual(report["sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main(verbosity=2)
