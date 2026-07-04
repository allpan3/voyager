#!/usr/bin/env python3
"""Compact runner for SystemC unittest binaries."""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
UNITTEST_DIR = SCRIPT_DIR.parent
REPO_ROOT = SCRIPT_DIR.parents[2]
VERILOG_DIR = UNITTEST_DIR / "verilog"

sys.path.insert(0, str(VERILOG_DIR))

from cim_unittest_runner import Printer, display_path, use_color_for  # noqa: E402

PASS_RE = re.compile(r"^\[PASS\]\s+(.+)$")


# Result from one captured command
@dataclass(frozen=True)
class CommandResult:
    returncode: int
    log_path: Path
    output: str


# Run one command and capture combined output into a log
def run_command_to_log(cmd: list[str], log_path: Path) -> CommandResult:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.strftime("%Y-%m-%d %H:%M:%S")
    completed = subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    output = completed.stdout or ""
    log_text = (
        f"$ {shlex.join(cmd)}\n"
        f"# cwd: {REPO_ROOT}\n"
        f"# started: {started}\n"
        f"# exit_code: {completed.returncode}\n\n"
        f"{output}"
    )
    log_path.write_text(log_text)
    return CommandResult(completed.returncode, log_path, output)


# Count compiler warning lines in captured build output
def warning_count(output: str) -> int:
    return sum(1 for line in output.splitlines() if "warning:" in line.lower())


# Return pass case names reported by the SystemC binary
def passed_cases(output: str) -> list[str]:
    names: list[str] = []
    for line in output.splitlines():
        match = PASS_RE.match(line.strip())
        if match is not None:
            names.append(match.group(1))
    return names


# Print the useful failure lines from a captured log
def dump_failure_summary(printer: Printer, suite: str, log_path: Path) -> None:
    printer.event_fields("LOG", case=suite, log=display_path(log_path))
    try:
        lines = log_path.read_text(errors="replace").splitlines()
    except FileNotFoundError:
        printer.raw(f"<missing log: {log_path}>\n")
        return

    interesting = [
        line
        for line in lines
        if "error:" in line.lower()
        or "fatal:" in line.lower()
        or "undefined" in line.lower()
        or "error " in line.lower()
    ]
    if not interesting:
        interesting = lines[-20:]
    for line in interesting[:40]:
        printer.raw(line)


# Parse command-line options for one SystemC test target
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a SystemC unittest with compact logging")
    parser.add_argument("--suite", required=True)
    parser.add_argument("--build-target", required=True)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--log-dir", required=True, type=Path)
    parser.add_argument("--makefile", required=True, type=Path)
    parser.add_argument("--datatype", required=True)
    parser.add_argument("--ic-dimension", required=True)
    parser.add_argument("--oc-dimension", required=True)
    parser.add_argument(
        "--color",
        choices=["auto", "always", "never"],
        default="auto",
        help="Control ANSI color output. Default: auto.",
    )
    return parser.parse_args()


# Run the requested SystemC test and emit Verilog-runner style status lines
def main() -> int:
    args = parse_args()
    printer = Printer(use_color_for(args.color), len(args.suite), 1, 1)
    started = time.perf_counter()

    make_cmd = [
        "make",
        "--no-print-directory",
        "-f",
        str(args.makefile),
        f"DATATYPE={args.datatype}",
        f"IC_DIMENSION={args.ic_dimension}",
        f"OC_DIMENSION={args.oc_dimension}",
        args.build_target,
    ]
    printer.event_fields(
        "CONFIG",
        case=args.suite,
        datatype=args.datatype,
        ic=args.ic_dimension,
        oc=args.oc_dimension,
    )

    build_log = args.log_dir / "build.log"
    build = run_command_to_log(make_cmd, build_log)
    if build.returncode != 0:
        elapsed_s = time.perf_counter() - started
        printer.event_fields(
            "FAIL",
            case=args.suite,
            stage="build",
            rc=build.returncode,
            log=display_path(build.log_path),
            elapsed=f"{elapsed_s:.2f}s",
        )
        dump_failure_summary(printer, args.suite, build.log_path)
        return build.returncode

    if build.output.strip():
        built = 1
        cached = 0
        printer.event_fields("BUILD", case=args.suite, log=display_path(build_log))
    else:
        built = 0
        cached = 1
        printer.event_fields("CACHED", case=args.suite, simulator=display_path(args.binary))

    warnings = warning_count(build.output)
    if warnings:
        printer.event_fields(
            "WARN",
            case=args.suite,
            warnings=warnings,
            log=display_path(build.log_path),
        )

    run_log = args.log_dir / "run.log"
    printer.event_fields("RUN", case=args.suite, log=display_path(run_log))
    run = run_command_to_log([str(args.binary)], run_log)
    names = passed_cases(run.output)
    elapsed_s = time.perf_counter() - started
    if run.returncode != 0:
        printer.event_fields(
            "FAIL",
            case=args.suite,
            stage="run",
            rc=run.returncode,
            log=display_path(run.log_path),
            elapsed=f"{elapsed_s:.2f}s",
        )
        dump_failure_summary(printer, args.suite, run.log_path)
        return run.returncode

    for name in names:
        printer.event_fields("PASS", case=name, sim_runs=1)
    printer.event_fields(
        "SUMMARY",
        passed=len(names),
        failed=0,
        built=built,
        cached=cached,
        illegal=0,
        sim_runs=1,
        elapsed=f"{elapsed_s:.2f}s",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
