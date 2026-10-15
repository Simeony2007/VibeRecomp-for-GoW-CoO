#!/usr/bin/env python3
"""Inspect PRX paths requested by sceKernelLoadModule and local loader support."""

import argparse
import re
import sys
from pathlib import Path


LEGACY_LOAD = re.compile(r'sceKernelLoadModule path="([^"]*)" valid=(yes|no)')
CORE_LOAD = re.compile(
    r'\[HLE ModuleMgr\] sceKernelLoadModule path="([^"]*)" '
    r'result=(0x[0-9A-Fa-f]+) read=([^ ]+)'
    r'(?: container=([^ ]+) payload=([^ ]+) psp_size=(0x[0-9A-Fa-f]+) '
    r'elf_size=(0x[0-9A-Fa-f]+) compression=(0x[0-9A-Fa-f]+) '
    r'tag=(0x[0-9A-Fa-f]+) action=([^ ]+))?'
)
CORE_RUNTIME_LOAD = re.compile(
    r'\[HLE ModuleMgr\] sceKernelLoadModule path="([^"]*)" '
    r'result=(0x[0-9A-Fa-f]+) '
    r'(?:(?:read=([^ ]+)) ?)?(?:prepare=([^ ]+))?'
    r'(?: .*? action=([^ ]+))?$'
)
CORE_START = re.compile(
    r'\[HLE ModuleMgr\] sceKernelStartModule id=(0x[0-9A-Fa-f]+) '
    r'result=(0x[0-9A-Fa-f]+).*?action=([^ ]+)'
)


def local_module_path(psp_path, game_root):
    normalized = psp_path.replace("\\", "/")
    if normalized.lower().startswith("disc0:/"):
        relative = normalized[len("disc0:/"):]
        return game_root / "umd0" / Path(*relative.split("/"))
    if normalized.lower().startswith("umd0:/"):
        relative = normalized[len("umd0:/"):]
        return game_root / "umd0" / Path(*relative.split("/"))
    return None


def inspect_header(path):
    try:
        with path.open("rb") as source:
            header = source.read(0x150)
    except OSError as exc:
        return {"state": "missing", "detail": str(exc)}

    magic = header[:4]
    if magic == b"\x7fELF":
        return {"state": "elf", "detail": "ELF direto"}
    if magic == b"~PSP":
        if len(header) < 8:
            return {"state": "truncated", "detail": "cabecalho ~PSP incompleto"}
        comp_type = int.from_bytes(header[6:8], "little") & 0xF00
        if comp_type == 0x300:
            return {
                "state": "plain_prx",
                "detail": "PRX ~PSP sem compressao (formato reconhecido pelo loader)",
            }
        return {
            "state": "compressed_prx",
            "detail": f"PRX ~PSP comprimido/cifrado; tipo 0x{comp_type:03X}",
        }
    if magic == b"~SCE":
        return {
            "state": "encrypted_sce",
            "detail": "container ~SCE; precisa de decriptacao PSP/KIRK antes do ELF",
        }
    return {
        "state": "unknown",
        "detail": "magic=" + (magic.hex(" ") or "(arquivo vazio)"),
    }


def inspect_log(console_text, game_root):
    findings = []
    starts = []
    for line_number, line in enumerate(console_text.splitlines(), 1):
        start_match = CORE_START.search(line)
        if start_match:
            starts.append(
                {
                    "line": line_number,
                    "id": start_match.group(1),
                    "result": start_match.group(2),
                    "action": start_match.group(3),
                }
            )

        runtime_match = CORE_RUNTIME_LOAD.search(line)
        if runtime_match:
            psp_path, result, read_status, prepare_status, action = runtime_match.groups()
            host_path = local_module_path(psp_path, game_root)
            header = inspect_header(host_path) if host_path is not None else {
                "state": "unmapped",
                "detail": "prefixo PSP nao resolvido pelo script",
            }
            findings.append(
                {
                    "line": line_number,
                    "psp_path": psp_path,
                    "valid": True,
                    "host_path": host_path,
                    **header,
                    "result": result,
                    "read_status": read_status or "ok",
                    "prepare_status": prepare_status,
                    "action": action,
                }
            )
            continue

        core_match = CORE_LOAD.search(line)
        if core_match:
            (
                psp_path,
                result,
                read_status,
                container,
                payload,
                psp_size,
                elf_size,
                compression,
                tag,
                action,
            ) = core_match.groups()
            runtime = {
                "result": result,
                "read_status": read_status,
                "runtime_container": container,
                "runtime_payload": payload,
                "runtime_psp_size": psp_size,
                "runtime_elf_size": elf_size,
                "runtime_compression": compression,
                "runtime_tag": tag,
                "action": action,
            }
            valid = True
        else:
            legacy_match = LEGACY_LOAD.search(line)
            if not legacy_match:
                continue
            psp_path, valid = legacy_match.groups()
            valid = valid == "yes"
            runtime = {"legacy_fallback": True}
        host_path = local_module_path(psp_path, game_root)
        if host_path is None:
            findings.append(
                {
                    "line": line_number,
                    "psp_path": psp_path,
                    "valid": valid,
                    "host_path": None,
                    "state": "unmapped",
                    "detail": "prefixo PSP nao resolvido pelo script",
                    **runtime,
                }
            )
            continue
        header = inspect_header(host_path)
        findings.append(
            {
                "line": line_number,
                "psp_path": psp_path,
                "valid": valid,
                "host_path": host_path,
                **header,
                **runtime,
            }
        )
    return findings, starts


def report(findings, starts, console_path):
    print(f"Console: {console_path}")
    print(f"Chamadas LoadModule detalhadas: {len(findings)}")
    if not findings:
        print("Nenhum caminho sceKernelLoadModule encontrado.")
        return 0

    for finding in findings:
        valid = "string valida" if finding["valid"] else "string invalida"
        print(
            f"- linha {finding['line']}: {finding['psp_path']} ({valid})"
        )
        if finding["host_path"] is not None:
            print(f"  arquivo: {finding['host_path']}")
        print(f"  formato: {finding['detail']}")
        if finding.get("legacy_fallback"):
            print("  core: fallback antigo retornou zero; a captura nao demonstra abertura do arquivo")
        elif "result" in finding:
            print(
                f"  core: read={finding['read_status']}, result={finding['result']}, "
                f"action={finding.get('action', 'n/a')}"
            )
            if finding.get("prepare_status"):
                print(f"  core ELF preparation: {finding['prepare_status']}")
            if finding.get("runtime_tag"):
                print(
                    f"  PRX recebido pelo core: container={finding['runtime_container']}, "
                    f"payload={finding['runtime_payload']}, "
                    f"psp_size={finding['runtime_psp_size']}, "
                    f"elf_size={finding['runtime_elf_size']}, "
                    f"compression={finding['runtime_compression']}, "
                    f"tag={finding['runtime_tag']}"
                )

    if starts:
        print(f"Chamadas StartModule: {len(starts)}")
        for start in starts:
            print(
                f"- linha {start['line']}: id={start['id']} "
                f"result={start['result']} action={start['action']}"
            )

    encrypted = [item for item in findings if item["state"] == "encrypted_sce"]
    if encrypted:
        loaded = [item for item in encrypted if item.get("action") == "loaded"]
        legacy = [item for item in encrypted if item.get("legacy_fallback")]
        if loaded:
            print(
                "Conclusao: o core decriptou/descomprimiu o PRX e o loader registrou "
                "um UID positivo. start_scheduled confirma que uma thread foi criada, "
                "nao que a inicializacao terminou ou que todos os imports foram resolvidos."
            )
        elif any(item.get("prepare_status") for item in encrypted):
            print(
                "Conclusao: o core abriu o arquivo; verifique prepare_status/action "
                "para localizar uma falha de decriptacao, ELF ou carregamento/relocacao."
            )
        elif any(item.get("read_status") == "ok" for item in encrypted):
            print(
                "Conclusao: o core abriu e reconheceu o PRX, mas esta captura nao "
                "registra o resultado atual da preparacao ELF."
            )
        elif legacy:
            print(
                "Conclusao: esta captura e do fallback antigo: retornou zero sem "
                "demonstrar que abriu o arquivo. O formato ~SCE requer decriptacao "
                "antes que o loader possa prosseguir."
            )
        else:
            print(
                "Conclusao: os PRX encontrados estao em ~SCE; confira no console "
                "se o core conseguiu abri-los e qual erro de carregamento retornou."
            )
    elif any(item["state"] == "missing" for item in findings):
        print(
            "Conclusao: pelo menos um PRX solicitado nao foi encontrado no caminho "
            "local correspondente."
        )
    elif all(item["state"] in ("plain_prx", "elf") for item in findings):
        print(
            "Conclusao: os arquivos estao em formato que o loader atual pode "
            "interpretar; investigue o carregamento/relocacao e o StartModule."
        )
    return 0


def self_test():
    import tempfile

    with tempfile.TemporaryDirectory(prefix="module-loads-") as directory:
        root = Path(directory)
        module = root / "umd0" / "PSP_GAME" / "USRDIR" / "module"
        module.mkdir(parents=True)
        (module / "encrypted.prx").write_bytes(b"~SCE" + bytes(0x14C))
        (module / "plain.prx").write_bytes(
            b"~PSP" + bytes(2) + (0x300).to_bytes(2, "little") + bytes(0x148)
        )
        console = "\n".join(
            [
                '[HLE Fallback Detail] sceKernelLoadModule path="disc0:/PSP_GAME/USRDIR/module/encrypted.prx" valid=yes',
                '[HLE Fallback Detail] sceKernelLoadModule path="disc0:/PSP_GAME/USRDIR/module/plain.prx" valid=yes',
                '[HLE ModuleMgr] sceKernelLoadModule path="disc0:/PSP_GAME/USRDIR/module/encrypted.prx" result=0x00000200 prepare=ok container=~SCE elf_size=5792 entry=0x08900000 module_start=0x08900000 gp=0x08901000 action=loaded',
                '[HLE ModuleMgr] sceKernelStartModule id=0x00000200 result=0x00000000 entry=0x08900000 thread=2 action=start_scheduled',
            ]
        )
        results, starts = inspect_log(console, root)
        assert [item["state"] for item in results] == [
            "encrypted_sce",
            "plain_prx",
            "encrypted_sce",
        ]
        assert results[2]["action"] == "loaded"
        assert starts[0]["action"] == "start_scheduled"
    print("inspect_module_loads self-test: OK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("console_log", nargs="?", type=Path)
    parser.add_argument("--game-root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.console_log is None:
        parser.error("informe o log do console ou use --self-test")
    try:
        console_text = args.console_log.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"erro ao ler {args.console_log}: {exc}", file=sys.stderr)
        return 2
    findings, starts = inspect_log(console_text, args.game_root)
    return report(findings, starts, args.console_log)


if __name__ == "__main__":
    raise SystemExit(main())
