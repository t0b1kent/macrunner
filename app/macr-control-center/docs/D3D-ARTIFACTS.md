# D3D Artifacts

When D3D tracing is enabled, the launcher produces several artifact types that the Control Center can browse and preview.

## Artifact Types

| Extension | Description | Viewer |
|-----------|-------------|--------|
| `.trace` | Raw D3D call trace | Trace timeline |
| `.ir` | Translated IR output | IR report |
| `.ppm` | Frame-buffer captures | PPM image preview |
| `.json` | Structured D3D report | JSON tree |

## PPM Image Preview

The Control Center includes a native PPM parser that reads `P6` binary PPM files and renders them in SwiftUI. It supports:
- 24-bit RGB (`maxval <= 255`)
- Automatic aspect-ratio preservation

## Trace Timeline

The trace viewer converts raw trace files into a scrollable timeline of D3D API calls with timestamps and arguments.

## Comparing Runs

Use the **D3D** tab to select two runs and compare PPM outputs side-by-side.
