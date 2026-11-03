# Contributing to Win.apk

Thanks for helping. There are two easy ways in.

## Report how an app runs

[Open an issue](../../issues/new) with:

- the Win.apk version and your Windows version,
- the app, its version, and where you got it,
- what happened, and
- the app's log, `%LOCALAPPDATA%\Win.apk\logs\last-run.log` (or the install
  log, `install-<app>.log`, if installing failed). Look it over before posting.

## Send code

1. Read [`windows/README.md`](windows/README.md) and get a build running.
2. Make your change on a branch and open a pull request. Say what it fixes and
   which apps you tested it on. The checks in `.github/workflows/` run on it.

### How the code is meant to work

- **Behave like Android.** Framework classes under `runtime/java/android/...`
  mirror AOSP's public API and observable behaviour. Check the AOSP source for
  the same class before changing one.
- **No app-specific code.** No branches on a package name, no offsets that only
  fit one app's binary. Fix the framework, shim or host so every app gets the
  right behaviour.
- **No emulation or virtualization.** Java runs on HotSpot, x86_64 native code
  runs as is, and the bridge is at the Bionic libc ABI, not at Linux syscalls.
- **Windows code lives in `windows/`.** Change `runtime/` only where Windows
  needs a hook, and list the change in `windows/README.md`.
- **License-clean dependencies.** BSD, MIT, Apache-2.0 and similar are fine.
- **Comments explain why.** Especially for ABI work: put the Bionic and Windows
  layouts side by side above a shim.

### Out of scope

Pull requests that defeat license checks or copy protection, forge Play
Integrity or other attestation tokens, bypass account or payment security, or
help obtain apps you don't have the right to use will be closed.

## Security problems

See [SECURITY.md](SECURITY.md). Please don't open a public issue for those.
