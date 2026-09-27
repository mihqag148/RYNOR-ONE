"""Compile the real renderer/decoder with fake flash and LCD on a Linux host."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    tmp = pathlib.Path(directory)
    for name in ("zephyr/device.h", "zephyr/devicetree.h",
                 "zephyr/drivers/display.h", "zephyr/storage/flash_map.h",
                 "zephyr/sys/util.h", "lvgl.h"):
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "test_platform.h"\n')
    executable = tmp / "raw-gif-tests"
    subprocess.run([
        "g++", "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I" + str(tmp),
        "-I" + str(ROOT / "tests"),
        "-I" + str(ROOT / "third_party/AnimatedGIF"),
        str(ROOT / "tests/raw_gif_test.cpp"),
        str(ROOT / "third_party/AnimatedGIF/AnimatedGIF.cpp"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
