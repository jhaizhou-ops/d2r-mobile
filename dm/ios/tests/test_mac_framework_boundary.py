"""Check the host-Makefile boundary used to generate iOS build inputs."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


GENERATOR = Path(__file__).resolve().parents[1] / 'tools/gen_build.py'


class MacFrameworkBoundary(unittest.TestCase):
    def generate(self):
        temporary = tempfile.TemporaryDirectory(prefix='mac-framework-boundary-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        host = root / 'host'
        host.mkdir()
        sources = ('main.c', 'front_mac.m', 'touch_mac.c', 'dm_controls_mac.m', 'shared.c')
        for name in sources:
            (host / name).write_text('', encoding='utf-8')
        (host / 'Makefile').write_text(
            'dmhost:\n\tclang -I. ' + ' '.join(sources) +
            ' -framework Cocoa -framework Carbon -framework UIKit'
            ' -framework GameController -framework Metal\n', encoding='utf-8')
        generated = root / 'Gen'
        result = subprocess.run([sys.executable, str(GENERATOR), str(host), str(generated)],
                                capture_output=True, text=True, check=True)
        return generated, result.stdout

    def test_only_shared_frameworks_reach_ios(self):
        _, output = self.generate()
        frameworks = next(line for line in output.splitlines()
                          if line.startswith('DM_FRAMEWORKS = '))
        self.assertEqual(frameworks, 'DM_FRAMEWORKS = -framework UIKit '
                         '-framework GameController -framework Metal')

    def test_mac_sources_are_excluded_without_dropping_shared_sources(self):
        generated, _ = self.generate()
        self.assertEqual({path.name for path in (generated / 'src').iterdir()},
                         {'host__shared.c'})
        self.assertEqual((generated / 'src/host__shared.c').resolve().name, 'shared.c')
        self.assertEqual((generated / 'main/main.c').resolve().name, 'main.c')


if __name__ == '__main__':
    unittest.main()
