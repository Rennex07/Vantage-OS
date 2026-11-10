"""Keep production ownership boundaries and the explicit source manifest intact."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROJECT_INCLUDE = re.compile(r'#include\s+[<"](vantage/[^>"]+)[>"]')


class LayoutTests(unittest.TestCase):
    def test_shared_abi_does_not_import_privileged_headers(self):
        headers = [ROOT / 'include/vantage/user.h', *sorted((ROOT / 'include/vantage/abi').glob('*.h'))]
        for header in headers:
            text = header.read_text(encoding='utf-8')
            self.assertNotRegex(text, r'#include\s+"', str(header.relative_to(ROOT)))
            for include in PROJECT_INCLUDE.findall(text):
                with self.subTest(header=header.name, include=include):
                    self.assertTrue(include == 'vantage/base.h' or include.startswith('vantage/abi/'))

    def test_user_apps_do_not_import_kernel_or_hardware_apis(self):
        for path in sorted((ROOT / 'user/apps').rglob('*')):
            if path.suffix not in ('.c', '.h'):
                continue
            text = path.read_text(encoding='utf-8')
            self.assertNotRegex(text, r'#include\s+"[^"\n]*[/\\]', str(path.relative_to(ROOT)))
            for include in PROJECT_INCLUDE.findall(text):
                with self.subTest(source=str(path.relative_to(ROOT)), include=include):
                    self.assertTrue(include in ('vantage/base.h', 'vantage/user.h') or
                                    include.startswith('vantage/abi/'))

    def test_project_includes_exist_and_use_the_canonical_namespace(self):
        for directory in ('include', 'arch', 'drivers', 'kernel', 'lib', 'platform', 'user', 'sim/platform', 'sim/tests', 'tests/native'):
            for path in (ROOT / directory).rglob('*'):
                if path.suffix not in ('.c', '.h'):
                    continue
                text = path.read_text(encoding='utf-8')
                with self.subTest(source=str(path.relative_to(ROOT))):
                    self.assertNotRegex(text, r'#include\s+"[^"\n]*\bincludes/')
                    for include in PROJECT_INCLUDE.findall(text):
                        self.assertTrue((ROOT / 'include' / include).is_file(), include)

    def test_kernel_manifest_covers_production_sources_exactly_once(self):
        text = (ROOT / 'mk/sources.mk').read_text(encoding='utf-8')
        manifest = re.findall(r'\b(?:arch|drivers|kernel|lib|platform|user)/[\w/]+\.(?:c|s)\b', text)
        actual = {str(path.relative_to(ROOT)).replace('\\', '/')
                  for directory in ('arch', 'drivers', 'kernel', 'lib', 'platform', 'user')
                  for path in (ROOT / directory).rglob('*') if path.suffix in ('.c', '.s')}
        self.assertEqual(len(manifest), len(set(manifest)), 'Duplicate production source')
        self.assertEqual(set(manifest), actual)
        self.assertNotIn('wildcard', text)


if __name__ == '__main__':
    unittest.main()
