"""Compile the real renderer/decoder with fake flash and LCD on a Linux host."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    tmp = pathlib.Path(directory)
    for name in ("zephyr/device.h", "zephyr/devicetree.h",
                 "zephyr/drivers/display.h", "zephyr/kernel.h",
                 "zephyr/storage/flash_map.h", "zephyr/sys/util.h", "lvgl.h"):
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "test_platform.h"\n')
    executable = tmp / "raw-gif-tests"
    flags = [
        "g++", "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-I" + str(tmp),
        "-I" + str(ROOT / "tests"),
        "-I" + str(ROOT / "third_party/AnimatedGIF"),
    ]
    # Upstream deliberately uses unaligned wide loads on x86_64. Its Cortex-M4
    # configuration here uses byte reads instead. Disable only this host-only
    # alignment diagnostic in the vendor translation unit; retain every
    # sanitizer, including alignment, for our renderer and fail on reports.
    decoder = tmp / "decoder.o"
    subprocess.run(flags + ["-fno-sanitize=alignment", "-c",
        str(ROOT / "third_party/AnimatedGIF/AnimatedGIF.cpp"),
        "-o", str(decoder)], check=True)
    subprocess.run(flags + [str(ROOT / "tests/raw_gif_test.cpp"),
        str(decoder), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
