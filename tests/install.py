"""End-to-end tests for install.sh's systemd drop-in handling.

Everything the script would touch stays inside a TemporaryDirectory: fake sudo,
cmake, systemctl and getent come first on PATH, so no real system directory,
privileged command or service is involved. The fake sudo maps every
/etc/systemd/system path into the sandbox, which is what lets the unmodified
production script run against it.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

INSTALL_SH = Path(__file__).resolve().parent.parent / 'install.sh'

FAKE_SUDO = """#!@PYTHON@
import os
import subprocess
import sys

SANDBOX = os.environ['BALL_TEST_SANDBOX']
TEST_ROOT = os.environ['BALL_TEST_ROOT']
STUBS = {'cmake', 'systemctl'}
LOCAL = ('test', 'grep', 'install', 'rm', 'rmdir')
ALLOWED = LOCAL + tuple(STUBS)
ROOT = '/etc/systemd/system'

args = sys.argv[1:]
if not args or args[0] not in ALLOWED:
    print('fake sudo refuses: ' + ' '.join(args), file=sys.stderr)
    sys.exit(64)
# The system paths the script uses live in the sandbox; nothing else moves.
args = ['%s%s' % (SANDBOX, a) if a == ROOT or a.startswith(ROOT + '/') else a
        for a in args]
# Every absolute path operand must stay inside the test root; anything else (a
# grep pattern, a mode, a relative operand) is left to the local command.
for arg in args[1:]:
    if arg.startswith('/') and not os.path.realpath(arg).startswith(os.path.realpath(TEST_ROOT) + '/'):
        print('fake sudo refuses path: ' + arg, file=sys.stderr)
        sys.exit(64)
if args[0] == 'install' and os.environ.get('BALL_TEST_FAIL_INSTALL'):
    sys.exit(1)
exe = os.path.join(SANDBOX, 'bin', args[0]) if args[0] in STUBS else '/usr/bin/' + args[0]
if not os.path.isfile(exe):
    print('fake sudo: not found: ' + exe, file=sys.stderr)
    sys.exit(64)
sys.exit(subprocess.call([exe] + args[1:]))
"""

FAKE_SYSTEMCTL = """#!/bin/sh
# Records invocations; the tests read the log instead of running systemd.
printf '%s\\n' "$*" >> "$BALL_TEST_SYSTEMCTL_LOG"
"""

FAKE_GETENT = """#!/bin/sh
[ "$1 $2" = 'group alice' ] && { echo 'alice:x:1000:'; exit 0; }
exit 2
"""

FAKE_CMAKE = '''#!@PYTHON@
import json
import os
import sys
from pathlib import Path

TEMPLATE = """[Unit]
Description=ball

[Service]
ExecStart="{prefix}/bin/ball" --daemon --socket /run/ball-%i/gui.sock --socktrail-bin "{prefix}/libexec/ball/socktrail"

[Install]
WantedBy=multi-user.target
"""

args = sys.argv[1:]
with open(os.environ['BALL_TEST_CMAKE_LOG'], 'a') as log:
    log.write(json.dumps(args) + '\\n')

if args and args[0] == '--install':
    prefix = Path(args[args.index('--prefix') + 1])
    unit = prefix / 'lib/systemd/system/ball@.service'
    unit.parent.mkdir(parents=True, exist_ok=True)
    unit.write_text(TEMPLATE.format(prefix=prefix))
    (prefix / 'bin').mkdir(parents=True, exist_ok=True)
    (prefix / 'bin/ball').write_text('')
'''


class InstallScriptTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.sandbox = self.root / 'sandbox'
        self.bin = self.sandbox / 'bin'
        self.bin.mkdir(parents=True)
        # mktemp in write_dropin must stay inside the test root too.
        self.tmp = self.root / 'tmp'
        self.tmp.mkdir()
        self._script('sudo', FAKE_SUDO.replace('@PYTHON@', sys.executable))
        self._script('systemctl', FAKE_SYSTEMCTL)
        self._script('getent', FAKE_GETENT)
        self._script('cmake', FAKE_CMAKE.replace('@PYTHON@', sys.executable))
        self.home = self.root / 'home'
        self.home.mkdir()
        self.prefix = self.root / 'prefix'
        self.build = self.root / 'build'
        self.build.mkdir()
        (self.build / 'cmake_install.cmake').write_text('# stub\n')
        self.systemctl_log = self.sandbox / 'systemctl.log'
        self.cmake_log = self.sandbox / 'cmake.log'

    def tearDown(self):
        self._tmp.cleanup()

    def _script(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)

    def env(self):
        return dict(
            os.environ,
            PATH=f'{self.bin}:{os.environ["PATH"]}',
            HOME=str(self.home),
            TMPDIR=str(self.tmp),
            BALL_TEST_ROOT=str(self.root),
            BALL_TEST_SANDBOX=str(self.sandbox),
            BALL_TEST_SYSTEMCTL_LOG=str(self.systemctl_log),
            BALL_TEST_CMAKE_LOG=str(self.cmake_log),
        )

    def run_install(self, *args):
        # Invoked through sh so the test does not depend on the exec bit.
        return subprocess.run(
            ['sh', str(INSTALL_SH), *args],
            env=self.env(),
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def install(self, *extra, prefix=None, user='alice'):
        return self.run_install(
            '--no-build',
            '--build-dir',
            str(self.build),
            '--prefix',
            str(prefix if prefix else self.prefix),
            '--no-enable',
            '--no-gui-autostart',
            '--user',
            user,
            *extra,
        )

    def uninstall(self, prefix=None, user='alice'):
        return self.run_install(
            '--uninstall',
            '--prefix',
            str(prefix if prefix else self.prefix),
            '--user',
            user,
        )

    def _install_ok(self, *extra, **kwargs):
        # Setup steps assert their own success, so a later failure is not blamed
        # on the step under test.
        proc = self.install(*extra, **kwargs)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def dropin(self, user='alice'):
        return self.sandbox / f'etc/systemd/system/ball@{user}.service.d/collector.conf'

    def dropin_dir(self, user='alice'):
        return self.dropin(user).parent

    def unit_file(self, prefix=None):
        return (prefix if prefix else self.prefix) / 'lib/systemd/system/ball@.service'

    def test_external_writes_dropin_and_keeps_template(self):
        proc = self.install('--collector-socket', '/run/socktrail/collector.sock')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(
            self.dropin().read_text().splitlines(),
            [
                '# X-Ball-Managed=true',
                '[Service]',
                'ExecStart=',
                (
                    f'ExecStart="{self.prefix}/bin/ball" --daemon --socket /run/ball-%i/gui.sock '
                    '--collector-socket "/run/socktrail/collector.sock"'
                ),
            ],
        )
        self.assertIn('--socktrail-bin', self.unit_file().read_text())

    def test_external_rerun_updates_dropin(self):
        self._install_ok('--collector-socket', '/run/first.sock')
        self._install_ok('--collector-socket', '/run/second.sock')
        lines = self.dropin().read_text().splitlines()
        self.assertEqual(len(lines), 4)
        self.assertTrue(
            lines[3].endswith('--collector-socket "/run/second.sock"'), lines
        )

    def test_reinstalling_template_keeps_dropin(self):
        self._install_ok('--collector-socket', '/run/socktrail/collector.sock')
        before = self.dropin().read_text()
        subprocess.run(
            [
                str(self.bin / 'cmake'),
                '--install',
                str(self.build),
                '--prefix',
                str(self.prefix),
            ],
            env=self.env(),
            check=True,
            timeout=15,
        )
        self.assertEqual(self.dropin().read_text(), before)
        self.assertIn('--socktrail-bin', self.unit_file().read_text())

    def test_spawn_removes_own_dropin_and_keeps_others(self):
        self._install_ok('--collector-socket', '/run/first.sock')
        other = self.dropin_dir() / 'custom.conf'
        other.write_text('[Service]\nEnvironment=BALL_CUSTOM=1\n')
        proc = self.install()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(self.dropin().exists())
        self.assertTrue(other.exists())

    def test_spawn_refuses_foreign_dropin(self):
        self.dropin_dir().mkdir(parents=True)
        self.dropin().write_text('[Service]\nExecStart=/bin/true\n')
        for extra in ((), ('--collector-socket', '/run/x.sock')):
            proc = self.install(*extra)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertEqual(
                self.dropin().read_text(), '[Service]\nExecStart=/bin/true\n'
            )
            # The refusal happens before anything is installed.
            self.assertFalse(self.unit_file().exists())

    def test_refuses_symlinked_dropin(self):
        target = self.sandbox / 'elsewhere.conf'
        target.write_text('# X-Ball-Managed=true\n[Service]\nExecStart=/bin/true\n')
        self.dropin_dir().mkdir(parents=True)
        self.dropin().symlink_to(target)
        proc = self.install('--collector-socket', '/run/x.sock')
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertTrue(self.dropin().is_symlink())

    def test_uninstall_removes_managed_dropin_only(self):
        self._install_ok('--collector-socket', '/run/first.sock')
        self.assertEqual(self.uninstall().returncode, 0)
        self.assertFalse(self.dropin().exists())
        self.assertFalse(self.dropin_dir().exists())

    def test_uninstall_keeps_custom_dropin(self):
        self.dropin_dir().mkdir(parents=True)
        self.dropin().write_text('[Service]\nExecStart=/bin/true\n')
        proc = self.uninstall()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(self.dropin().exists())
        self.assertIn('kept', proc.stderr)

    def test_rejects_invalid_arguments(self):
        for extra in (
            ('--user', 'ali ce'),
            ('--user', 'a/b'),
            ('--user', ''),
            ('--prefix', 'relative/prefix'),
            ('--collector-socket', 'run/x.sock'),
            ('--collector-socket', '/run/x\n.sock'),
        ):
            proc = self.install(*extra)
            self.assertEqual(proc.returncode, 2, (extra, proc.stderr))
        self.assertFalse(self.dropin().exists())
        self.assertFalse(self.unit_file().exists())

    def test_escapes_dynamic_unit_arguments(self):
        prefix = self.root / 'pre fix' / 'ball'
        sock = '/run/a b"c\\d$e%f.sock'
        proc = self.install('--collector-socket', sock, prefix=prefix)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(
            self.dropin().read_text().splitlines(),
            [
                '# X-Ball-Managed=true',
                '[Service]',
                'ExecStart=',
                (
                    f'ExecStart="{prefix}/bin/ball" --daemon --socket /run/ball-%i/gui.sock '
                    f'--collector-socket "/run/a b\\"c\\\\d$$e%%f.sock"'
                ),
            ],
        )

    def test_build_args_keep_prefix_with_spaces(self):
        prefix = self.root / 'pre fix' / 'ball'
        proc = self.run_install(
            '--build-dir',
            str(self.build),
            '--prefix',
            str(prefix),
            '--no-enable',
            '--no-gui-autostart',
            '--user',
            'alice',
            '--collector-socket',
            '/run/x.sock',
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        configure = json.loads(self.cmake_log.read_text().splitlines()[0])
        self.assertIn(f'-DCMAKE_INSTALL_PREFIX={prefix}', configure)

    def test_enable_records_enable_now(self):
        proc = self.run_install(
            '--no-build',
            '--build-dir',
            str(self.build),
            '--prefix',
            str(self.prefix),
            '--no-gui-autostart',
            '--user',
            'alice',
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('enable --now ball@alice.service', self.systemctl_log.read_text())

    def test_failed_dropin_install_cleans_temporary_file(self):
        env = dict(self.env(), BALL_TEST_FAIL_INSTALL='1')
        proc = subprocess.run(
            [
                'sh',
                str(INSTALL_SH),
                '--no-build',
                '--build-dir',
                str(self.build),
                '--prefix',
                str(self.prefix),
                '--no-enable',
                '--no-gui-autostart',
                '--user',
                'alice',
                '--collector-socket',
                '/run/x.sock',
            ],
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(list(self.tmp.iterdir()), [])
        self.assertFalse(self.dropin().exists())

    def test_uninstall_preserves_unrelated_dropin(self):
        self._install_ok('--collector-socket', '/run/first.sock')
        other = self.dropin_dir() / 'custom.conf'
        other.write_text('[Service]\nEnvironment=BALL_CUSTOM=1\n')
        proc = self.uninstall()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertFalse(self.dropin().exists())
        self.assertTrue(other.exists())

    def test_no_enable_does_not_enable(self):
        self._install_ok()
        log = self.systemctl_log.read_text()
        self.assertIn('daemon-reload', log)
        self.assertNotIn('enable', log)


if __name__ == '__main__':
    unittest.main()
