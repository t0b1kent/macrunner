# STATUS graphics phase 1 - MoltenVK

Date: 2026-05-14T05:48:49Z
Scope: MoltenVK native arm64 artifact for Vulkan-to-Metal layer.

## Gate

Command:

```text
./scripts/build-moltenvk.sh
```

Output:

```text
MacRunner graphics: MoltenVK native arm64 artifact
source: /Volumes/MacOS/MacRunner/engine/moltenvk
/Volumes/MacOS/MacRunner/engine/graphics/dist/lib/libMoltenVK.dylib: Mach-O 64-bit dynamically linked shared library arm64
/System/Library/Frameworks/Metal.framework/Versions/A/Metal
MoltenVK PASS: /Volumes/MacOS/MacRunner/engine/graphics/dist/lib/libMoltenVK.dylib
```

## Result

PASS. Physical artifact exists at `engine/graphics/dist/lib/libMoltenVK.dylib`.

## Limitation

Default script copies the installed native Homebrew MoltenVK dylib. Source rebuild is supported by `MACRUNNER_MOLTENVK_FROM_SOURCE=1 ./scripts/build-moltenvk.sh` but was not required for this pass because the native dylib was present and verified.
