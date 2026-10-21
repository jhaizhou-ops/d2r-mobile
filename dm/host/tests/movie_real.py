"""Generate synthetic VP8 fixtures and compare the movie ABI output with FFmpeg."""
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True).stdout


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="dm-movie-test-") as directory:
        root = Path(directory)
        for audio in (False, True):
            webm, nv12 = root / "synthetic.webm", root / "reference.nv12"
            command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                       "testsrc2=size=160x90:rate=30:duration=2"]
            if audio:
                command += ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2",
                            "-c:a", "libopus"]
            command += ["-c:v", "libvpx", "-deadline", "good", "-cpu-used", "4", "-b:v", "300k",
                        "-g", "30", "-auto-alt-ref", "0", str(webm)]
            run(*command)
            run("ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", webm,
                "-map", "0:v:0", "-pix_fmt", "nv12", "-f", "rawvideo", nv12)
            reference = nv12.read_bytes()
            assert len(reference) == 60 * 160 * 90 * 3 // 2
            result = run(binary, webm, nv12)
            digest = re.search(r"NV12_SHA256=([0-9a-f]{64})", result)
            assert digest and digest[1] == hashlib.sha256(reference).hexdigest(), result
            assert "VP8: 60 frames," in result, result
            print(f"Synthetic VP8 ({'Opus present' if audio else 'video only'}): 60 NV12 frames match FFmpeg")
            print(result, end="")


if __name__ == "__main__":
    main()
