#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from engine.graphics.metal_ir.render_state import RenderState, Vertex
from engine.graphics.d3d9_to_d3d11 import apply_d3d9_event
from engine.graphics.runtime_backend.mock_executor import MockExecutor
from engine.graphics.runtime_backend.metal_executor import MetalExecutor


def _color(value: Any) -> tuple[int, int, int, int]:
    vals = list(value)
    while len(vals) < 4:
        vals.append(255)
    return tuple(int(v * 255 if isinstance(v, float) and 0 <= v <= 1 else v) for v in vals[:4])  # type: ignore[return-value]


def _vertex(payload: dict[str, Any]) -> Vertex:
    return Vertex(
        position=tuple(float(x) for x in payload.get("position", [0, 0, 0, 1])),
        color=tuple(float(x) for x in payload.get("color", [1, 1, 1, 1])),
        uv=tuple(float(x) for x in payload.get("uv", [0, 0])),
    )


def load_trace(path: Path) -> RenderState:
    state: RenderState | None = None
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            event = json.loads(line)
            api = event.get("api", "d3d11")
            command = event.get("command", "")
            payload = event.get("payload", {})
            if state is None:
                state = RenderState(api=api, width=int(payload.get("width", event.get("width", 64))), height=int(payload.get("height", event.get("height", 64))))
            state.commands.append(type("TraceCommandCompat", (), {"to_dict": lambda self, e=event: e})())
            if api in {"d3d8", "d3d9"} and apply_d3d9_event(state, command, payload):
                pass
            elif command in {"create_render_target", "create_swapchain"}:
                state.width = int(payload.get("width", state.width))
                state.height = int(payload.get("height", state.height))
                state.pipeline.render_target_bound = True
                if command == "create_swapchain":
                    state.pipeline.swapchain_backbuffer_bound = True
            elif command in {"clear", "clear_render_target", "clear_rtv"}:
                state.clear_color = _color(payload.get("color", [0, 0, 0, 255]))
                state.pipeline.render_target_bound = True
            elif command == "set_viewport":
                state.viewport = tuple(int(payload[k]) for k in ("x", "y", "width", "height"))
                state.pipeline.viewport_set = True
            elif command == "set_scissor":
                state.scissor = tuple(int(payload[k]) for k in ("x", "y", "width", "height"))
                state.pipeline.scissor_set = True
            elif command == "set_topology":
                state.topology = payload.get("topology", "trianglelist")
            elif command == "set_shader":
                state.shader = payload.get("shader", "vertex-color")
                state.pipeline.vertex_shader = payload.get("vertex_shader", "fixture-vs")
                state.pipeline.pixel_shader = payload.get("pixel_shader", "fixture-ps")
            elif command == "set_vertex_buffer":
                state.vertex_buffer = [_vertex(v) for v in payload.get("vertices", [])]
            elif command == "set_index_buffer":
                state.index_buffer = [int(i) for i in payload.get("indices", [])]
            elif command == "set_texture":
                state.texture_size = (int(payload["width"]), int(payload["height"]))
                state.texture = [_color(p) for p in payload.get("pixels", [])]
                state.pipeline.shader_resource_bound = True
            elif command == "set_constant_buffer":
                state.pipeline.constant_buffer_bound = True
                state.pipeline.metadata["constant_buffer"] = payload
            elif command == "create_descriptor_heap":
                state.pipeline.metadata.setdefault("descriptor_heaps", []).append(payload)
            elif command == "create_root_signature":
                state.pipeline.metadata["root_signature"] = payload
            elif command == "create_fence":
                state.pipeline.metadata["fence"] = payload
            elif command == "wait_fence":
                state.pipeline.metadata["fence_wait"] = payload
            elif command == "create_depth_target":
                state.depth_format = str(payload.get("format", "d24s8"))
                state.pipeline.depth_target_bound = True
            elif command == "clear_depth":
                state.pipeline.depth_target_bound = True
                state.pipeline.metadata["depth_clear"] = payload
            elif command == "set_blend_state":
                state.pipeline.metadata["blend_state"] = payload
            elif command == "set_rasterizer_state":
                state.pipeline.metadata["rasterizer_state"] = payload
            elif command == "set_sampler":
                state.pipeline.metadata.setdefault("samplers", []).append(payload)
            elif command == "copy_resource":
                state.pipeline.metadata.setdefault("copy_resource", []).append(payload)
            elif command == "resize_swapchain":
                state.width = int(payload.get("width", state.width))
                state.height = int(payload.get("height", state.height))
                state.pipeline.swapchain_backbuffer_bound = True
            elif command == "resource_barrier":
                err = state.pipeline.validate_barrier(str(payload.get("before", "")), str(payload.get("after", "")))
                if err:
                    state.validation_errors.append(err)
            elif command in {"object_create", "object_ref", "object_release", "method_call"}:
                pass
            elif command == "present":
                state.present_count += 1
                state.frame_index += 1
            elif command in {"draw", "draw_indexed", "execute_command_lists", "close", "signal"}:
                pass
            else:
                state.unsupported_calls += 1
                state.warnings.append(f"unsupported {command} at line {line_no}")
    if state is None:
        raise ValueError(f"empty trace: {path}")
    return state


def replay(trace: Path, output_dir: Path, backend: str, *, fail_on_unsupported: bool = False) -> dict[str, Any]:
    state = load_trace(trace)
    if fail_on_unsupported and state.unsupported_calls:
        state.validation_errors.append("unsupported calls present")
    executor = MetalExecutor() if backend == "metal" else MockExecutor()
    result = executor.execute(state, output_dir, trace_path=str(trace), name=trace.stem)
    return result.to_dict()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace")
    parser.add_argument("--backend", choices=["mock", "metal"], default="mock")
    parser.add_argument("--output-dir", default="artifacts/d3d-trace")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--fail-on-unsupported", action="store_true")
    args = parser.parse_args()
    payload = replay(Path(args.trace), Path(args.output_dir), args.backend, fail_on_unsupported=args.fail_on_unsupported)
    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print(f"{payload['status']} backend={payload['backend']} ppm={payload.get('ppm_path')} non_bg={payload['non_background_pixels']}")
    return 0 if payload["status"] in {"PASS", "SKIP"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
