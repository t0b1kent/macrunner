# Packaging

The Control Center can be packaged as a native macOS `.app` bundle for distribution.

## Build App Bundle

```bash
./scripts/build-app-bundle.sh
```

Produces:
```
dist/MacRunner Control Center.app/
  Contents/
    Info.plist
    MacOS/
      MacRunner Control Center
    Resources/
      AppIcon.icns
      docs/
      README.md
```

The script ad-hoc signs the bundle so Gatekeeper allows local execution.

## Package for Distribution

```bash
./scripts/package-dmg-like-folder.sh
```

Produces a folder ready for DMG or zip creation:
```
dist/MacRunner_Control_Center_v0.2/
  MacRunner Control Center.app/
  README.txt
  Documentation/
  Applications -> /Applications
```

## Requirements

- macOS 14.0+
- Xcode 15+ / Swift 6.0 toolchain
- Swift Package Manager

## Manual Build

```bash
cd app/macr-control-center
swift build -c release
```

The executable is produced at `.build/release/MacRunnerControlCenter`.
