#!/usr/bin/env python3
"""Exercise installer ownership and failure handling with tiny source archives."""
import hashlib
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

INSTALLER = Path(__file__).resolve().parents[1] / 'install.sh'

class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mustermark-install-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / 'app'
        self.archive = self.root / 'source.tar.gz'
        self.make_archive()

    def make_archive(self, broken=False, unsafe=False):
        cmake = '''cmake_minimum_required(VERSION 3.21)
project(installer_fixture NONE)
install(PROGRAMS mustermark DESTINATION bin)
install(FILES mustermark.desktop DESTINATION share/applications)
install(FILES mustermark.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION share/licenses/mustermark)
'''
        files = {'CMakeLists.txt': 'invalid(cmake' if broken else cmake,
                 'mustermark': '#!/bin/sh\necho "mustermark 0.2.0"\n',
                 'mustermark.desktop': '[Desktop Entry]\nType=Application\nName=Mustermark\nExec=mustermark %f\nIcon=mustermark\n',
                 'mustermark.svg': '<svg/>', 'LICENSE': 'fixture'}
        if unsafe:
            files['../escape'] = 'reject me'
        with tarfile.open(self.archive, 'w:gz') as tar:
            for name, text in files.items():
                data = text.encode()
                info = tarfile.TarInfo('mustermark-0.2.0/' + name)
                info.size = len(data)
                info.mode = 0o755 if name == 'mustermark' else 0o644
                tar.addfile(info, io.BytesIO(data))
        self.checksum = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def run_installer(self, *args, success=True):
        result = subprocess.run(['sh', str(INSTALLER), '--prefix', str(self.prefix), *args],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode == 0, success, result.stdout)
        return result.stdout

    def install(self, success=True, checksum=None):
        return self.run_installer('--archive', str(self.archive), '--sha256', checksum or self.checksum, success=success)

    def test_install_update_and_uninstall_preserve_user_files(self):
        self.install()
        current = self.prefix / 'lib/mustermark/current'
        first = current.resolve()
        self.install()
        self.assertNotEqual(first, current.resolve())
        self.assertIn(str(self.prefix / 'bin/mustermark'), (self.prefix / 'share/applications/mustermark.desktop').read_text())
        document = self.prefix / 'notes.md'
        document.write_text('Keep my notes')
        self.run_installer('--uninstall')
        self.assertFalse((self.prefix / 'bin/mustermark').is_symlink())
        self.assertFalse((self.prefix / 'lib/mustermark').exists())
        self.assertEqual(document.read_text(), 'Keep my notes')

    def test_failed_build_and_bad_checksum_preserve_active_install(self):
        self.install()
        before = (self.prefix / 'lib/mustermark/current').resolve()
        self.assertIn('checksum mismatch', self.install(success=False, checksum='0' * 64))
        self.make_archive(broken=True)
        self.install(success=False)
        self.assertEqual(before, (self.prefix / 'lib/mustermark/current').resolve())
        self.assertEqual(subprocess.check_output([str(self.prefix / 'bin/mustermark')], text=True).strip(), 'mustermark 0.2.0')

    def test_unmanaged_install_is_not_overwritten(self):
        binary = self.prefix / 'bin/mustermark'
        binary.parent.mkdir(parents=True)
        binary.write_text('manual install')
        self.assertIn('unmanaged file', self.install(success=False))
        self.assertEqual(binary.read_text(), 'manual install')

    def test_archive_traversal_and_concurrent_install_rejected(self):
        self.make_archive(unsafe=True)
        self.assertIn('Unsafe archive paths', self.install(success=False))
        lock = self.prefix / 'lib/.mustermark-install-lock'
        lock.mkdir()
        self.assertIn('Another installation', self.install(success=False))
        self.assertTrue(lock.exists())

if __name__ == '__main__':
    unittest.main()
