#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase 20-A1 JDK Runtime Preflight for Win.apk.

Read-only診断ツール。既存ビルド成果物とJDKを調査し、JSON/Markdown/TXTを出力する。
jdeps解析とjlink生成は行わない（A2/A3で実装）。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any, Iterable, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_RUNTIME_CANDIDATES = (
    Path("build-windows-ucrt64/winapk-runtime-dist"),
    Path("windows/build/runtime"),
    Path("Win.apk-UCRT"),
)
EXPECTED_JARS = (
    "android-framework.jar",
    "macapk-loader.jar",
    "bcprov-jdk18on-1.79.jar",
    "winapk-tools.jar",
)
EXPECTED_PATCH_DIRS = ("java_base_patch", "jdk_unsupported_patch")
DEBUG_DLL_PATTERNS = (
    re.compile(r"^ucrtbased\.dll$", re.I),
    re.compile(r"^(?:vcruntime|msvcp|concrt|vcomp).*d\.dll$", re.I),
)
MACHINE_NAMES = {0x014C: "x86", 0x8664: "x86_64", 0xAA64: "aarch64", 0x01C4: "armv7"}


class PreflightError(RuntimeError):
    pass


@dataclass
class CommandResult:
    command: list[str]
    returncode: int
    stdout: str
    stderr: str


@dataclass
class ToolInfo:
    name: str
    path: str | None
    version_output: str | None
    returncode: int | None
    same_jdk_home: bool | None


def run(command: Sequence[str], timeout: int = 30) -> CommandResult:
    try:
        p = subprocess.run(list(command), text=True, capture_output=True, timeout=timeout)
        return CommandResult(list(command), p.returncode, p.stdout.strip(), p.stderr.strip())
    except (OSError, subprocess.TimeoutExpired) as exc:
        return CommandResult(list(command), 127, "", f"{type(exc).__name__}: {exc}")


def executable(name: str) -> str:
    return name + (".exe" if os.name == "nt" else "")


def resolve_tool(home: Path, name: str) -> Path | None:
    p = home / "bin" / executable(name)
    return p.resolve() if p.is_file() else None


def java_home_from_java(java: Path) -> Path:
    parent = java.resolve().parent
    if parent.name.casefold() == "bin":
        return parent.parent
    raise PreflightError(f"Java実行ファイルからJDKルートを判定できません: {java}")


def discover_jdk(explicit: Path | None) -> tuple[Path | None, list[dict[str, str]]]:
    candidates: list[tuple[str, Path]] = []
    if explicit:
        candidates.append(("--jdk", explicit.expanduser()))
    for key in ("JAVA_HOME", "JDK_HOME"):
        if os.environ.get(key):
            candidates.append((key, Path(os.environ[key]).expanduser()))
    found_java = shutil.which(executable("java")) or shutil.which("java")
    if found_java:
        try:
            candidates.append(("PATH", java_home_from_java(Path(found_java))))
        except PreflightError:
            pass
    if os.name == "nt":
        for env_name in ("ProgramFiles", "ProgramFiles(x86)"):
            root = os.environ.get(env_name)
            if not root:
                continue
            for vendor in ("Java", "Eclipse Adoptium", "Microsoft", "Amazon Corretto"):
                base = Path(root) / vendor
                if base.is_dir():
                    for child in sorted(base.iterdir(), reverse=True):
                        if child.is_dir():
                            candidates.append((f"{env_name}/{vendor}", child))
    seen: set[str] = set()
    evidence: list[dict[str, str]] = []
    for source, raw in candidates:
        home = raw.resolve()
        key = os.path.normcase(str(home))
        if key in seen:
            continue
        seen.add(key)
        java = resolve_tool(home, "java")
        javac = resolve_tool(home, "javac")
        jdeps = resolve_tool(home, "jdeps")
        jlink = resolve_tool(home, "jlink")
        evidence.append({
            "source": source, "home": str(home),
            "java": str(java) if java else "missing",
            "javac": str(javac) if javac else "missing",
            "jdeps": str(jdeps) if jdeps else "missing",
            "jlink": str(jlink) if jlink else "missing",
        })
        if java and javac and jdeps and jlink:
            return home, evidence
    return None, evidence


def parse_properties(text: str) -> dict[str, str]:
    props: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"^\s*([^=]+?)\s*=\s*(.*)$", line)
        if m:
            props[m.group(1).strip()] = m.group(2).strip()
    return props


def architecture_family(value: str) -> str:
    v = value.casefold().replace("-", "_")
    if v in {"amd64", "x86_64", "x64"}: return "x86_64"
    if v in {"x86", "i386", "i486", "i586", "i686"}: return "x86"
    if v in {"aarch64", "arm64"}: return "aarch64"
    if v.startswith("arm"): return "arm"
    return v or "unknown"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def pe_machine(path: Path) -> str | None:
    try:
        with path.open("rb") as f:
            if f.read(2) != b"MZ": return None
            f.seek(0x3C); offset = struct.unpack("<I", f.read(4))[0]
            f.seek(offset)
            if f.read(4) != b"PE\0\0": return None
            machine = struct.unpack("<H", f.read(2))[0]
            return MACHINE_NAMES.get(machine, f"0x{machine:04x}")
    except (OSError, struct.error):
        return None


def detect_runtime_dist(base: Path, explicit: Path | None) -> tuple[Path | None, list[str]]:
    candidates = [explicit] if explicit else list(DEFAULT_RUNTIME_CANDIDATES)
    checked: list[str] = []
    for candidate in candidates:
        if candidate is None: continue
        p = candidate if candidate.is_absolute() else base / candidate
        p = p.resolve(); checked.append(str(p))
        if p.is_dir(): return p, checked
    return None, checked


def relative_or_absolute(path: Path, root: Path) -> str:
    try: return path.relative_to(root).as_posix()
    except ValueError: return str(path)


def scan_artifacts(runtime: Path | None) -> dict[str, Any]:
    result: dict[str, Any] = {
        "root": str(runtime) if runtime else None,
        "jars": [], "classDirectories": [], "nativeLibraries": [],
        "expectedJars": {}, "expectedPatchDirectories": {},
        "debugRuntimeCandidates": [], "architectures": {},
    }
    if runtime is None: return result
    jars = sorted(p for p in runtime.rglob("*.jar") if p.is_file())
    native = sorted(p for p in runtime.rglob("*") if p.is_file() and p.suffix.casefold() in {".dll", ".exe"})
    class_dirs: list[Path] = []
    for wanted in EXPECTED_PATCH_DIRS:
        class_dirs.extend(p for p in runtime.rglob(wanted) if p.is_dir())
    result["jars"] = [{"path": relative_or_absolute(p, runtime), "size": p.stat().st_size, "sha256": sha256(p)} for p in jars]
    result["classDirectories"] = [relative_or_absolute(p, runtime) for p in sorted(set(class_dirs))]
    for filename in EXPECTED_JARS:
        matches = [relative_or_absolute(p, runtime) for p in jars if p.name == filename]
        result["expectedJars"][filename] = matches
    for dirname in EXPECTED_PATCH_DIRS:
        result["expectedPatchDirectories"][dirname] = [x for x in result["classDirectories"] if Path(x).name == dirname]
    for p in native:
        arch = pe_machine(p)
        rel = relative_or_absolute(p, runtime)
        result["nativeLibraries"].append({"path": rel, "size": p.stat().st_size, "architecture": arch})
        if arch: result["architectures"][arch] = result["architectures"].get(arch, 0) + 1
        if any(rx.match(p.name) for rx in DEBUG_DLL_PATTERNS): result["debugRuntimeCandidates"].append(rel)
    return result


def collect_tool(home: Path, name: str, args: Sequence[str]) -> ToolInfo:
    path = resolve_tool(home, name)
    if path is None: return ToolInfo(name, None, None, None, None)
    result = run([str(path), *args])
    text = "\n".join(x for x in (result.stdout, result.stderr) if x)
    try: same = path.parent.parent.samefile(home)
    except OSError: same = path.parent.parent.resolve() == home.resolve()
    return ToolInfo(name, str(path), text, result.returncode, same)


def verdict(report: dict[str, Any], require_artifacts: bool) -> tuple[str, list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    jdk = report["jdk"]
    if not jdk["home"]: errors.append("完全なJDK（java/javac/jdeps/jlink）を検出できませんでした。")
    else:
        if jdk["featureVersion"] != 21: errors.append(f"JDK 21が必要ですが、検出値は {jdk['featureVersion']!r} です。")
        if not jdk["jmodsPresent"]: errors.append("JDKのjmodsディレクトリがありません。JDK 21のjlink入力を確認できません。")
        for tool in report["tools"]:
            if not tool["path"] or tool["returncode"] != 0: errors.append(f"{tool['name']}が利用できません。")
            elif tool["same_jdk_home"] is not True: errors.append(f"{tool['name']}が選択JDK外を参照しています。")
    artifacts = report["artifacts"]
    if not artifacts["root"]:
        (errors if require_artifacts else warnings).append("既存Java runtime成果物ディレクトリを検出できませんでした。")
    else:
        missing_jars = [k for k,v in artifacts["expectedJars"].items() if not v]
        missing_dirs = [k for k,v in artifacts["expectedPatchDirectories"].items() if not v]
        if missing_jars: errors.append("必須JAR不足: " + ", ".join(missing_jars))
        if missing_dirs: errors.append("patch classディレクトリ不足: " + ", ".join(missing_dirs))
        if artifacts["debugRuntimeCandidates"]: errors.append("Debug Runtime候補を検出: " + ", ".join(artifacts["debugRuntimeCandidates"]))
    host = report["host"]["architectureFamily"]
    java_arch = jdk.get("architectureFamily")
    if java_arch and java_arch != host:
        errors.append(f"ホストCPU({host})とJDK({java_arch})のアーキテクチャが一致しません。")
    native_arches = [k for k in artifacts.get("architectures", {}) if k]
    if java_arch and native_arches and java_arch not in native_arches:
        warnings.append(f"JDK({java_arch})と同じPEアーキテクチャを成果物から確認できません。検出: {native_arches}")
    return ("FAIL" if errors else "PASS"), errors, warnings


def markdown(report: dict[str, Any]) -> str:
    lines = ["# Phase 20-A1 JDK Runtime Preflight", "", f"- 判定: **{report['verdict']}**", f"- プロジェクト: `{report['projectRoot']}`", f"- JDK: `{report['jdk']['home'] or '未検出'}`", f"- Java成果物: `{report['artifacts']['root'] or '未検出'}`", ""]
    if report["errors"]:
        lines += ["## エラー", ""] + [f"- {x}" for x in report["errors"]] + [""]
    if report["warnings"]:
        lines += ["## 警告", ""] + [f"- {x}" for x in report["warnings"]] + [""]
    lines += ["## JDK", "", f"- バージョン: `{report['jdk'].get('javaVersion')}`", f"- ベンダー: `{report['jdk'].get('vendor')}`", f"- アーキテクチャ: `{report['jdk'].get('architecture')}`", f"- `jmods`: `{report['jdk'].get('jmodsPresent')}`", "", "## ツール", ""]
    lines += [f"- {t['name']}: `{t['path']}` / returncode={t['returncode']} / sameJdk={t['same_jdk_home']}" for t in report["tools"]]
    lines += ["", "## Java成果物", "", f"- JAR数: {len(report['artifacts']['jars'])}", f"- patch classディレクトリ数: {len(report['artifacts']['classDirectories'])}", f"- PEファイル数: {len(report['artifacts']['nativeLibraries'])}", "", "### 必須JAR", ""]
    lines += [f"- {k}: {'OK' if v else 'MISSING'}" for k,v in report["artifacts"]["expectedJars"].items()]
    lines += ["", "### patch classディレクトリ", ""]
    lines += [f"- {k}: {'OK' if v else 'MISSING'}" for k,v in report["artifacts"]["expectedPatchDirectories"].items()]
    lines += ["", "## A1の範囲", "", "このスクリプトは読取り専用の事前検査です。`jdeps`による必要モジュール解析と`jlink` runtime生成は実行しません。", ""]
    return "\n".join(lines)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Phase 20-A1 JDK Runtime Preflight")
    p.add_argument("--project-root", type=Path, default=SCRIPT_DIR)
    p.add_argument("--jdk", type=Path)
    p.add_argument("--runtime-dist", type=Path)
    p.add_argument("--output-dir", type=Path, default=Path("build/preflight"))
    p.add_argument("--require-artifacts", action="store_true", default=False)
    return p.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    root = args.project_root.expanduser().resolve()
    out = args.output_dir if args.output_dir.is_absolute() else root / args.output_dir
    out.mkdir(parents=True, exist_ok=True)
    jdk_home, candidates = discover_jdk(args.jdk)
    tools: list[ToolInfo] = []
    jdk_data: dict[str, Any] = {"home": str(jdk_home) if jdk_home else None, "candidates": candidates}
    if jdk_home:
        java = resolve_tool(jdk_home, "java")
        assert java
        props_result = run([str(java), "-XshowSettings:properties", "-version"])
        props = parse_properties("\n".join((props_result.stdout, props_result.stderr)))
        version = props.get("java.version", "")
        m = re.match(r"^(?:1\.)?(\d+)", version)
        jdk_data.update({
            "javaVersion": version or None,
            "featureVersion": int(m.group(1)) if m else None,
            "vendor": props.get("java.vendor"),
            "runtimeName": props.get("java.runtime.name"),
            "runtimeVersion": props.get("java.runtime.version"),
            "vmName": props.get("java.vm.name"),
            "architecture": props.get("os.arch"),
            "architectureFamily": architecture_family(props.get("os.arch", "")),
            "reportedJavaHome": props.get("java.home"),
            "jmodsPresent": (jdk_home / "jmods").is_dir(),
            "jmodsCount": len(list((jdk_home / "jmods").glob("*.jmod"))) if (jdk_home / "jmods").is_dir() else 0,
            "releaseFilePresent": (jdk_home / "release").is_file(),
            "javaPropertiesReturnCode": props_result.returncode,
        })
        tools = [collect_tool(jdk_home, "java", ["--version"]), collect_tool(jdk_home, "javac", ["--version"]), collect_tool(jdk_home, "jdeps", ["--version"]), collect_tool(jdk_home, "jlink", ["--version"])]
    runtime, checked = detect_runtime_dist(root, args.runtime_dist)
    artifacts = scan_artifacts(runtime)
    artifacts["checkedCandidates"] = checked
    report: dict[str, Any] = {
        "schemaVersion": 1,
        "phase": "20-A1",
        "projectRoot": str(root),
        "host": {"system": platform.system(), "release": platform.release(), "machine": platform.machine(), "architectureFamily": architecture_family(platform.machine()), "python": platform.python_version()},
        "jdk": jdk_data,
        "tools": [asdict(x) for x in tools],
        "artifacts": artifacts,
        "scope": {"readOnly": True, "runsJdepsAnalysis": False, "runsJlink": False},
    }
    status, errors, warnings = verdict(report, args.require_artifacts)
    report.update({"verdict": status, "errors": errors, "warnings": warnings})
    (out / "jdk-preflight-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / "jdk-preflight-report.md").write_text(markdown(report), encoding="utf-8", newline="\n")
    (out / "jdk-preflight-summary.txt").write_text(f"verdict={status}\njdk={jdk_data.get('home')}\nruntime={artifacts.get('root')}\nerrors={len(errors)}\nwarnings={len(warnings)}\n", encoding="utf-8")
    print(f"[Phase 20-A1] {status}")
    print(f"report: {out / 'jdk-preflight-report.json'}")
    for item in errors: print(f"ERROR: {item}", file=sys.stderr)
    for item in warnings: print(f"WARNING: {item}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
