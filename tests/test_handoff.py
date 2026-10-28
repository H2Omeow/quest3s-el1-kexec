#!/usr/bin/env python3
"""Compile actual handoff orchestration with injected host-side dependencies.

This validates cleanup and the irreversible boundary, not ARM instructions,
real driver ABI/CFI compatibility, or a secure-world ownership transfer.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source: Path, name: str) -> str:
    text = source.read_text()
    match = re.search(rf"(?m)^[\w *]+\b{re.escape(name)}\s*\(", text)
    if not match:
        raise ValueError(f"Cannot find function {name} in {source}")
    brace = text.index("{", match.end())
    end = text.index("\n}", brace) + 2
    line = text.count("\n", 0, match.start()) + 1
    return f'#line {line} "{source}"\n{text[match.start():end]}\n'


def main() -> None:
    cases = {
        "handoff_exec": ("kernel/kexec_drv.c", ["quest_execute"]),
        "compat_bindings": (
            "kernel/kexec_compat.c",
            [
                "quest_lookup_name", "quest_kernel_restart_prepare",
                "quest_migrate_to_reboot_cpu", "quest_machine_shutdown",
                "quest_transition_mutex", "quest_freeze_processes",
                "quest_thaw_processes", "quest_block_probing",
                "quest_unblock_probing", "kexec_compat_load",
                "kexec_compat_unload",
            ],
        ),
        "platform_rollback": (
            "kernel/platform_quest.c",
            [
                "quest_function_is",
                "quest_bound_device",
                "quest_get_binding",
                "quest_note_bound",
                "quest_reject_tee_driver",
                "quest_scm_warm_shutdown",
                "quest_watchdog_shutdown",
                "quest_check_watchdog",
                "quest_release_binding",
                "quest_cancel_locked",
                "quest_install",
                "quest_platform_prepare",
                "quest_platform_cancel",
                "quest_platform_shutdown_ok",
            ],
        ),
    }
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="quest-handoff-test-") as tmp:
        for name, (path, names) in cases.items():
            source = ROOT / path
            actual = "\n".join(function(source, item) for item in names)
            template = (ROOT / "tests" / f"{name}.c").read_text()
            if template.count("/* ACTUAL_FUNCTIONS */") != 1:
                raise ValueError(f"Missing extraction marker in {name}")
            output = Path(tmp) / f"{name}.c"
            output.write_text(template.replace("/* ACTUAL_FUNCTIONS */", actual))
            binary = Path(tmp) / name
            subprocess.run(
                compiler
                + [
                    "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-Wno-unused-parameter", "-Wno-sign-compare",
                    "-fsanitize=undefined", str(output), "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
