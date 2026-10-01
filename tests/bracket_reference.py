"""Compare built CPU math adapter against RawBracket's actual production shader.
Usage: python3 tests/bracket_reference.py /path/to/rawbracket_pixels
Requires the sibling RawBracket checkout, moderngl, numpy and EGL.
No compilation is performed by this script.
"""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import numpy as np

reference = Path(__file__).resolve().parents[2] / 'RawBracket/tests/test_gpu.py'
spec = importlib.util.spec_from_file_location('reference_gpu', reference)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.GPU.setUpClass()
gpu = module.GPU()
rng = np.random.default_rng(813)
count = 0
for cfa in range(4):
    for gain in (4, 16, 64):
        for wb in ((1, 1, 1), (2, 1, 2)):
            lb, sb = (64, 65, 66, 67), (32, 34, 36, 38)
            long = rng.integers(64, 1200, size=(12, 14), dtype=np.uint16)
            short = rng.integers(32, 400, size=(12, 14), dtype=np.uint16)
            matrix = np.array([[1.1, -.1, 0], [-.05, 1.1, -.05], [0, -.1, 1.1]])
            luma = np.array(wb) * (np.array([.2126, .7152, .0722]) @ matrix)
            payload = dict(width=14, height=12, cfa=cfa, white=1023, lb=lb, sb=sb,
                           wb=wb, luma=luma.tolist(), gain=gain,
                           long=long.flatten().tolist(), short=short.flatten().tolist())
            cpu = np.array(json.loads(subprocess.check_output(
                [sys.argv[1]], input=json.dumps(payload).encode()))).reshape(12, 14, 3)
            expected = gpu.render(long, short, cfa=cfa, gain=gain, lb=lb, sb=sb,
                                  wb=wb, matrix=matrix)[:, :, :3]
            np.testing.assert_allclose(cpu, expected, rtol=2e-6, atol=2e-6)
            count += 1
print(f'{count} CPU/reference shader comparisons passed')
