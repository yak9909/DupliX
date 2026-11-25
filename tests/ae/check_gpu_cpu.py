"""Compares the fast-mode renders of the CPU run (build/test/fast) and the GPU run (build/test/fast_gpu) of
tests/ae/run_fast_mode.ps1: every r_fm_*.tif present in both. The GPU evaluates depth in float, so nearly coplanar
copies may swap at a few pixels: judged by the mean difference and the share of pixels differing by more than 2.
Also fast_gpu_switch: sw_gpu.png (GPU) vs sw_cpu.png (the same session switched to CPU only).
usage: python check_gpu_cpu.py <cpu dir> <gpu dir> <label>  -> exit 0 if all pass."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(p: Path) -> np.ndarray:
    return np.asarray(Image.open(p).convert("RGBA"), dtype=np.int16)


def judge(a: np.ndarray, b: np.ndarray, base_mean: float = 0.0, base_frac: float = 0.0) -> tuple[bool, str]:
    d = np.abs(a - b).max(axis=2)
    ok = a.shape == b.shape and d.mean() <= base_mean + 0.1 and (d > 2).mean() <= base_frac + 0.001
    return ok, f"max={int(d.max())} mean={d.mean():.4f} >2={100 * (d > 2).mean():.4f}%"


def base(cpu: Path, gpu: Path, name: str) -> tuple[float, float, str]:
    """AE's own GPU-vs-CPU difference on the same frame in normal mode (r_nm_*): its half-resolution
    resampling and GPU effects (e.g. a drop shadow) differ from the CPU ones, so randamu's drawing is judged on
    top of that."""
    nm = name.replace("r_fm_", "r_nm_", 1)
    if not ((cpu / nm).exists() and (gpu / nm).exists()):
        return 0.0, 0.0, ""
    d = np.abs(load(gpu / nm) - load(cpu / nm)).max(axis=2)
    return float(d.mean()), float((d > 2).mean()), f" (AE's own GPU-vs-CPU in normal mode: mean={d.mean():.4f})"


def main() -> int:
    cpu, gpu, label = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else ""
    fails, n = 0, 0
    worst = ""
    for g in sorted(gpu.glob("r_fm_*.tif")):
        c = cpu / g.name
        if not c.exists():
            continue
        n += 1
        bm, bf, btxt = base(cpu, gpu, g.name)
        ok, what = judge(load(g), load(c), bm, bf)
        if not ok or bm > 0.01:  # failures, and the frames where AE itself differs
            fails += 0 if ok else 1
            print(f"  {g.name:<36} {'PASS' if ok else 'FAIL'} GPU vs CPU {what}{btxt}")
    print(f"  GPU vs CPU ({label}): {n} frames compared, {fails} failed")
    if n == 0:
        fails += 1
    sg, sc = gpu / "sw_gpu.png", gpu / "sw_cpu.png"
    if sg.exists() and sc.exists():
        a, b = load(sg), load(sc)
        ok, what = judge(a, b)
        drawn = int((a[..., 3] > 0).sum())
        ok = ok and drawn > 1000
        fails += 0 if ok else 1
        print(f"  fast_gpu_switch {'PASS' if ok else 'FAIL'} GPU then CPU only in one session {what} drawn px={drawn}")
    else:
        fails += 1
        print("  fast_gpu_switch FAIL sw_*.png missing")
    print("GPU vs CPU ALL PASS" if fails == 0 else f"GPU vs CPU FAILURES: {fails}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
