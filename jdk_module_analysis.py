#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase 20-A2 jdeps Module Analysis for Win.apk.

Win.apk-UCRT/lib のJARとpatch classディレクトリをJDK付属jdepsで解析し、
jlink向けの確定モジュール一覧と機械可読レポートを生成する。
既存成果物およびJDKには変更を加えない。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
EXPECTED_JARS = (
    "android-framework.jar",
    "macapk-loader.jar",
    "bcprov-jdk18on-1.79.jar",
    "winapk-tools.jar",
)
EXPECTED_PATCH_DIRS = ("java_base_patch", "jdk_unsupported_patch")
MODULE_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.]*$")
INTERNAL_EDGE_RE = re.compile(
    r"^\s*(?P<source>\S+)\s+->\s+(?P<target>\S+)\s+JDK internal API \((?P<module>[^)]+)\)\s*$"
)
KNOWN_INTERNAL_APIS: dict[str, set[tuple[str, str, str]]] = {
    "macapk-loader.jar": {
        ("com.macapk.loader.MacApkUnsafe", "sun.misc.Unsafe", "jdk.unsupported"),
    },
    "java_base_patch": {
        ("java.text.SimpleDateFormat", "sun.util.calendar.CalendarUtils", "java.base"),
        ("java.text.SimpleDateFormat", "sun.util.calendar.ZoneInfoFile", "java.base"),
        ("java.text.SimpleDateFormat", "sun.util.locale.provider.LocaleProviderAdapter", "java.base"),
        ("java.text.SimpleDateFormat", "sun.util.locale.provider.LocaleResources", "java.base"),
        ("java.text.SimpleDateFormat", "sun.util.locale.provider.TimeZoneNameUtility", "java.base"),
        ("jdk.internal.reflect.Reflection", "jdk.internal.access.JavaLangAccess", "java.base"),
        ("jdk.internal.reflect.Reflection", "jdk.internal.access.JavaLangReflectAccess", "java.base"),
        ("jdk.internal.reflect.Reflection", "jdk.internal.access.SharedSecrets", "java.base"),
        ("jdk.internal.reflect.Reflection", "jdk.internal.misc.VM", "java.base"),
        ("jdk.internal.reflect.Reflection", "jdk.internal.vm.annotation.ForceInline", "java.base"),
        ("jdk.internal.reflect.Reflection", "jdk.internal.vm.annotation.IntrinsicCandidate", "java.base"),
    },
    "jdk_unsupported_patch": {
        ("sun.misc.Unsafe", "jdk.internal.misc.Unsafe", "java.base"),
    },
}


class AnalysisError(RuntimeError):
    pass


@dataclass
class CommandResult:
    name: str
    command: list[str]
    returncode: int
    stdout: str
    stderr: str


def log(text: str) -> None:
    print(f"[Phase 20-A2] {text}")


def exe(name: str) -> str:
    return name + (".exe" if os.name == "nt" else "")


def run(name: str, command: Sequence[str], timeout: int = 300) -> CommandResult:
    try:
        p = subprocess.run(list(command), text=True, capture_output=True, timeout=timeout)
        return CommandResult(name, list(command), p.returncode, p.stdout.strip(), p.stderr.strip())
    except (OSError, subprocess.TimeoutExpired) as exc:
        return CommandResult(name, list(command), 127, "", f"{type(exc).__name__}: {exc}")


def java_home_from_path() -> Path | None:
    found = shutil.which(exe("java")) or shutil.which("java")
    if not found:
        return None
    parent = Path(found).resolve().parent
    return parent.parent if parent.name.casefold() == "bin" else None


def discover_jdk(explicit: Path | None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(explicit.expanduser())
    for key in ("JAVA_HOME", "JDK_HOME"):
        if os.environ.get(key):
            candidates.append(Path(os.environ[key]).expanduser())
    from_path = java_home_from_path()
    if from_path:
        candidates.append(from_path)
    seen: set[str] = set()
    for raw in candidates:
        home = raw.resolve()
        key = os.path.normcase(str(home))
        if key in seen:
            continue
        seen.add(key)
        if all((home / "bin" / exe(tool)).is_file() for tool in ("java", "jdeps", "jlink")):
            return home
    raise AnalysisError("java/jdeps/jlinkを備えたJDKを検出できません。--jdkで指定してください。")


def parse_feature_version(text: str) -> int | None:
    m = re.search(r'(?m)^(?:openjdk|java)\s+(?:version\s+)?"?(\d+)', text)
    return int(m.group(1)) if m else None


def normalize_path(base: Path, value: Path) -> Path:
    return value.resolve() if value.is_absolute() else (base / value).resolve()


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def discover_inputs(runtime_dist: Path) -> tuple[list[Path], list[Path]]:
    lib = runtime_dist / "lib"
    if not lib.is_dir():
        raise AnalysisError(f"libディレクトリがありません: {lib}")
    jars: list[Path] = []
    for name in EXPECTED_JARS:
        matches = sorted(p for p in lib.rglob(name) if p.is_file())
        if len(matches) != 1:
            raise AnalysisError(f"{name} は1件必要です。検出数={len(matches)}: {matches}")
        jars.append(matches[0].resolve())
    patches: list[Path] = []
    for name in EXPECTED_PATCH_DIRS:
        matches = sorted(p for p in lib.rglob(name) if p.is_dir())
        if len(matches) != 1:
            raise AnalysisError(f"{name} は1件必要です。検出数={len(matches)}: {matches}")
        if not any(p.suffix.casefold() == ".class" for p in matches[0].rglob("*.class")):
            raise AnalysisError(f"classファイルがありません: {matches[0]}")
        patches.append(matches[0].resolve())
    return jars, patches


def parse_modules(text: str) -> list[str]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        return []
    candidate = lines[-1]
    modules = sorted(set(x.strip() for x in candidate.split(",") if x.strip()))
    invalid = [x for x in modules if not MODULE_RE.fullmatch(x)]
    if invalid:
        raise AnalysisError(f"jdeps出力から不正なモジュール名を検出しました: {invalid}; output={text!r}")
    return modules


def parse_explicit_modules(values: Sequence[str], file_path: Path | None) -> list[str]:
    tokens: list[str] = []
    for value in values:
        tokens.extend(re.split(r"[,\s]+", value.strip()))
    if file_path:
        if not file_path.is_file():
            raise AnalysisError(f"追加モジュールファイルがありません: {file_path}")
        for line in file_path.read_text(encoding="utf-8-sig").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                tokens.extend(re.split(r"[,\s]+", line))
    modules = sorted(set(x for x in tokens if x))
    invalid = [x for x in modules if not MODULE_RE.fullmatch(x)]
    if invalid:
        raise AnalysisError(f"不正な明示追加モジュール名: {invalid}")
    return modules


def jdeps_base(jdeps: Path, jdk: Path, multi_release: int, classpath: str) -> list[str]:
    return [str(jdeps), "--multi-release", str(multi_release), "--system", str(jdk), "--class-path", classpath]


def analyze_target(jdeps: Path, jdk: Path, target: Path, classpath: str, multi_release: int) -> tuple[list[str], CommandResult]:
    command = [*jdeps_base(jdeps, jdk, multi_release, classpath), "--recursive", "--print-module-deps", str(target)]
    result = run(f"module-deps:{target.name}", command)
    if result.returncode != 0:
        raise AnalysisError(
            f"jdeps解析に失敗しました: {target}\nreturncode={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
        )
    modules = parse_modules(result.stdout)
    if not modules:
        raise AnalysisError(f"jdepsがモジュールを返しませんでした: {target}")
    return modules, result


def parse_internal_api_edges(text: str) -> set[tuple[str, str, str]]:
    edges: set[tuple[str, str, str]] = set()
    for line in text.splitlines():
        match = INTERNAL_EDGE_RE.match(line)
        if match:
            edges.add((match.group("source"), match.group("target"), match.group("module")))
    return edges


def analyze_internal_api_target(
    jdeps: Path,
    jdk: Path,
    target: Path,
    dependency_entries: Sequence[Path],
    multi_release: int,
) -> tuple[CommandResult, set[tuple[str, str, str]]]:
    # --jdk-internals はclass-path上のクラスも解析対象に含めるため、
    # 対象別の帰属判定ではclass-pathを一切渡さない。
    # 通常の --print-module-deps 解析で欠落依存は厳格に検査済みなので、
    # この補助検査だけは --ignore-missing-deps を使い、対象自身に含まれる
    # JDK内部API参照だけを抽出する。
    del dependency_entries  # API互換用。内部API帰属検査では意図的に使用しない。
    command = [
        str(jdeps),
        "--multi-release", str(multi_release),
        "--system", str(jdk),
        "--jdk-internals",
        "--ignore-missing-deps",
        str(target),
    ]
    result = run(f"jdk-internals:{target.name}", command)
    output = "\n".join(x for x in (result.stdout, result.stderr) if x).strip()
    return result, parse_internal_api_edges(output)


def validate_modules(jlink: Path, jdk: Path, modules: Sequence[str]) -> CommandResult:
    command = [str(jlink), "--module-path", str(jdk / "jmods"), "--add-modules", ",".join(modules), "--suggest-providers", "java.lang.System$LoggerFinder"]
    # --suggest-providersは解決検査を伴うが、runtime imageは生成しない。
    return run("jlink-module-resolution", command)


def rel(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def command_json(result: CommandResult) -> dict[str, Any]:
    return asdict(result)


def markdown(report: dict[str, Any]) -> str:
    lines = [
        "# Phase 20-A2 jdeps Module Analysis", "",
        f"- 判定: **{report['verdict']}**",
        f"- JDK: `{report['jdk']['home']}`",
        f"- 入力: `{report['runtimeDist']}`",
        f"- 自動検出モジュール: `{','.join(report['modules']['jdeps'])}`",
        f"- 明示追加モジュール: `{','.join(report['modules']['explicit']) or '(なし)'}`",
        f"- 最終モジュール: `{','.join(report['modules']['final'])}`", "",
    ]
    if report["errors"]:
        lines += ["## エラー", ""] + [f"- {x}" for x in report["errors"]] + [""]
    if report["warnings"]:
        lines += ["## 警告", ""] + [f"- {x}" for x in report["warnings"]] + [""]
    lines += ["## 入力別解析", ""]
    for item in report["targets"]:
        lines.append(f"- `{item['path']}`: `{','.join(item['modules'])}`")
    lines += ["", "## JDK内部API", ""]
    internal = report["internalApis"]
    lines.append(f"- 検出: **{'あり' if internal['detected'] else 'なし'}**")
    lines.append(f"- 方針: `{internal['policy']}`")
    lines.append(f"- 内部API解析class-path: `{internal['classPathPolicy']}`")
    for item in internal["targets"]:
        lines.append(
            f"- `{item['path']}`: **{item['status']}** / "
            f"検出={len(item['observed'])}, 想定外={len(item['unexpected'])}, "
            f"baseline消失={len(item['missingFromBaseline'])}"
        )
    if internal["output"]:
        lines += ["", "```text", internal["output"], "```"]
    lines += ["", "## A2の範囲", "", "`jdeps --print-module-deps`による解析とモジュール解決確認のみを行います。`jlink --output`によるruntime生成は行いません。", ""]
    return "\n".join(lines)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Phase 20-A2 jdeps Module Analysis")
    p.add_argument("--project-root", type=Path, default=SCRIPT_DIR)
    p.add_argument("--runtime-dist", type=Path, default=Path("Win.apk-UCRT"))
    p.add_argument("--jdk", type=Path)
    p.add_argument("--output-dir", type=Path, default=Path("build/phase20-a2"))
    p.add_argument("--expected-jdk", type=int, default=21)
    p.add_argument("--multi-release", type=int, default=21)
    p.add_argument("--add-modules", action="append", default=[], help="追加モジュール。カンマ区切り可、複数指定可")
    p.add_argument("--add-modules-file", type=Path)
    return p.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    errors: list[str] = []
    warnings: list[str] = []
    report: dict[str, Any] = {
        "schemaVersion": 1, "phase": "20-A2", "verdict": "FAIL",
        "scope": {"readOnly": True, "runsJdepsAnalysis": True, "runsJlinkImageGeneration": False},
        "errors": errors, "warnings": warnings,
    }
    try:
        root = args.project_root.expanduser().resolve()
        runtime = normalize_path(root, args.runtime_dist)
        out = normalize_path(root, args.output_dir)
        out.mkdir(parents=True, exist_ok=True)
        jdk = discover_jdk(args.jdk)
        java = jdk / "bin" / exe("java")
        jdeps = jdk / "bin" / exe("jdeps")
        jlink = jdk / "bin" / exe("jlink")
        java_version = run("java-version", [str(java), "--version"])
        feature = parse_feature_version("\n".join((java_version.stdout, java_version.stderr)))
        if java_version.returncode != 0 or feature != args.expected_jdk:
            raise AnalysisError(f"JDK {args.expected_jdk}が必要です。検出={feature}, home={jdk}")
        if not (jdk / "jmods").is_dir():
            raise AnalysisError(f"jmodsがありません: {jdk / 'jmods'}")

        jars, patches = discover_inputs(runtime)
        targets = [*jars, *patches]
        classpath_entries = [*jars, *patches]
        classpath = os.pathsep.join(str(p) for p in classpath_entries)
        explicit_file = normalize_path(root, args.add_modules_file) if args.add_modules_file else None
        explicit = parse_explicit_modules(args.add_modules, explicit_file)

        target_reports: list[dict[str, Any]] = []
        commands: list[dict[str, Any]] = [command_json(java_version)]
        automatic: set[str] = set()
        for target in targets:
            modules, result = analyze_target(jdeps, jdk, target, classpath, args.multi_release)
            automatic.update(modules)
            target_reports.append({"path": rel(target, runtime), "type": "jar" if target.is_file() else "class-directory", "modules": modules})
            commands.append(command_json(result))

        internal_target_reports: list[dict[str, Any]] = []
        internal_outputs: list[str] = []
        for target in targets:
            # 対象単体で内部APIを検査する。class-pathは渡さないため、
            # 他JARやpatchの内部APIが現在の対象へ混入しない。
            internal_result, observed_edges = analyze_internal_api_target(
                jdeps, jdk, target, (), args.multi_release
            )
            commands.append(command_json(internal_result))
            output = "\n".join(x for x in (internal_result.stdout, internal_result.stderr) if x).strip()
            if output:
                internal_outputs.append(f"===== {rel(target, runtime)} =====\n{output}")
            if internal_result.returncode != 0:
                raise AnalysisError(f"--jdk-internals検査が失敗しました: {target}\n{output}")

            is_patch = target.is_dir()
            expected_edges = KNOWN_INTERNAL_APIS.get(target.name, set())
            unexpected_edges = observed_edges - expected_edges
            missing_edges = expected_edges - observed_edges
            status = "PASS"
            if unexpected_edges or missing_edges:
                status = "FAIL"
                if unexpected_edges:
                    errors.append(
                        f"{rel(target, runtime)}: 許可されていないJDK内部API依存を検出: "
                        + "; ".join(f"{a} -> {b} ({m})" for a, b, m in sorted(unexpected_edges))
                    )
                if missing_edges:
                    errors.append(
                        f"{rel(target, runtime)}: 既知のpatch内部API依存が消失・変化: "
                        + "; ".join(f"{a} -> {b} ({m})" for a, b, m in sorted(missing_edges))
                    )
            elif observed_edges:
                warnings.append(f"{rel(target, runtime)}: 既知の内部API依存 {len(observed_edges)}件をbaseline一致として許可しました。")

            internal_target_reports.append({
                "path": rel(target, runtime),
                "type": "patch-class-directory" if is_patch else "jar",
                "status": status,
                "observed": [
                    {"source": a, "target": b, "module": m}
                    for a, b, m in sorted(observed_edges)
                ],
                "allowedBaseline": [
                    {"source": a, "target": b, "module": m}
                    for a, b, m in sorted(expected_edges)
                ],
                "unexpected": [
                    {"source": a, "target": b, "module": m}
                    for a, b, m in sorted(unexpected_edges)
                ],
                "missingFromBaseline": [
                    {"source": a, "target": b, "module": m}
                    for a, b, m in sorted(missing_edges)
                ],
                "rawOutput": output,
            })

        internal_output = "\n\n".join(internal_outputs)
        internal_detected = any(item["observed"] for item in internal_target_reports)

        final_modules = sorted(automatic | set(explicit))
        if "java.base" not in final_modules:
            final_modules.insert(0, "java.base")
        resolution = validate_modules(jlink, jdk, final_modules)
        commands.append(command_json(resolution))
        if resolution.returncode != 0:
            errors.append("最終モジュール集合をJDKのjmodsから解決できません。")

        report.update({
            "projectRoot": str(root), "runtimeDist": str(runtime),
            "jdk": {"home": str(jdk), "featureVersion": feature, "versionOutput": java_version.stdout or java_version.stderr, "jmods": str(jdk / "jmods")},
            "inputs": {
                "jars": [{"path": rel(p, runtime), "size": p.stat().st_size, "sha256": sha256(p)} for p in jars],
                "patchClassDirectories": [{"path": rel(p, runtime), "classCount": sum(1 for _ in p.rglob('*.class'))} for p in patches],
                "classPath": [rel(p, runtime) for p in classpath_entries],
            },
            "targets": target_reports,
            "modules": {"jdeps": sorted(automatic), "explicit": explicit, "final": final_modules},
            "internalApis": {
                "detected": internal_detected,
                "policy": "all-targets-exact-baseline; unlisted-internal-apis-forbidden",
                "classPathPolicy": "none; target-only; ignore-missing-deps-for-attribution-only",
                "targets": internal_target_reports,
                "output": internal_output,
            },
            "moduleResolution": {"passed": resolution.returncode == 0, "stdout": resolution.stdout, "stderr": resolution.stderr},
            "commands": commands,
        })
        report["verdict"] = "FAIL" if errors else "PASS"

        (out / "runtime-modules-jdeps.txt").write_text(",".join(sorted(automatic)) + "\n", encoding="ascii")
        (out / "runtime-modules-explicit.txt").write_text(",".join(explicit) + "\n", encoding="ascii")
        (out / "runtime-modules-final.txt").write_text(",".join(final_modules) + "\n", encoding="ascii")
        (out / "jdeps-internals.txt").write_text(internal_output + ("\n" if internal_output else ""), encoding="utf-8")
        (out / "jdeps-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (out / "jdeps-report.md").write_text(markdown(report), encoding="utf-8", newline="\n")
        log(report["verdict"])
        log("最終モジュール: " + ",".join(final_modules))
        log(f"レポート: {out / 'jdeps-report.json'}")
        for item in errors:
            print(f"ERROR: {item}", file=sys.stderr)
        for item in warnings:
            print(f"WARNING: {item}", file=sys.stderr)
        return 1 if errors else 0
    except AnalysisError as exc:
        errors.append(str(exc))
        report["verdict"] = "FAIL"
        print(f"[Phase 20-A2] ERROR: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        errors.append(f"{type(exc).__name__}: {exc}")
        print(f"[Phase 20-A2] UNEXPECTED: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
