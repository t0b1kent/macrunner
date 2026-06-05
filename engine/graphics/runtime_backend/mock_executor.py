from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
from typing import Iterable

from engine.graphics.d3d9_to_d3d11 import apply_d3d9_wvp, d3d9_effective_indices, d3d9_effective_vertex_indices
from engine.graphics.metal_ir.render_state import RenderResult, RenderState, Vertex

Color = tuple[int, int, int, int]


def _clamp_channel(value: float) -> int:
    return max(0, min(255, int(round(value))))


def _color_to_u8(color: Iterable[float | int]) -> Color:
    vals = list(color)
    if not vals:
        return (255, 255, 255, 255)
    out: list[int] = []
    for value in vals[:4]:
        if isinstance(value, float) and 0.0 <= value <= 1.0:
            out.append(_clamp_channel(value * 255.0))
        else:
            out.append(_clamp_channel(float(value)))
    while len(out) < 4:
        out.append(255)
    return (out[0], out[1], out[2], out[3])


class MockExecutor:
    """Deterministic CPU raster executor for MacRunner D3D bridge traces."""

    def __init__(self) -> None:
        self.backend = "mock"

    def execute(self, state: RenderState, output_dir: str | Path, *, trace_path: str | None = None, name: str = "render") -> RenderResult:
        out_dir = Path(output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        background = _color_to_u8(state.clear_color)
        pixels = [background for _ in range(state.width * state.height)]
        depth = [1.0 for _ in range(state.width * state.height)]

        state.pipeline.render_target_bound = True
        if state.topology:
            state.pipeline.primitive_topology = state.topology
        state.pipeline.vertex_buffer_bound = bool(state.vertex_buffer)
        state.pipeline.index_buffer_bound = bool(state.index_buffer)
        state.pipeline.vertex_shader = state.pipeline.vertex_shader or "fixture-vs"
        state.pipeline.pixel_shader = state.pipeline.pixel_shader or "fixture-ps"
        if state.vertex_buffer:
            errors = state.pipeline.validate_for_draw(indexed=False, require_shader=True)
            if errors:
                state.validation_errors.extend(errors)
            else:
                indexed_draw = bool(state.index_buffer)
                if state.api in {"d3d8", "d3d9"} and state.pipeline.metadata.get("d3d9_draw_range", {}).get("indexed") is False:
                    indexed_draw = False
                if indexed_draw:
                    indexed_errors = state.pipeline.validate_for_draw(indexed=True, require_shader=True)
                    if indexed_errors:
                        state.validation_errors.extend(indexed_errors)
                    else:
                        self._draw_indexed_triangles(state, pixels, depth)
                else:
                    indices = d3d9_effective_vertex_indices(len(state.vertex_buffer), state.pipeline.metadata, state.topology) if state.api in {"d3d8", "d3d9"} else list(range(len(state.vertex_buffer)))
                    self._draw_triangles(state, pixels, depth, indices)

        ppm_path = out_dir / f"{name}.ppm"
        self._write_ppm(ppm_path, state.width, state.height, pixels)
        rgb = bytes(channel for pixel in pixels for channel in pixel[:3])
        checksum = hashlib.sha256(rgb).hexdigest()
        non_bg = sum(1 for pixel in pixels if pixel != background)
        status = "PASS" if not state.validation_errors else "FAIL_VALIDATION"
        result = RenderResult(
            status=status,
            width=state.width,
            height=state.height,
            checksum=checksum,
            non_background_pixels=non_bg,
            unsupported_calls=state.unsupported_calls,
            validation_errors=list(state.validation_errors),
            warnings=list(state.warnings),
            ppm_path=str(ppm_path),
            backend=self.backend,
            api=state.api,
            trace_path=trace_path,
            present_count=state.present_count,
            frame_index=state.frame_index,
        )
        report_path = out_dir / f"{name}.report.json"
        report_path.write_text(json.dumps(result.to_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")
        result.report_path = str(report_path)
        return result

    def _draw_indexed_triangles(self, state: RenderState, pixels: list[Color], depth: list[float]) -> None:
        self._draw_triangles(state, pixels, depth, d3d9_effective_indices(state.index_buffer, state.pipeline.metadata, state.topology))

    def _draw_triangles(self, state: RenderState, pixels: list[Color], depth: list[float], indices: list[int]) -> None:
        if state.topology in {"trianglestrip", "D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP"}:
            strip_indices: list[int] = []
            for offset in range(0, len(indices) - 2):
                if offset % 2:
                    strip_indices.extend([indices[offset + 1], indices[offset], indices[offset + 2]])
                else:
                    strip_indices.extend([indices[offset], indices[offset + 1], indices[offset + 2]])
            indices = strip_indices
        elif state.topology not in {"trianglelist", "D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST"}:
            state.validation_errors.append("unsupported topology for mock raster")
            return
        for offset in range(0, len(indices) - 2, 3):
            try:
                tri = [state.vertex_buffer[indices[offset + i]] for i in range(3)]
            except IndexError:
                state.validation_errors.append("triangle index out of bounds")
                return
            self._raster_triangle(state, pixels, depth, tri)

    def _viewport(self, state: RenderState) -> tuple[int, int, int, int]:
        if state.viewport:
            return state.viewport
        return (0, 0, state.width, state.height)

    def _scissor(self, state: RenderState) -> tuple[int, int, int, int]:
        if state.api in {"d3d8", "d3d9"} and not self._d3d9_render_state_bool(state, "D3DRS_SCISSORTESTENABLE", False):
            return (0, 0, state.width, state.height)
        if state.scissor:
            return state.scissor
        return (0, 0, state.width, state.height)

    @staticmethod
    def _d3d9_render_state_bool(state: RenderState, key: str, default: bool = False) -> bool:
        value = state.pipeline.metadata.get("d3d9_render_states", {}).get(key, default)
        if isinstance(value, bool):
            return value
        if isinstance(value, int):
            return value != 0
        if isinstance(value, str):
            return value.strip().lower() in {"1", "true", "yes", "on"}
        return default

    def _to_screen(self, vertex: Vertex, viewport: tuple[int, int, int, int]) -> tuple[float, float]:
        vx, vy, vw, vh = viewport
        x, y, _z, w = vertex.position
        w = w or 1.0
        ndc_x = x / w
        ndc_y = y / w
        return (vx + (ndc_x + 1.0) * 0.5 * vw, vy + (1.0 - (ndc_y + 1.0) * 0.5) * vh)

    def _raster_triangle(self, state: RenderState, pixels: list[Color], depth: list[float], tri: list[Vertex]) -> None:
        viewport = self._viewport(state)
        sc_x, sc_y, sc_w, sc_h = self._scissor(state)
        transformed = [
            Vertex(
                position=apply_d3d9_wvp(v.position, state.pipeline.metadata),
                color=v.color,
                uv=v.uv,
            )
            for v in tri
        ]
        pts = [self._to_screen(v, viewport) for v in transformed]
        depths = [self._ndc_depth(v) for v in transformed]
        area = self._edge(pts[0], pts[1], pts[2])
        if self._culled_by_d3d9(state, area):
            return
        min_x = max(sc_x, int(min(p[0] for p in pts)))
        max_x = min(sc_x + sc_w - 1, int(max(p[0] for p in pts) + 1))
        min_y = max(sc_y, int(min(p[1] for p in pts)))
        max_y = min(sc_y + sc_h - 1, int(max(p[1] for p in pts) + 1))
        if abs(area) < 1e-6:
            state.validation_errors.append("degenerate triangle")
            return
        for y in range(max(0, min_y), min(state.height, max_y + 1)):
            for x in range(max(0, min_x), min(state.width, max_x + 1)):
                p = (x + 0.5, y + 0.5)
                w0 = self._edge(pts[1], pts[2], p) / area
                w1 = self._edge(pts[2], pts[0], p) / area
                w2 = self._edge(pts[0], pts[1], p) / area
                if w0 >= -1e-6 and w1 >= -1e-6 and w2 >= -1e-6:
                    index = y * state.width + x
                    pixel_depth = depths[0] * w0 + depths[1] * w1 + depths[2] * w2
                    if not self._passes_depth_test(state, pixel_depth, depth[index]):
                        continue
                    shaded = self._shade(state, transformed, (w0, w1, w2))
                    if shaded is not None:
                        pixels[index] = self._write_color_d3d9(state, shaded, pixels[index])
                        if self._depth_write_enabled(state):
                            depth[index] = pixel_depth

    @staticmethod
    def _edge(a: tuple[float, float], b: tuple[float, float], c: tuple[float, float]) -> float:
        return (c[0] - a[0]) * (b[1] - a[1]) - (c[1] - a[1]) * (b[0] - a[0])

    @staticmethod
    def _culled_by_d3d9(state: RenderState, area: float) -> bool:
        mode = str(state.pipeline.metadata.get("d3d9_render_states", {}).get("D3DRS_CULLMODE", "D3DCULL_NONE"))
        if mode == "D3DCULL_CW":
            return area < 0.0
        if mode == "D3DCULL_CCW":
            return area > 0.0
        return False

    @staticmethod
    def _ndc_depth(vertex: Vertex) -> float:
        _x, _y, z, w = vertex.position
        w = w or 1.0
        return max(0.0, min(1.0, z / w))

    @staticmethod
    def _depth_state(state: RenderState) -> dict:
        return state.pipeline.metadata.get("d3d9_depth_state", {})

    def _depth_enabled(self, state: RenderState) -> bool:
        return bool(self._depth_state(state).get("z_enable", False))

    def _depth_write_enabled(self, state: RenderState) -> bool:
        if not self._depth_enabled(state):
            return False
        return bool(self._depth_state(state).get("z_write_enable", True))

    def _passes_depth_test(self, state: RenderState, incoming: float, current: float) -> bool:
        if not self._depth_enabled(state):
            return True
        func = str(self._depth_state(state).get("z_func", "D3DCMP_LESSEQUAL"))
        if func == "D3DCMP_NEVER":
            return False
        if func == "D3DCMP_LESS":
            return incoming < current
        if func == "D3DCMP_EQUAL":
            return abs(incoming - current) < 1e-6
        if func == "D3DCMP_LESSEQUAL":
            return incoming <= current + 1e-6
        if func == "D3DCMP_GREATER":
            return incoming > current
        if func == "D3DCMP_NOTEQUAL":
            return abs(incoming - current) >= 1e-6
        if func == "D3DCMP_GREATEREQUAL":
            return incoming >= current - 1e-6
        return True

    def _shade(self, state: RenderState, tri: list[Vertex], weights: tuple[float, float, float]) -> Color | None:
        shader = state.shader or "vertex-color"
        if shader == "solid-color":
            return _color_to_u8(tri[0].color)
        diffuse = self._interpolate_diffuse(tri, weights)
        texture = self._sample_texture(state, tri, weights)
        if shader == "texture-sample" and texture is not None:
            return texture
        if shader in {"d3d9-fixed-function", "d3d9-programmable-texture-modulate"}:
            return self._shade_d3d9(state, diffuse, texture)
        return diffuse

    def _interpolate_diffuse(self, tri: list[Vertex], weights: tuple[float, float, float]) -> Color:
        channels: list[float] = []
        for channel in range(4):
            channels.append(sum(weights[i] * tri[i].color[channel] for i in range(3)))
        return _color_to_u8(channels)

    def _sample_texture(self, state: RenderState, tri: list[Vertex], weights: tuple[float, float, float]) -> Color | None:
        if not state.texture or not state.texture_size:
            return None
        u = sum(weights[i] * tri[i].uv[0] for i in range(3))
        v = sum(weights[i] * tri[i].uv[1] for i in range(3))
        width, height = state.texture_size
        sampler = state.pipeline.metadata.get("d3d9_sampler", {})
        address_u = str(sampler.get("address_u", "D3DTADDRESS_CLAMP"))
        address_v = str(sampler.get("address_v", "D3DTADDRESS_CLAMP"))
        u = self._address_coord(u, address_u)
        v = self._address_coord(v, address_v)
        texture_filter = str(sampler.get("mag_filter", sampler.get("min_filter", "D3DTEXF_POINT")))
        if texture_filter == "D3DTEXF_LINEAR":
            return self._sample_linear(state.texture, width, height, u, v, address_u, address_v)
        tx = max(0, min(width - 1, int(u * (width - 1) + 0.5)))
        ty = max(0, min(height - 1, int(v * (height - 1) + 0.5)))
        return _color_to_u8(state.texture[ty * width + tx])

    @staticmethod
    def _address_coord(value: float, mode: str) -> float:
        if mode == "D3DTADDRESS_WRAP":
            return value % 1.0
        if mode == "D3DTADDRESS_MIRROR":
            period = value % 2.0
            return period if period <= 1.0 else 2.0 - period
        return max(0.0, min(1.0, value))

    @staticmethod
    def _sample_linear(texture: list[Color], width: int, height: int, u: float, v: float, address_u: str, address_v: str) -> Color:
        x = u * (width - 1)
        y = v * (height - 1)
        x0 = max(0, min(width - 1, int(math.floor(x))))
        y0 = max(0, min(height - 1, int(math.floor(y))))
        if address_u == "D3DTADDRESS_WRAP":
            x1 = (x0 + 1) % width
        else:
            x1 = max(0, min(width - 1, x0 + 1))
        if address_v == "D3DTADDRESS_WRAP":
            y1 = (y0 + 1) % height
        else:
            y1 = max(0, min(height - 1, y0 + 1))
        fx = x - math.floor(x)
        fy = y - math.floor(y)
        c00 = texture[y0 * width + x0]
        c10 = texture[y0 * width + x1]
        c01 = texture[y1 * width + x0]
        c11 = texture[y1 * width + x1]
        out = []
        for channel in range(4):
            top = c00[channel] * (1.0 - fx) + c10[channel] * fx
            bottom = c01[channel] * (1.0 - fx) + c11[channel] * fx
            out.append(_clamp_channel(top * (1.0 - fy) + bottom * fy))
        return (out[0], out[1], out[2], out[3])

    def _shade_d3d9(self, state: RenderState, diffuse: Color, texture: Color | None) -> Color | None:
        ffp = state.pipeline.metadata.get("d3d9_ffp_shader", {})
        if state.shader == "d3d9-programmable-texture-modulate":
            shaded = self._modulate(texture or (255, 255, 255, 255), diffuse)
            return shaded if self._passes_alpha_test(ffp, shaded[3]) else None

        texture_factor = _color_to_u8(ffp.get("texture_factor", [255, 255, 255, 255]))
        color_arg1 = self._resolve_d3d9_arg(str(ffp.get("color_arg1", "D3DTA_DIFFUSE")), diffuse, texture, texture_factor)
        color_arg2 = self._resolve_d3d9_arg(str(ffp.get("color_arg2", "D3DTA_TEXTURE")), diffuse, texture, texture_factor)
        color_op = str(ffp.get("color_op", "D3DTOP_SELECTARG1"))
        alpha_arg1 = self._resolve_d3d9_arg(str(ffp.get("alpha_arg1", "D3DTA_DIFFUSE")), diffuse, texture, texture_factor)
        alpha_arg2 = self._resolve_d3d9_arg(str(ffp.get("alpha_arg2", "D3DTA_TEXTURE")), diffuse, texture, texture_factor)
        alpha_op = str(ffp.get("alpha_op", "D3DTOP_SELECTARG1"))
        rgb = self._apply_d3d9_op(color_op, color_arg1, color_arg2, diffuse, texture, texture_factor)
        alpha = self._apply_d3d9_op(alpha_op, alpha_arg1, alpha_arg2, diffuse, texture, texture_factor)[3]
        shaded = (rgb[0], rgb[1], rgb[2], alpha)
        return shaded if self._passes_alpha_test(ffp, alpha) else None

    @staticmethod
    def _resolve_d3d9_arg(arg: str, diffuse: Color, texture: Color | None, texture_factor: Color) -> Color:
        complement = "D3DTA_COMPLEMENT" in arg
        alpha_replicate = "D3DTA_ALPHAREPLICATE" in arg
        normalized = (
            arg.replace("D3DTA_", "")
            .replace("|COMPLEMENT", "")
            .replace("|ALPHAREPLICATE", "")
            .replace("|D3DTA_COMPLEMENT", "")
            .replace("|D3DTA_ALPHAREPLICATE", "")
        )
        if normalized == "TEXTURE":
            value = texture or (255, 255, 255, 255)
        elif normalized == "TFACTOR":
            value = texture_factor
        elif normalized == "CURRENT":
            value = texture or diffuse
        else:
            value = diffuse
        if complement:
            value = tuple(255 - channel for channel in value)  # type: ignore[assignment]
        if alpha_replicate:
            value = (value[3], value[3], value[3], value[3])
        return value

    def _apply_d3d9_op(self, op: str, lhs: Color, rhs: Color, diffuse: Color, texture: Color | None, texture_factor: Color = (255, 255, 255, 255)) -> Color:
        if op == "D3DTOP_SELECTARG2":
            return rhs
        if op == "D3DTOP_MODULATE":
            return self._modulate(lhs, rhs)
        if op == "D3DTOP_MODULATE2X":
            return tuple(min(255, _clamp_channel(lhs[i] * rhs[i] * 2.0 / 255.0)) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_ADD":
            return tuple(min(255, lhs[i] + rhs[i]) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_ADDSIGNED":
            return tuple(_clamp_channel(lhs[i] + rhs[i] - 128) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_ADDSIGNED2X":
            return tuple(_clamp_channel((lhs[i] + rhs[i] - 128) * 2) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_ADDSMOOTH":
            return tuple(_clamp_channel(lhs[i] + rhs[i] - lhs[i] * rhs[i] / 255.0) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_DOTPRODUCT3":
            dot = sum(((lhs[i] / 127.5) - 1.0) * ((rhs[i] / 127.5) - 1.0) for i in range(3))
            value = _clamp_channel(max(0.0, min(1.0, dot)) * 255.0)
            return (value, value, value, lhs[3])
        if op == "D3DTOP_SUBTRACT":
            return tuple(max(0, lhs[i] - rhs[i]) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_BLENDDIFFUSEALPHA":
            alpha = diffuse[3] / 255.0
            return tuple(_clamp_channel(lhs[i] * alpha + rhs[i] * (1.0 - alpha)) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_BLENDTEXTUREALPHA":
            alpha = (texture or (255, 255, 255, 255))[3] / 255.0
            return tuple(_clamp_channel(lhs[i] * alpha + rhs[i] * (1.0 - alpha)) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_BLENDFACTORALPHA":
            alpha = texture_factor[3] / 255.0
            return tuple(_clamp_channel(lhs[i] * alpha + rhs[i] * (1.0 - alpha)) for i in range(4))  # type: ignore[return-value]
        if op == "D3DTOP_DISABLE":
            return lhs
        return lhs

    @staticmethod
    def _modulate(lhs: Color, rhs: Color) -> Color:
        return tuple(_clamp_channel(lhs[i] * rhs[i] / 255.0) for i in range(4))  # type: ignore[return-value]

    @staticmethod
    def _passes_alpha_test(ffp: dict, alpha: int) -> bool:
        if not ffp.get("alpha_test_enable", False):
            return True
        ref = int(ffp.get("alpha_ref", 0))
        func = str(ffp.get("alpha_func", "D3DCMP_ALWAYS"))
        if func == "D3DCMP_NEVER":
            return False
        if func == "D3DCMP_LESS":
            return alpha < ref
        if func == "D3DCMP_EQUAL":
            return alpha == ref
        if func == "D3DCMP_LESSEQUAL":
            return alpha <= ref
        if func == "D3DCMP_GREATER":
            return alpha > ref
        if func == "D3DCMP_NOTEQUAL":
            return alpha != ref
        if func == "D3DCMP_GREATEREQUAL":
            return alpha >= ref
        return True

    def _write_color_d3d9(self, state: RenderState, src: Color, dst: Color) -> Color:
        ffp = state.pipeline.metadata.get("d3d9_ffp_shader", {})
        render_states = state.pipeline.metadata.get("d3d9_render_states", {})
        if not (ffp.get("alpha_blend_enable", False) or render_states.get("D3DRS_ALPHABLENDENABLE", False)):
            return self._apply_color_write_mask(render_states, src, dst)
        src_factor = self._blend_factor(str(ffp.get("src_blend", render_states.get("D3DRS_SRCBLEND", "D3DBLEND_ONE"))), src, dst)
        dst_factor = self._blend_factor(str(ffp.get("dest_blend", render_states.get("D3DRS_DESTBLEND", "D3DBLEND_ZERO"))), src, dst)
        blended = tuple(
            _clamp_channel(src[i] * src_factor[i] + dst[i] * dst_factor[i])
            for i in range(4)
        )
        return self._apply_color_write_mask(render_states, blended, dst)  # type: ignore[arg-type]

    @classmethod
    def _apply_color_write_mask(cls, render_states: dict, src: Color, dst: Color) -> Color:
        mask = cls._color_write_mask(render_states.get("D3DRS_COLORWRITEENABLE", 0x0f))
        return tuple(src[i] if mask[i] else dst[i] for i in range(4))  # type: ignore[return-value]

    @staticmethod
    def _color_write_mask(value: object) -> tuple[bool, bool, bool, bool]:
        bits = 0x0f
        if isinstance(value, bool):
            bits = 0x0f if value else 0
        elif isinstance(value, int):
            bits = value
        elif isinstance(value, str):
            text = value.strip()
            if text.lower().startswith("0x") or text.isdigit():
                bits = int(text, 0)
            else:
                bits = 0
                for name, bit in {
                    "RED": 0x1,
                    "GREEN": 0x2,
                    "BLUE": 0x4,
                    "ALPHA": 0x8,
                }.items():
                    if name in text:
                        bits |= bit
        return (
            bool(bits & 0x1),
            bool(bits & 0x2),
            bool(bits & 0x4),
            bool(bits & 0x8),
        )

    @staticmethod
    def _blend_factor(blend: str, src: Color, dst: Color) -> tuple[float, float, float, float]:
        if blend == "D3DBLEND_ZERO":
            return (0.0, 0.0, 0.0, 0.0)
        if blend == "D3DBLEND_SRCALPHA":
            a = src[3] / 255.0
            return (a, a, a, a)
        if blend == "D3DBLEND_INVSRCALPHA":
            a = 1.0 - src[3] / 255.0
            return (a, a, a, a)
        if blend == "D3DBLEND_DESTALPHA":
            a = dst[3] / 255.0
            return (a, a, a, a)
        if blend == "D3DBLEND_INVDESTALPHA":
            a = 1.0 - dst[3] / 255.0
            return (a, a, a, a)
        if blend == "D3DBLEND_SRCCOLOR":
            return tuple(channel / 255.0 for channel in src)  # type: ignore[return-value]
        if blend == "D3DBLEND_INVSRCCOLOR":
            return tuple(1.0 - channel / 255.0 for channel in src)  # type: ignore[return-value]
        if blend == "D3DBLEND_DESTCOLOR":
            return tuple(channel / 255.0 for channel in dst)  # type: ignore[return-value]
        if blend == "D3DBLEND_INVDESTCOLOR":
            return tuple(1.0 - channel / 255.0 for channel in dst)  # type: ignore[return-value]
        return (1.0, 1.0, 1.0, 1.0)

    @staticmethod
    def _write_ppm(path: Path, width: int, height: int, pixels: list[Color]) -> None:
        with path.open("wb") as handle:
            handle.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
            handle.write(bytes(channel for pixel in pixels for channel in pixel[:3]))
