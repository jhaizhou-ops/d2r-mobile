#!/usr/bin/env python3
# 慢帧分析的合成日志回归；不需要游戏内容或 Apple SDK。
# Synthetic-log regressions for slow-frame analysis; no game content or Apple SDK needed.
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'slowframe.py'


class SlowframeTest(unittest.TestCase):
    def report(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frames.tsv'
            path.write_text(rows)
            return subprocess.run([sys.executable, str(SCRIPT), str(path), '--all'],
                                  check=True, capture_output=True, text=True).stdout

    def test_invalid_present_samples_do_not_dilute_denominator(self):
        rows = ''.join(f'{i}\t@presented_ms\t{value}\n'
                       for i, value in enumerate(('0', 'nan', 'inf', '-inf', '-2', '16.667', '25', '25.001', '-1'), 1))
        report = self.report(rows)
        self.assertIn('可判 4 帧', report)
        self.assertIn('未分段 2/4', report)

    def test_windowed_missing_intervals_do_not_fall_back_to_cpu(self):
        report = self.report('1\t@pres_at_ms\t100\n1\t@frame_ms\t40\n')
        self.assertIn('判据 @presented_ms', report)
        self.assertIn('可判 0 帧', report)

    def test_headless_fallback_requires_positive_finite_intervals(self):
        rows = ''.join(f'{i}\t@frame_ms\t{value}\n'
                       for i, value in enumerate(('0', '-1', 'nan', 'inf', '16.667', '40'), 1))
        report = self.report(rows)
        self.assertIn('判据 @frame_ms', report)
        self.assertIn('未分段 1/2', report)

    def test_sparse_log_retains_host_denominator(self):
        report = self.report('[slowframe]\t1\t@presented_ms=-1\n'
                             '[slowframe]\t#\t至帧 300\tjudged=290\tslow=8\t判据=上屏间隔\n')
        self.assertIn('宿主统计 可判 290 帧、掉帧 8 帧', report)
        self.assertNotIn('各段 慢帧 / 可判帧', report)


if __name__ == '__main__':
    unittest.main()
