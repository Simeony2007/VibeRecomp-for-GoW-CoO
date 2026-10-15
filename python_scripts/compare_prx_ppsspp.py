#!/usr/bin/env python3
"""Run PPSSPP's PRX crypto routine on local game modules for comparison."""

import argparse
import gzip
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PPSSPP = ROOT / "PPSSPP"
DEFAULT_MODULES = (
    ROOT / "umd0" / "PSP_GAME" / "USRDIR" / "module" / "audiocodec.prx",
    ROOT / "umd0" / "PSP_GAME" / "USRDIR" / "module" / "libatrac3plus.prx",
    ROOT / "umd0" / "PSP_GAME" / "USRDIR" / "module" / "sc_sascore.prx",
)
KIRK_SOURCES = (
    "kirk_engine.c",
    "AES.c",
    "SHA1.c",
    "bn.c",
    "ec.c",
    "amctrl.c",
)

HARNESS = r"""
#include "Core/ELF/PrxDecrypter.h"
#include "Common/Log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

bool logEnabled = false;
bool *g_bLogEnabledSetting = &logEnabled;
LogChannel g_log[(size_t)Log::NUMBER_OF_LOGS]{};
void GenericLog(Log, LogLevel, const char *, int, const char *, ...) {}

static unsigned int read32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<unsigned char> file(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (input.bad() || file.size() < 8) {
        std::printf("result=read_error\n");
        return 0;
    }

    size_t offset = 0;
    if (std::memcmp(file.data(), "~SCE", 4) == 0) {
        offset = read32(file.data() + 4);
        if (offset < 8 || offset >= file.size()) {
            std::printf("result=bad_sce_header\n");
            return 0;
        }
    }
    if (file.size() - offset < 0x150 ||
        std::memcmp(file.data() + offset, "~PSP", 4) != 0) {
        std::printf("result=not_psp_prx\n");
        return 0;
    }

    const unsigned char *prx = file.data() + offset;
    const unsigned int pspSize = read32(prx + 0x2C);
    const unsigned int elfSize = read32(prx + 0x28);
    const unsigned int tag = read32(prx + 0xD0);
    if (pspSize < 0x150 || pspSize > file.size() - offset ||
        elfSize > 24 * 1024 * 1024) {
        std::printf("result=bad_psp_sizes psp_size=%u elf_size=%u\n", pspSize, elfSize);
        return 0;
    }

    std::vector<unsigned char> decrypted(std::max(pspSize, elfSize));
    const int result = pspDecryptPRX(prx, decrypted.data(), pspSize, nullptr);
    std::printf("result=%d container=%s tag=0x%08X psp_size=%u elf_size=%u "
                "decrypted_magic=%02X%02X%02X%02X\n",
                result, offset ? "~SCE -> ~PSP" : "~PSP", tag, pspSize, elfSize,
                decrypted[0], decrypted[1], decrypted[2], decrypted[3]);
    if (result > 0 && (size_t)result <= decrypted.size()) {
        std::ofstream output(argv[2], std::ios::binary);
        output.write((const char *)decrypted.data(), result);
        if (!output) return 3;
    }
    return 0;
}
"""

RUNTIME_HARNESS = r"""
#include "mips_prx.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    MIPS_PRX_File file;
    MIPS_PRX_ReadStatus read_status;
    MIPS_PRX_PrepareStatus prepare_status;
    unsigned char *elf_data = NULL;
    size_t elf_size = 0;
    FILE *output;

    if (argc != 3) return 2;
    read_status = mips_prx_read_file(argv[1], &file);
    if (read_status != MIPS_PRX_READ_OK) {
        printf("read=%s\n", mips_prx_read_status_name(read_status));
        return 1;
    }
    prepare_status = mips_prx_prepare_elf(&file, &elf_data, &elf_size);
    if (prepare_status != MIPS_PRX_PREPARE_OK) {
        printf("prepare=%s\n", mips_prx_prepare_status_name(prepare_status));
        mips_prx_file_release(&file);
        return 1;
    }
    output = fopen(argv[2], "wb");
    if (output == NULL || fwrite(elf_data, 1, elf_size, output) != elf_size) {
        if (output != NULL) fclose(output);
        free(elf_data);
        mips_prx_file_release(&file);
        return 1;
    }
    fclose(output);
    printf("prepare=ok elf_size=%zu\n", elf_size);
    free(elf_data);
    mips_prx_file_release(&file);
    return 0;
}
"""


def run(command, *, cwd, capture_output=False):
    result = subprocess.run(
        command,
        cwd=cwd,
        capture_output=capture_output,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        details = (result.stdout or "") + (result.stderr or "")
        raise RuntimeError(
            f"Command failed ({result.returncode}): {command[0]}\n{details}"
        )
    return result


def build_reference(executable, temp):
    gcc = shutil.which("gcc")
    gxx = shutil.which("g++")
    if gcc is None or gxx is None:
        raise RuntimeError("gcc and g++ are required; run this script in the project WSL environment")

    ext = PPSSPP / "ext" / "libkirk"
    include_args = ["-I", str(PPSSPP), "-I", str(ext)]
    objects = []
    for source_name in KIRK_SOURCES:
        source = ext / source_name
        obj = temp / (source.stem + ".o")
        run(
            [gcc, "-std=c11", *include_args, "-c", str(source), "-o", str(obj)],
            cwd=ROOT,
        )
        objects.append(str(obj))

    harness = temp / "prx_decrypt_reference.cpp"
    harness.write_text(HARNESS, encoding="utf-8")
    run(
        [
            gxx,
            "-std=c++17",
            "-I",
            str(PPSSPP),
            *include_args,
            str(harness),
            str(PPSSPP / "Core" / "ELF" / "PrxDecrypter.cpp"),
            *objects,
            "-o",
            str(executable),
        ],
        cwd=ROOT,
    )
    return objects


def build_runtime_probe(executable, objects, temp):
    gcc = shutil.which("gcc")
    gxx = shutil.which("g++")
    if gcc is None or gxx is None:
        raise RuntimeError("gcc and g++ are required; run this script in the project WSL environment")
    harness = temp / "prx_runtime_probe.c"
    harness.write_text(RUNTIME_HARNESS, encoding="utf-8")
    harness_object = temp / "prx_runtime_probe.o"
    reader_object = temp / "mips_prx_reader.o"
    runtime_object = temp / "mips_prx_runtime.o"
    include_args = ["-I", str(ROOT), "-I", str(PPSSPP),
                    "-I", str(PPSSPP / "ext" / "libkirk")]
    run([gcc, "-std=c11", *include_args, "-c", str(harness),
         "-o", str(harness_object)], cwd=ROOT)
    run([gcc, "-std=c11", *include_args, "-c", str(ROOT / "mips_prx.c"),
         "-o", str(reader_object)], cwd=ROOT)
    run([gxx, "-std=c++17", *include_args, "-c",
         str(ROOT / "mips_prx_runtime.cpp"), "-o", str(runtime_object)],
        cwd=ROOT)
    run(
        [
            gxx,
            *include_args,
            str(harness_object),
            str(reader_object),
            str(runtime_object),
            str(PPSSPP / "Core" / "ELF" / "PrxDecrypter.cpp"),
            *objects,
            "-lz",
            "-o",
            str(executable),
        ],
        cwd=ROOT,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("modules", nargs="*", type=Path)
    args = parser.parse_args()
    modules = args.modules or list(DEFAULT_MODULES)
    missing = [str(path) for path in modules if not path.is_file()]
    if missing:
        parser.error("module file not found: " + ", ".join(missing))

    try:
        with tempfile.TemporaryDirectory(prefix="ppsspp-prx-reference-") as temp_dir:
            temp = Path(temp_dir)
            executable = temp / "ppsspp_prx_decrypt_reference"
            runtime_probe = temp / "core_prx_runtime_probe"
            kirk_objects = build_reference(executable, temp)
            build_runtime_probe(runtime_probe, kirk_objects, temp)
            failed = False
            print(
                "Compare PPSSPP and core PRX decrypt + gzip outputs "
                "(ELF registration/start are tested by runtime)"
            )
            for index, module in enumerate(modules):
                decrypted_path = Path(temp_dir) / f"decrypted-{index}.bin"
                runtime_elf_path = Path(temp_dir) / f"runtime-elf-{index}.bin"
                result = run(
                    [str(executable), str(module), str(decrypted_path)],
                    cwd=ROOT,
                    capture_output=True,
                )
                output = result.stdout.strip()
                print(f"{module}: {output}")
                match = re.search(r"\bresult=(-?\d+)\b", output)
                if match is None or int(match.group(1)) <= 0:
                    failed = True
                    continue
                try:
                    decrypted = decrypted_path.read_bytes()
                    elf = gzip.decompress(decrypted)
                except (OSError, EOFError, gzip.BadGzipFile, zlib.error) as exc:
                    print(f"  gzip/ELF validation failed: {exc}")
                    failed = True
                    continue
                if len(elf) < 0x34 or elf[:4] != b"\x7fELF":
                    print(f"  decompressed payload is not an ELF ({len(elf)} bytes)")
                    failed = True
                    continue
                machine = struct.unpack_from("<H", elf, 0x12)[0]
                print(
                    f"  PPSSPP-equivalent gzip step: ELF={len(elf)} bytes, "
                    f"machine={machine} ({'MIPS' if machine == 8 else 'unexpected'})"
                )
                if machine != 8:
                    failed = True
                    continue

                runtime_result = run(
                    [str(runtime_probe), str(module), str(runtime_elf_path)],
                    cwd=ROOT,
                    capture_output=True,
                )
                runtime_output = runtime_result.stdout.strip()
                print(f"  core runtime: {runtime_output}")
                if not runtime_output.startswith("prepare=ok "):
                    failed = True
                    continue
                runtime_elf = runtime_elf_path.read_bytes()
                if runtime_elf != elf:
                    print("  mismatch: core output differs from PPSSPP gzip output")
                    failed = True
                else:
                    print("  byte-for-byte match with PPSSPP ELF output")
            return 1 if failed else 0
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
