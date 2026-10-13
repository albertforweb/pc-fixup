# Windows Speed Master

Windows Speed Master is a native Windows desktop application for reviewing common automatic-start locations. It uses the Win32 API and Windows Task Scheduler COM interfaces; it has no UI framework or third-party runtime dependency. The release executable is named `WindowsSpeedMaster.exe`.

## Current scope

The first version is a read-only inventory of:

- Automatic and delayed automatic Windows services
- `Run` and `RunOnce` values in the current-user and local-machine registry
- Tasks with boot or logon triggers
- Per-user and common Startup folders

The main window has a File/View menu bar, a Refresh toolbar button, a status bar with visible and total counts, and a draggable split view. The left tree groups entries by source; selecting a category filters the report list on the right.

Each row records its source and available startup details. The application does not decide that an unfamiliar entry is safe to disable, and it does not change the machine. “Review” is intentional: a process being non-Microsoft or using resources is not enough evidence that it is safe to remove.

## Build

Build on Windows with Visual Studio 2022 (C++ desktop workload) and CMake:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

With GNU Make installed, the Makefile provides shortcuts:

```sh
make build    # Build Debug
make release  # Build Release and stage dist/WindowsSpeedMaster.exe
make clean    # Remove build/ and dist/
```

## GitHub Actions release

The Windows workflow builds x64 on pushes to `main`, pull requests targeting `main`, and manual runs. Each push to `main` publishes a commit-specific prerelease. Pushing a version tag such as `v0.1.0` publishes a regular GitHub Release. Both include the `.exe`, a ZIP archive, and its SHA-256 checksum:

```sh
git tag v0.1.0
git push origin v0.1.0
```

The inventory is a starting point for a later reversible optimization workflow. That workflow should preserve original settings, show the exact change and its impact, and require the user to apply each recommendation. Dependency relationships should be presented as evidence and uncertainty, not treated as proof that an item is safe to disable.
