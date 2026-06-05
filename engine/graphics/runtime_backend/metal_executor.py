from __future__ import annotations

import json
import subprocess
from pathlib import Path

from engine.graphics.d3d9_to_d3d11 import d3d9_effective_indices
from engine.graphics.metal_ir.render_state import RenderResult, RenderState
from engine.graphics.runtime_backend.mock_executor import MockExecutor


class MetalExecutor:
    """Real Metal executor wrapper.

    The native helper owns Metal device/command queue setup. Python only passes a
    compact render request and validates the resulting PPM/report.
    """

    def __init__(self, helper_path: str | Path | None = None) -> None:
        root = Path(__file__).resolve().parents[3]
        default = root / "engine" / "graphics" / "runtime_backend" / "metal_executor_native" / "metal_render"
        self.helper_path = Path(helper_path) if helper_path else default
        self.backend = "metal"

    def available(self) -> bool:
        if self._helper_stale():
            self._build_helper()
        if not self.helper_path.exists():
            return False
        proc = subprocess.run([str(self.helper_path), "--probe"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10)
        return proc.returncode == 0 and "device=yes" in proc.stdout

    def execute(self, state: RenderState, output_dir: str | Path, *, trace_path: str | None = None, name: str = "render") -> RenderResult:
        out_dir = Path(output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        if self._helper_stale():
            self._build_helper()
        if not self.helper_path.exists():
            return self._skip(state, out_dir, trace_path, name, "metal helper missing")

        request_info = self.write_request(state, out_dir, trace_path=trace_path, name=name)
        request = Path(request_info["request_path"])
        report_path = Path(request_info["payload"]["report"])

        proc = subprocess.run([str(self.helper_path), "--request", str(request)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=20)
        if proc.returncode != 0:
            return self._skip(state, out_dir, trace_path, name, (proc.stderr or proc.stdout or "metal helper failed").strip())
        payload = json.loads(report_path.read_text(encoding="utf-8"))
        return RenderResult(
            status="PASS",
            width=int(payload["width"]),
            height=int(payload["height"]),
            checksum=payload["checksum"],
            non_background_pixels=int(payload["non_background_pixels"]),
            unsupported_calls=state.unsupported_calls,
            validation_errors=list(state.validation_errors),
            warnings=list(state.warnings),
            ppm_path=str(payload["ppm_path"]),
            report_path=str(report_path),
            backend=self.backend,
            api=state.api,
            trace_path=trace_path,
            present_count=state.present_count,
            frame_index=state.frame_index,
        )

    def write_request(self, state: RenderState, output_dir: str | Path, *, trace_path: str | None = None, name: str = "render") -> dict:
        out_dir = Path(output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        request = out_dir / f"{name}.metal-request.json"
        ppm_path = out_dir / f"{name}.metal.ppm"
        report_path = out_dir / f"{name}.metal.report.json"
        payload = self.request_payload(state, ppm_path=ppm_path, report_path=report_path, trace_path=trace_path)
        request.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return {"request_path": str(request), "payload": payload}

    def request_payload(self, state: RenderState, *, ppm_path: str | Path, report_path: str | Path, trace_path: str | None = None) -> dict:
        metadata = state.pipeline.metadata
        indices = d3d9_effective_indices(state.index_buffer, metadata, state.topology) if state.api in {"d3d8", "d3d9"} else list(state.index_buffer)
        mode = "clear"
        if state.texture:
            mode = "texture"
        elif indices:
            mode = "indexed_triangle"
        elif state.vertex_buffer:
            mode = "triangle"
        payload = {
            "mode": mode,
            "source_api": state.api,
            "width": state.width,
            "height": state.height,
            "render_target_format": state.render_target_format,
            "depth_format": state.depth_format,
            "clear_color": list(state.clear_color),
            "topology": state.topology,
            "viewport": list(state.viewport) if state.viewport else None,
            "scissor": list(state.scissor) if state.scissor else None,
            "shader": state.shader,
            "texture_size": list(state.texture_size) if state.texture_size else None,
            "texture_pixels": [list(pixel) for pixel in state.texture] if state.texture else None,
            "vertices": [vertex.to_dict() for vertex in state.vertex_buffer],
            "indices": indices,
            "index_format": state.pipeline.metadata.get("d3d9_index_format", "uint16"),
            "vertex_count": len(state.vertex_buffer),
            "index_count": len(indices),
            "present_count": state.present_count,
            "frame_index": state.frame_index,
            "trace_path": trace_path,
            "output": str(ppm_path),
            "report": str(report_path),
            "pipeline": {
                "primitive_topology": state.pipeline.primitive_topology,
                "input_layout": state.pipeline.input_layout,
                "vertex_shader": state.pipeline.vertex_shader,
                "pixel_shader": state.pipeline.pixel_shader,
                "constant_buffer_bound": state.pipeline.constant_buffer_bound,
                "shader_resource_bound": state.pipeline.shader_resource_bound,
                "render_target_bound": state.pipeline.render_target_bound,
                "depth_target_bound": state.pipeline.depth_target_bound,
                "swapchain_backbuffer_bound": state.pipeline.swapchain_backbuffer_bound,
            },
        }
        if state.api in {"d3d8", "d3d9"}:
            payload["d3d9"] = {
                "translation_target": metadata.get("translation_target"),
                "fixed_function": metadata.get("fixed_function", False),
                "programmable": metadata.get("programmable", False),
                "present_parameters": metadata.get("d3d9_present_parameters", {}),
                "render_states": metadata.get("d3d9_render_states", {}),
                "texture_stage_states": metadata.get("d3d9_texture_stage_states", []),
                "sampler_states": metadata.get("d3d9_sampler_states", []),
                "sampler": metadata.get("d3d9_sampler", {}),
                "depth_state": metadata.get("d3d9_depth_state", {}),
                "transforms": metadata.get("d3d9_transforms", {}),
                "wvp_matrix": metadata.get("d3d9_wvp_matrix"),
                "index_format": metadata.get("d3d9_index_format"),
                "index_format_raw": metadata.get("d3d9_index_format_raw"),
                "draw_range": metadata.get("d3d9_draw_range", {}),
                "texture_format": metadata.get("d3d9_texture_format"),
                "ffp_shader": metadata.get("d3d9_ffp_shader", {}),
            }
        return payload

    def _build_helper(self) -> None:
        makefile_dir = self.helper_path.parent
        if (makefile_dir / "Makefile").exists():
            subprocess.run(["make", "-C", str(makefile_dir)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)

    def _helper_stale(self) -> bool:
        source = self.helper_path.with_suffix(".m")
        if not self.helper_path.exists():
            return True
        return source.exists() and source.stat().st_mtime > self.helper_path.stat().st_mtime

    def _skip(self, state: RenderState, output_dir: Path, trace_path: str | None, name: str, reason: str) -> RenderResult:
        fallback = MockExecutor().execute(state, output_dir, trace_path=trace_path, name=f"{name}.mock-fallback")
        fallback.status = "SKIP"
        fallback.backend = self.backend
        fallback.warnings.append(reason)
        return fallback
