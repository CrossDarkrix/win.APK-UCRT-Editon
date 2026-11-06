#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase 20-A3: generate and verify a custom JDK 21 runtime with jlink.

Consumes the PASS report and final module list emitted by Phase 20-A2.
The image is built in a sibling temporary directory and published only after
all verification checks pass. Existing output is never silently overwritten.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
MODULE_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.]*$")


class BuildError(RuntimeError):
    pass


@dataclass
class CommandResult:
    name: str
    command: list[str]
    returncode: int
    stdout: str
    stderr: str


def exe(name: str) -> str:
    return name + (".exe" if os.name == "nt" else "")


def run(name: str, command: Sequence[str], timeout: int = 900) -> CommandResult:
    try:
        process = subprocess.run(
            list(command), text=True, capture_output=True, timeout=timeout
        )
        return CommandResult(
            name, list(command), process.returncode,
            process.stdout.strip(), process.stderr.strip()
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return CommandResult(name, list(command), 127, "", f"{type(exc).__name__}: {exc}")


def normalize(base: Path, value: Path) -> Path:
    return value.expanduser().resolve() if value.is_absolute() else (base / value).resolve()


def discover_jdk(explicit: Path | None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(explicit)
    for key in ("JAVA_HOME", "JDK_HOME"):
        if os.environ.get(key):
            candidates.append(Path(os.environ[key]))
    found = shutil.which(exe("java")) or shutil.which("java")
    if found:
        parent = Path(found).resolve().parent
        if parent.name.casefold() == "bin":
            candidates.append(parent.parent)
    seen: set[str] = set()
    for raw in candidates:
        home = raw.expanduser().resolve()
        key = os.path.normcase(str(home))
        if key in seen:
            continue
        seen.add(key)
        if all((home / "bin" / exe(tool)).is_file() for tool in ("java", "jlink")):
            return home
    raise BuildError("java/jlinkを備えたJDKを検出できません。--jdkで指定してください。")


def parse_feature_version(text: str) -> int | None:
    match = re.search(r'(?m)^(?:openjdk|java)\s+(?:version\s+)?"?(\d+)', text)
    return int(match.group(1)) if match else None


def parse_modules(text: str) -> list[str]:
    values = [x for x in re.split(r"[,\s]+", text.strip()) if x]
    modules = sorted(set(values))
    invalid = [x for x in modules if not MODULE_RE.fullmatch(x)]
    if invalid:
        raise BuildError(f"不正なモジュール名: {invalid}")
    if not modules:
        raise BuildError("最終モジュール一覧が空です。")
    if "java.base" not in modules:
        raise BuildError("最終モジュール一覧にjava.baseがありません。")
    return modules


def load_a2(a2_dir: Path) -> tuple[dict[str, Any], list[str], Path, Path]:
    report_path = a2_dir / "jdeps-report.json"
    modules_path = a2_dir / "runtime-modules-final.txt"
    if not report_path.is_file():
        raise BuildError(f"A2レポートがありません: {report_path}")
    if not modules_path.is_file():
        raise BuildError(f"A2最終モジュール一覧がありません: {modules_path}")
    try:
        report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BuildError(f"A2レポートを読めません: {exc}") from exc
    if report.get("phase") != "20-A2" or report.get("verdict") != "PASS":
        raise BuildError("Phase 20-A2のPASSレポートが必要です。")
    if report.get("errors"):
        raise BuildError("A2レポートにエラーが残っています。")
    if report.get("moduleResolution", {}).get("passed") is not True:
        raise BuildError("A2のモジュール解決確認がPASSではありません。")
    modules = parse_modules(modules_path.read_text(encoding="utf-8-sig"))
    report_modules = sorted(set(report.get("modules", {}).get("final", [])))
    if modules != report_modules:
        raise BuildError(
            "runtime-modules-final.txtとjdeps-report.jsonの最終モジュールが一致しません。"
        )
    return report, modules, report_path, modules_path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def image_manifest(root: Path) -> tuple[list[dict[str, Any]], int]:
    entries: list[dict[str, Any]] = []
    total = 0
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        size = path.stat().st_size
        total += size
        entries.append({
            "path": path.relative_to(root).as_posix(),
            "size": size,
            "sha256": sha256(path),
        })
    return entries, total


def parse_list_modules(text: str) -> list[str]:
    result: list[str] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        result.append(line.split("@", 1)[0])
    return sorted(set(result))


def required_image_files(image: Path) -> list[Path]:
    files = [
        image / "bin" / exe("java"),
        image / "lib" / "modules",
        image / "release",
    ]
    if os.name == "nt":
        files.append(image / "bin" / "server" / "jvm.dll")
    else:
        candidates = list((image / "lib").glob("**/libjvm.so"))
        if candidates:
            files.append(candidates[0])
    return files


def atomic_publish(temp_image: Path, output: Path, replace: bool) -> None:
    backup: Path | None = None
    if output.exists():
        if not replace:
            raise BuildError(f"出力先が既に存在します。更新する場合は--replaceを指定: {output}")
        backup = output.with_name(output.name + ".previous")
        if backup.exists():
            if backup.is_dir():
                shutil.rmtree(backup)
            else:
                backup.unlink()
        output.rename(backup)
    try:
        temp_image.rename(output)
    except Exception:
        if backup and backup.exists() and not output.exists():
            backup.rename(output)
        raise
    if backup and backup.exists():
        shutil.rmtree(backup)


def markdown(report: dict[str, Any]) -> str:
    lines = [
        "# Phase 20-A3 jlink Runtime Image", "",
        f"- 判定: **{report['verdict']}**",
        f"- JDK: `{report['jdk']['home']}`",
        f"- 出力: `{report['outputImage']}`",
        f"- ルートモジュール: `{','.join(report['modules']['requested'])}`",
        f"- 生成イメージ内モジュール数: {len(report['modules']['image'])}",
        f"- ファイル数: {report['image']['fileCount']}",
        f"- 合計サイズ: {report['image']['totalBytes']} bytes", "",
    ]
    if report["errors"]:
        lines += ["## エラー", ""] + [f"- {x}" for x in report["errors"]] + [""]
    if report["warnings"]:
        lines += ["## 警告", ""] + [f"- {x}" for x in report["warnings"]] + [""]
    lines += [
        "## 検証", "",
        f"- 必須ファイル: **{'PASS' if report['verification']['requiredFilesPassed'] else 'FAIL'}**",
        f"- `java --version`: **{'PASS' if report['verification']['javaVersionPassed'] else 'FAIL'}**",
        f"- `java --list-modules`: **{'PASS' if report['verification']['moduleListPassed'] else 'FAIL'}**",
        f"- 要求モジュール包含: **{'PASS' if report['verification']['requestedModulesPassed'] else 'FAIL'}**",
        "", "## A3の範囲", "",
        "jlink runtime imageの生成と静的・起動検証のみを行います。Win.apk-UCRTへの組込みやNSIS生成は行いません。", "",
    ]
    return "\n".join(lines)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Phase 20-A3 jlink Runtime Image")
    parser.add_argument("--project-root", type=Path, default=SCRIPT_DIR)
    parser.add_argument("--a2-dir", type=Path, default=Path("build/phase20-a2"))
    parser.add_argument("--jdk", type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("build/phase20-a3"))
    parser.add_argument("--image-name", default="runtime-image")
    parser.add_argument("--expected-jdk", type=int, default=21)
    parser.add_argument("--compress", choices=("0", "1", "2"), default="2")
    parser.add_argument("--bind-services", action="store_true")
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--keep-debug", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    errors: list[str] = []
    warnings: list[str] = []
    commands: list[dict[str, Any]] = []
    work_parent: Path | None = None
    try:
        root = args.project_root.expanduser().resolve()
        a2_dir = normalize(root, args.a2_dir)
        output_dir = normalize(root, args.output_dir)
        if not args.image_name or Path(args.image_name).name != args.image_name:
            raise BuildError("--image-nameは単一のディレクトリ名で指定してください。")
        output = output_dir / args.image_name
        report, modules, a2_report_path, modules_path = load_a2(a2_dir)
        jdk = discover_jdk(args.jdk)
        java = jdk / "bin" / exe("java")
        jlink = jdk / "bin" / exe("jlink")
        jmods = jdk / "jmods"
        if not jmods.is_dir():
            raise BuildError(f"jmodsがありません: {jmods}")

        version = run("jdk-version", [str(java), "--version"])
        commands.append(asdict(version))
        feature = parse_feature_version("\n".join((version.stdout, version.stderr)))
        if version.returncode != 0 or feature != args.expected_jdk:
            raise BuildError(f"JDK {args.expected_jdk}が必要です。検出={feature}, home={jdk}")
        a2_feature = report.get("jdk", {}).get("featureVersion")
        if a2_feature != feature:
            raise BuildError(f"A2のJDK feature={a2_feature}とA3のJDK feature={feature}が不一致です。")

        output_dir.mkdir(parents=True, exist_ok=True)
        work_parent = Path(tempfile.mkdtemp(prefix=".phase20-a3-", dir=output_dir))
        temp_image = work_parent / args.image_name
        saved_opts = work_parent / "jlink-options.txt"
        command = [
            str(jlink),
            "--module-path", str(jmods),
            "--add-modules", ",".join(modules),
            "--output", str(temp_image),
            f"--compress={args.compress}",
            "--no-header-files",
            "--no-man-pages",
            "--save-opts", str(saved_opts),
        ]
        if not args.keep_debug:
            command.append("--strip-debug")
        if args.bind_services:
            command.append("--bind-services")
        linked = run("jlink", command)
        commands.append(asdict(linked))
        if linked.returncode != 0:
            raise BuildError(f"jlinkに失敗しました。\nstdout={linked.stdout}\nstderr={linked.stderr}")

        missing = [str(path) for path in required_image_files(temp_image) if not path.is_file()]
        if missing:
            raise BuildError("生成イメージの必須ファイル不足: " + ", ".join(missing))
        image_java = temp_image / "bin" / exe("java")
        image_version = run("image-java-version", [str(image_java), "--version"])
        commands.append(asdict(image_version))
        if image_version.returncode != 0:
            raise BuildError(f"生成runtimeのjava --versionが失敗: {image_version.stderr}")
        listed = run("image-list-modules", [str(image_java), "--list-modules"])
        commands.append(asdict(listed))
        if listed.returncode != 0:
            raise BuildError(f"生成runtimeのjava --list-modulesが失敗: {listed.stderr}")
        image_modules = parse_list_modules(listed.stdout)
        missing_modules = sorted(set(modules) - set(image_modules))
        if missing_modules:
            raise BuildError("生成runtimeに要求モジュールがありません: " + ", ".join(missing_modules))

        files, total_bytes = image_manifest(temp_image)
        if not files or total_bytes <= 0:
            raise BuildError("生成runtimeのファイルmanifestが空です。")
        saved_opts_text = saved_opts.read_text(encoding="utf-8", errors="replace") if saved_opts.is_file() else ""
        atomic_publish(temp_image, output, args.replace)
        shutil.rmtree(work_parent)
        work_parent = None

        report_out: dict[str, Any] = {
            "schemaVersion": 1,
            "phase": "20-A3",
            "verdict": "PASS",
            "scope": {"runsJlinkImageGeneration": True, "integratesIntoDistribution": False, "runsNsis": False},
            "projectRoot": str(root),
            "a2": {
                "directory": str(a2_dir),
                "report": str(a2_report_path),
                "reportSha256": sha256(a2_report_path),
                "moduleFile": str(modules_path),
                "moduleFileSha256": sha256(modules_path),
            },
            "jdk": {"home": str(jdk), "featureVersion": feature, "jmods": str(jmods)},
            "outputImage": str(output),
            "options": {
                "compress": int(args.compress),
                "stripDebug": not args.keep_debug,
                "noHeaderFiles": True,
                "noManPages": True,
                "bindServices": args.bind_services,
                "savedOptions": saved_opts_text,
            },
            "modules": {"requested": modules, "image": image_modules, "missing": []},
            "verification": {
                "requiredFilesPassed": True,
                "javaVersionPassed": image_version.returncode == 0,
                "moduleListPassed": listed.returncode == 0,
                "requestedModulesPassed": True,
            },
            "image": {"fileCount": len(files), "totalBytes": total_bytes, "files": files},
            "commands": commands,
            "warnings": warnings,
            "errors": errors,
        }
        report_json = output_dir / "jlink-report.json"
        report_md = output_dir / "jlink-report.md"
        manifest_json = output_dir / "runtime-image-manifest.json"
        sha_file = output_dir / "runtime-image.sha256"
        report_json.write_text(json.dumps(report_out, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        report_md.write_text(markdown(report_out), encoding="utf-8", newline="\n")
        manifest_json.write_text(json.dumps({"root": str(output), "files": files}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        sha_file.write_text("".join(f"{item['sha256']}  {item['path']}\n" for item in files), encoding="ascii")
        print("[Phase 20-A3] PASS")
        print("runtime:", output)
        print("modules:", ",".join(image_modules))
        print("report:", report_json)
        return 0
    except BuildError as exc:
        errors.append(str(exc))
        print(f"[Phase 20-A3] ERROR: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        errors.append(f"{type(exc).__name__}: {exc}")
        print(f"[Phase 20-A3] UNEXPECTED: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    finally:
        if work_parent and work_parent.exists():
            shutil.rmtree(work_parent, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
