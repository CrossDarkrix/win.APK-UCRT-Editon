#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase 20-A4: integrate a verified A3 jlink image into Win.apk-UCRT.

The script validates the A3 PASS report and every source hash, copies the image
into a sibling temporary directory under the distribution stage, verifies the
copied runtime by hash and execution, then replaces only stage/runtime with
rollback support. It does not run NSIS or modify application binaries.
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
import tempfile
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class IntegrationError(RuntimeError):
    pass


@dataclass
class CommandResult:
    name: str
    command: list[str]
    returncode: int
    stdout: str
    stderr: str


def log(message: str) -> None:
    print(f"[Phase 20-A4] {message}")


def normalize(base: Path, value: Path) -> Path:
    return value.expanduser().resolve() if value.is_absolute() else (base / value).resolve()


def run(name: str, command: Sequence[str], timeout: int = 300) -> CommandResult:
    try:
        completed = subprocess.run(
            list(command), text=True, capture_output=True, timeout=timeout
        )
        return CommandResult(
            name=name,
            command=list(command),
            returncode=completed.returncode,
            stdout=completed.stdout.strip(),
            stderr=completed.stderr.strip(),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return CommandResult(name, list(command), 127, "", f"{type(exc).__name__}: {exc}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative(value: str) -> Path:
    posix = PurePosixPath(value)
    if posix.is_absolute() or not posix.parts or any(part in {"", ".", ".."} for part in posix.parts):
        raise IntegrationError(f"manifestに不正な相対パスがあります: {value!r}")
    if any(":" in part for part in posix.parts):
        raise IntegrationError(f"manifestにドライブ指定を含むパスがあります: {value!r}")
    return Path(*posix.parts)


def validate_stage(stage: Path, app_exe: str) -> Path:
    if not stage.is_dir():
        raise IntegrationError(f"配布ステージがありません: {stage}")
    candidates = [p for p in stage.rglob("*") if p.is_file() and p.name.casefold() == app_exe.casefold()]
    if len(candidates) != 1:
        raise IntegrationError(f"{app_exe}は配布ステージ内に1件必要です。検出数={len(candidates)}")
    return candidates[0]


def load_a3(a3_dir: Path) -> tuple[dict[str, Any], Path, list[dict[str, Any]], Path]:
    report_path = a3_dir / "jlink-report.json"
    manifest_path = a3_dir / "runtime-image-manifest.json"
    if not report_path.is_file():
        raise IntegrationError(f"A3レポートがありません: {report_path}")
    if not manifest_path.is_file():
        raise IntegrationError(f"A3 runtime manifestがありません: {manifest_path}")
    try:
        report = json.loads(report_path.read_text(encoding="utf-8-sig"))
        external_manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise IntegrationError(f"A3成果物を読み込めません: {exc}") from exc
    if report.get("phase") != "20-A3" or report.get("verdict") != "PASS":
        raise IntegrationError("Phase 20-A3のPASSレポートが必要です。")
    if report.get("errors"):
        raise IntegrationError("A3レポートにエラーが残っています。")
    verification = report.get("verification", {})
    required_checks = (
        "requiredFilesPassed", "javaVersionPassed",
        "moduleListPassed", "requestedModulesPassed",
    )
    if not all(verification.get(name) is True for name in required_checks):
        raise IntegrationError("A3の生成後検証がすべてPASSではありません。")
    scope = report.get("scope", {})
    if scope.get("runsJlinkImageGeneration") is not True:
        raise IntegrationError("A3レポートがjlink生成済みであることを確認できません。")

    source = Path(report.get("outputImage", "")).expanduser().resolve()
    if not source.is_dir():
        raise IntegrationError(f"A3 runtime imageがありません: {source}")
    report_files = report.get("image", {}).get("files")
    external_files = external_manifest.get("files")
    if not isinstance(report_files, list) or not report_files:
        raise IntegrationError("A3レポートのimage.filesが空です。")
    if report_files != external_files:
        raise IntegrationError("jlink-report.jsonとruntime-image-manifest.jsonのfilesが一致しません。")
    return report, source, report_files, report_path


def normalize_manifest_entries(entries: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    normalized: dict[str, dict[str, Any]] = {}
    for item in entries:
        if not isinstance(item, dict):
            raise IntegrationError("manifestのファイル項目がobjectではありません。")
        rel = safe_relative(str(item.get("path", "")))
        key = rel.as_posix()
        if key in normalized:
            raise IntegrationError(f"manifestに重複パスがあります: {key}")
        size = item.get("size")
        digest = str(item.get("sha256", "")).lower()
        if not isinstance(size, int) or size < 0 or not SHA256_RE.fullmatch(digest):
            raise IntegrationError(f"manifest項目が不正です: {item}")
        normalized[key] = {"path": key, "size": size, "sha256": digest}
    return normalized


def actual_files(root: Path) -> dict[str, Path]:
    return {
        path.relative_to(root).as_posix(): path
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def verify_tree(root: Path, expected: dict[str, dict[str, Any]], label: str) -> None:
    actual = actual_files(root)
    expected_names = set(expected)
    actual_names = set(actual)
    missing = sorted(expected_names - actual_names)
    extra = sorted(actual_names - expected_names)
    if missing or extra:
        raise IntegrationError(
            f"{label}のファイル集合がmanifestと不一致。missing={missing}, extra={extra}"
        )
    for name, item in expected.items():
        path = actual[name]
        size = path.stat().st_size
        if size != item["size"]:
            raise IntegrationError(
                f"{label}のサイズ不一致: {name}, expected={item['size']}, actual={size}"
            )
        digest = sha256(path)
        if digest != item["sha256"]:
            raise IntegrationError(
                f"{label}のSHA-256不一致: {name}, expected={item['sha256']}, actual={digest}"
            )


def find_runtime_java(runtime: Path) -> Path:
    for name in ("java.exe", "java"):
        path = runtime / "bin" / name
        if path.is_file():
            return path
    raise IntegrationError(f"runtimeのjava実行ファイルがありません: {runtime / 'bin'}")


def verify_required_runtime_files(runtime: Path) -> list[str]:
    required = [runtime / "lib" / "modules", runtime / "release"]
    java = find_runtime_java(runtime)
    if java.name.casefold() == "java.exe":
        required += [runtime / "bin" / "javaw.exe", runtime / "bin" / "server" / "jvm.dll"]
    else:
        jvms = list(runtime.glob("lib/**/libjvm.so")) + list(runtime.glob("lib/**/libjvm.dylib"))
        if not jvms:
            required.append(runtime / "lib" / "server" / "libjvm.so")
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise IntegrationError("runtime必須ファイル不足: " + ", ".join(missing))
    return [str(java), *(str(path) for path in required)]


def parse_modules(text: str) -> list[str]:
    return sorted({line.strip().split("@", 1)[0] for line in text.splitlines() if line.strip()})


def verify_runtime_execution(
    runtime: Path, expected_modules: Sequence[str]
) -> tuple[CommandResult, CommandResult, list[str]]:
    java = find_runtime_java(runtime)
    version = run("integrated-java-version", [str(java), "--version"])
    if version.returncode != 0:
        raise IntegrationError(
            f"統合runtimeのjava --versionに失敗。stdout={version.stdout}, stderr={version.stderr}"
        )
    listed = run("integrated-list-modules", [str(java), "--list-modules"])
    if listed.returncode != 0:
        raise IntegrationError(
            f"統合runtimeのjava --list-modulesに失敗。stdout={listed.stdout}, stderr={listed.stderr}"
        )
    modules = parse_modules(listed.stdout)
    if modules != sorted(set(expected_modules)):
        raise IntegrationError(
            "統合runtimeのモジュール一覧がA3と一致しません。"
            f" expected={sorted(set(expected_modules))}, actual={modules}"
        )
    return version, listed, modules


def write_json_atomic(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temp = Path(temp_name)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(data, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temp, path)
    finally:
        if temp.exists():
            temp.unlink()


def write_text_atomic(path: Path, text: str, encoding: str = "utf-8") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temp = Path(temp_name)
    try:
        with os.fdopen(handle, "w", encoding=encoding, newline="\n") as stream:
            stream.write(text)
        os.replace(temp, path)
    finally:
        if temp.exists():
            temp.unlink()


def publish_runtime(prepared: Path, destination: Path) -> Path | None:
    backup = destination.with_name(destination.name + ".a4-backup")
    if backup.exists():
        shutil.rmtree(backup)
    had_old = destination.exists()
    if had_old:
        destination.rename(backup)
    try:
        prepared.rename(destination)
    except Exception:
        if had_old and backup.exists() and not destination.exists():
            backup.rename(destination)
        raise
    return backup if had_old else None


def rollback_runtime(destination: Path, backup: Path | None) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    if backup and backup.exists():
        backup.rename(destination)


def stage_manifest(stage: Path, excluded_roots: set[str]) -> tuple[list[dict[str, Any]], int]:
    files: list[dict[str, Any]] = []
    total = 0
    for path in sorted(p for p in stage.rglob("*") if p.is_file()):
        rel = path.relative_to(stage)
        if rel.parts and rel.parts[0] in excluded_roots:
            continue
        size = path.stat().st_size
        total += size
        files.append({"path": rel.as_posix(), "size": size, "sha256": sha256(path)})
    return files, total


def markdown(report: dict[str, Any]) -> str:
    lines = [
        "# Phase 20-A4 Runtime Stage Integration", "",
        f"- 判定: **{report['verdict']}**",
        f"- 配布ステージ: `{report['stage']['root']}`",
        f"- 統合runtime: `{report['runtime']['destination']}`",
        f"- runtimeファイル数: {report['runtime']['fileCount']}",
        f"- runtimeサイズ: {report['runtime']['totalBytes']} bytes",
        f"- runtimeモジュール数: {len(report['runtime']['modules'])}", "",
        "## 検証", "",
        "- A3 PASS・manifest整合: **PASS**",
        "- コピー元SHA-256: **PASS**",
        "- コピー後SHA-256: **PASS**",
        "- runtime必須ファイル: **PASS**",
        "- 統合runtime `java --version`: **PASS**",
        "- 統合runtime `java --list-modules`: **PASS**",
        "- A3モジュール一覧一致: **PASS**", "",
        "## A4の範囲", "",
        "`Win.apk-UCRT/runtime`への安全な統合と配布stage manifest生成のみを行います。Win.apk本体の同梱JVMロード検証およびNSIS生成は行いません。", "",
    ]
    return "\n".join(lines)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Phase 20-A4 Runtime Stage Integration")
    parser.add_argument("--project-root", type=Path, default=SCRIPT_DIR)
    parser.add_argument("--a3-dir", type=Path, default=Path("build/phase20-a3"))
    parser.add_argument("--stage-dir", type=Path, default=Path("Win.apk-UCRT"))
    parser.add_argument("--runtime-name", default="runtime")
    parser.add_argument("--output-dir", type=Path, default=Path("build/phase20-a4"))
    parser.add_argument("--app-exe", default="Win.apk.exe")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    prepared: Path | None = None
    backup: Path | None = None
    destination: Path | None = None
    published = False
    try:
        root = args.project_root.expanduser().resolve()
        a3_dir = normalize(root, args.a3_dir)
        stage = normalize(root, args.stage_dir)
        output_dir = normalize(root, args.output_dir)
        if not args.runtime_name or Path(args.runtime_name).name != args.runtime_name:
            raise IntegrationError("--runtime-nameは単一ディレクトリ名で指定してください。")
        app = validate_stage(stage, args.app_exe)
        report_a3, source, raw_entries, a3_report_path = load_a3(a3_dir)
        expected = normalize_manifest_entries(raw_entries)
        if len(expected) != report_a3.get("image", {}).get("fileCount"):
            raise IntegrationError("A3レポートのfileCountとmanifest件数が一致しません。")
        expected_total = sum(item["size"] for item in expected.values())
        if expected_total != report_a3.get("image", {}).get("totalBytes"):
            raise IntegrationError("A3レポートのtotalBytesとmanifest合計が一致しません。")
        verify_tree(source, expected, "A3コピー元runtime")

        destination = stage / args.runtime_name
        prepared = Path(tempfile.mkdtemp(prefix=f".{args.runtime_name}.a4-", dir=stage))
        shutil.rmtree(prepared)
        shutil.copytree(source, prepared, copy_function=shutil.copy2)
        verify_tree(prepared, expected, "コピー後runtime")
        required_files = verify_required_runtime_files(prepared)
        version, listed, modules = verify_runtime_execution(
            prepared, report_a3.get("modules", {}).get("image", [])
        )

        integration_manifest = {
            "schemaVersion": 1,
            "phase": "20-A4",
            "sourceA3Report": str(a3_report_path),
            "sourceA3ReportSha256": sha256(a3_report_path),
            "sourceRuntime": str(source),
            "destinationRuntime": str(destination),
            "fileCount": len(expected),
            "totalBytes": expected_total,
            "requestedModules": report_a3.get("modules", {}).get("requested", []),
            "imageModules": modules,
            "requiredFiles": required_files,
            "files": list(expected.values()),
        }

        backup = publish_runtime(prepared, destination)
        prepared = None
        published = True

        manifests_dir = stage / "manifests"
        write_json_atomic(manifests_dir / "runtime-integration-manifest.json", integration_manifest)
        write_text_atomic(
            manifests_dir / "runtime-files.sha256",
            "".join(f"{item['sha256']}  runtime/{item['path']}\n" for item in expected.values()),
            encoding="ascii",
        )
        write_text_atomic(
            manifests_dir / "runtime-modules.txt",
            "\n".join(modules) + "\n",
            encoding="ascii",
        )

        stage_files, stage_total = stage_manifest(
            stage, {"manifests", destination.name + ".a4-backup"}
        )
        distribution_manifest = {
            "schemaVersion": 1,
            "phase": "20-A4",
            "stageRoot": str(stage),
            "appExecutable": app.relative_to(stage).as_posix(),
            "runtimeDirectory": destination.relative_to(stage).as_posix(),
            "fileCount": len(stage_files),
            "totalBytes": stage_total,
            "files": stage_files,
        }
        write_json_atomic(manifests_dir / "distribution-manifest.json", distribution_manifest)
        write_text_atomic(
            manifests_dir / "distribution-files.sha256",
            "".join(f"{item['sha256']}  {item['path']}\n" for item in stage_files),
            encoding="ascii",
        )

        output_dir.mkdir(parents=True, exist_ok=True)
        report = {
            "schemaVersion": 1,
            "phase": "20-A4",
            "verdict": "PASS",
            "scope": {
                "integratesRuntimeIntoDistribution": True,
                "validatesWinApkRuntimeLoading": False,
                "runsNsis": False,
            },
            "stage": {
                "root": str(stage),
                "appExecutable": str(app),
                "manifest": str(manifests_dir / "distribution-manifest.json"),
                "fileCountExcludingManifests": len(stage_files),
                "totalBytesExcludingManifests": stage_total,
            },
            "runtime": {
                "source": str(source),
                "destination": str(destination),
                "fileCount": len(expected),
                "totalBytes": expected_total,
                "modules": modules,
                "requiredFiles": required_files,
            },
            "verification": {
                "a3Pass": True,
                "sourceHashesPassed": True,
                "copiedHashesPassed": True,
                "requiredFilesPassed": True,
                "javaVersionPassed": True,
                "moduleListPassed": True,
                "a3ModuleListMatched": True,
            },
            "commands": [asdict(version), asdict(listed)],
            "warnings": [],
            "errors": [],
            "generatedAtUtc": datetime.now(timezone.utc).isoformat(),
        }
        write_json_atomic(output_dir / "runtime-integration-report.json", report)
        write_text_atomic(output_dir / "runtime-integration-report.md", markdown(report))
        write_json_atomic(output_dir / "runtime-integration-manifest.json", integration_manifest)

        if backup and backup.exists():
            shutil.rmtree(backup)
            backup = None
        log("PASS")
        log(f"runtime: {destination}")
        log(f"files: {len(expected)}, bytes: {expected_total}")
        log(f"report: {output_dir / 'runtime-integration-report.json'}")
        return 0
    except IntegrationError as exc:
        if published and destination is not None:
            rollback_runtime(destination, backup)
            backup = None
        print(f"[Phase 20-A4] ERROR: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        if published and destination is not None:
            rollback_runtime(destination, backup)
            backup = None
        print(f"[Phase 20-A4] UNEXPECTED: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    finally:
        if prepared and prepared.exists():
            shutil.rmtree(prepared, ignore_errors=True)
        if backup and backup.exists() and not published:
            shutil.rmtree(backup, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
