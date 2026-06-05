from __future__ import annotations

from typing import Any

from engine.graphics.metal_ir.render_state import RenderState, Vertex


D3D9_FORMATS = {
    "D3DFMT_A8R8G8B8": "bgra8",
    "D3DFMT_X8R8G8B8": "bgra8",
    "D3DFMT_A2R10G10B10": "rgba10a2",
    "D3DFMT_A2B10G10R10": "bgra10a2",
    "D3DFMT_R8G8B8": "rgba8",
    "D3DFMT_A1R5G5B5": "rgba8",
    "D3DFMT_X1R5G5B5": "rgba8",
    "D3DFMT_R5G6B5": "rgba8",
    "D3DFMT_A4R4G4B4": "rgba4",
    "D3DFMT_X4R4G4B4": "rgba4",
    "D3DFMT_A8R3G3B2": "rgba8",
    "D3DFMT_A8": "a8",
    "D3DFMT_L8": "r8",
    "D3DFMT_L16": "r16",
    "D3DFMT_A8L8": "rg8",
    "D3DFMT_V8U8": "rg8_snorm",
    "D3DFMT_Q8W8V8U8": "rgba8_snorm",
    "D3DFMT_V16U16": "rg16_snorm",
    "D3DFMT_DXT1": "bc1",
    "D3DFMT_DXT2": "bc2",
    "D3DFMT_DXT3": "bc2",
    "D3DFMT_DXT4": "bc3",
    "D3DFMT_DXT5": "bc3",
    "D3DFMT_R16F": "r16f",
    "D3DFMT_G16R16F": "rg16f",
    "D3DFMT_A16B16G16R16F": "rgba16f",
    "D3DFMT_R32F": "r32f",
    "D3DFMT_G32R32F": "rg32f",
    "D3DFMT_A32B32G32R32F": "rgba32f",
    "D3DFMT_G16R16": "rg16",
    "D3DFMT_A16B16G16R16": "rgba16",
    "D3DFMT_D16": "d16",
    "D3DFMT_D15S1": "d15s1",
    "D3DFMT_D24X8": "d24x8",
    "D3DFMT_D24S8": "d24s8",
    "D3DFMT_D24X4S4": "d24x4s4",
    "D3DFMT_D32": "d32",
    "D3DFMT_D32F_LOCKABLE": "d32f",
    "D3DFMT_INTZ": "d24s8",
    "D3DFMT_NULL": "null",
}

D3D9_TOPOLOGIES = {
    "D3DPT_TRIANGLELIST": "trianglelist",
    "D3DPT_TRIANGLESTRIP": "trianglestrip",
    "D3DPT_LINELIST": "linelist",
    "D3DPT_POINTLIST": "pointlist",
}


def _identity_matrix() -> list[float]:
    return [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]


def _matrix(value: Any) -> list[float]:
    if value is None:
        return _identity_matrix()
    if isinstance(value, dict):
        value = value.get("m", value.get("matrix", []))
    if isinstance(value, list) and len(value) == 4 and all(isinstance(row, list) for row in value):
        flat = [float(channel) for row in value for channel in row]
    else:
        flat = [float(channel) for channel in list(value)]
    if len(flat) != 16:
        raise ValueError("D3D9 transform matrix must contain 16 values")
    return flat


def _matrix_multiply(lhs: list[float], rhs: list[float]) -> list[float]:
    out = [0.0] * 16
    for row in range(4):
        for col in range(4):
            out[row * 4 + col] = sum(lhs[row * 4 + k] * rhs[k * 4 + col] for k in range(4))
    return out


def _update_wvp_metadata(state: RenderState) -> None:
    transforms = state.pipeline.metadata.get("d3d9_transforms", {})
    if not transforms:
        state.pipeline.metadata.pop("d3d9_wvp_matrix", None)
        return
    world = transforms.get("D3DTS_WORLD", _identity_matrix())
    view = transforms.get("D3DTS_VIEW", _identity_matrix())
    projection = transforms.get("D3DTS_PROJECTION", _identity_matrix())
    state.pipeline.metadata["d3d9_wvp_matrix"] = _matrix_multiply(_matrix_multiply(world, view), projection)


def _update_sampler_metadata(state: RenderState) -> None:
    entries = state.pipeline.metadata.get("d3d9_sampler_states", [])
    sampler0 = {
        str(entry.get("state")): entry.get("value")
        for entry in entries
        if int(entry.get("sampler", 0)) == 0
    }
    state.pipeline.metadata["d3d9_sampler"] = {
        "address_u": str(sampler0.get("D3DSAMP_ADDRESSU", "D3DTADDRESS_CLAMP")),
        "address_v": str(sampler0.get("D3DSAMP_ADDRESSV", "D3DTADDRESS_CLAMP")),
        "min_filter": str(sampler0.get("D3DSAMP_MINFILTER", "D3DTEXF_POINT")),
        "mag_filter": str(sampler0.get("D3DSAMP_MAGFILTER", "D3DTEXF_POINT")),
        "mip_filter": str(sampler0.get("D3DSAMP_MIPFILTER", "D3DTEXF_NONE")),
    }


def _bool_state(value: Any, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.upper() not in {"FALSE", "0", "D3DZB_FALSE", "D3D_FALSE"}
    return bool(value)


def _update_depth_metadata(state: RenderState) -> None:
    render_states = state.pipeline.metadata.get("d3d9_render_states", {})
    depth = {
        "z_enable": _bool_state(render_states.get("D3DRS_ZENABLE", False)),
        "z_write_enable": _bool_state(render_states.get("D3DRS_ZWRITEENABLE", True), True),
        "z_func": str(render_states.get("D3DRS_ZFUNC", "D3DCMP_LESSEQUAL")),
        "format": state.depth_format,
    }
    state.pipeline.metadata["d3d9_depth_state"] = depth
    state.pipeline.depth_target_bound = bool(depth["z_enable"] or state.depth_format)


def apply_d3d9_wvp(position: tuple[float, float, float, float], metadata: dict[str, Any]) -> tuple[float, float, float, float]:
    matrix = metadata.get("d3d9_wvp_matrix")
    if not matrix:
        return position
    x, y, z, w = position
    values = [x, y, z, w]
    return tuple(sum(values[row] * float(matrix[row * 4 + col]) for row in range(4)) for col in range(4))  # type: ignore[return-value]


def d3d9_effective_indices(index_buffer: list[int], metadata: dict[str, Any], topology: str | None) -> list[int]:
    draw = metadata.get("d3d9_draw_range")
    if not draw:
        return list(index_buffer)
    if draw.get("indexed") is False:
        return []
    start_index = int(draw.get("start_index", 0))
    primitive_count = int(draw.get("primitive_count", 0))
    base_vertex = int(draw.get("base_vertex_index", 0))
    if topology in {"trianglestrip", "D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP"}:
        count = primitive_count + 2
    else:
        count = primitive_count * 3
    if count <= 0:
        return []
    return [base_vertex + index for index in index_buffer[start_index:start_index + count]]


def d3d9_effective_vertex_indices(vertex_count: int, metadata: dict[str, Any], topology: str | None) -> list[int]:
    draw = metadata.get("d3d9_draw_range")
    if not draw or draw.get("indexed") is not False:
        return list(range(vertex_count))
    start_vertex = int(draw.get("start_vertex", 0))
    primitive_count = int(draw.get("primitive_count", 0))
    if topology in {"trianglestrip", "D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP"}:
        count = primitive_count + 2
    else:
        count = primitive_count * 3
    if count <= 0:
        return []
    return list(range(start_vertex, min(vertex_count, start_vertex + count)))


def _color(value: Any) -> tuple[int, int, int, int]:
    if isinstance(value, int):
        argb = value
        return (
            (argb >> 16) & 0xff,
            (argb >> 8) & 0xff,
            argb & 0xff,
            (argb >> 24) & 0xff,
        )
    if isinstance(value, str):
        value = value.strip()
        if value.startswith("0x"):
            argb = int(value, 16)
            return (
                (argb >> 16) & 0xff,
                (argb >> 8) & 0xff,
                argb & 0xff,
                (argb >> 24) & 0xff,
            )
    vals = list(value)
    while len(vals) < 4:
        vals.append(255)
    out: list[int] = []
    for channel in vals[:4]:
        if isinstance(channel, float) and 0.0 <= channel <= 1.0:
            out.append(int(channel * 255))
        else:
            out.append(int(channel))
    return (out[0], out[1], out[2], out[3])


def _vertex(payload: dict[str, Any]) -> Vertex:
    return Vertex(
        position=tuple(float(x) for x in payload.get("position", [0, 0, 0, 1])),
        color=tuple(float(x) for x in payload.get("color", [1, 1, 1, 1])),
        uv=tuple(float(x) for x in payload.get("uv", [0, 0])),
    )


def _fvf_layout(fvf: Any) -> list[str]:
    if isinstance(fvf, list):
        return [str(part).lower() for part in fvf]
    if isinstance(fvf, str):
        return [part.strip().lower() for part in fvf.split("|") if part.strip()]
    if isinstance(fvf, int):
        layout = []
        if fvf & 0x002:
            layout.append("xyz")
        if fvf & 0x040:
            layout.append("diffuse")
        if fvf & 0x100:
            layout.append("tex1")
        return layout
    return []


def _bind_fixed_function_shader(state: RenderState) -> None:
    state.shader = state.shader or "d3d9-fixed-function"
    state.pipeline.vertex_shader = state.pipeline.vertex_shader or "d3d9-fixed-function-vs"
    state.pipeline.pixel_shader = state.pipeline.pixel_shader or "d3d9-fixed-function-ps"
    state.pipeline.metadata["translation_target"] = "d3d11"
    state.pipeline.metadata["fixed_function"] = True


def _update_ffp_shader_metadata(state: RenderState) -> None:
    stages = state.pipeline.metadata.get("d3d9_texture_stage_states", [])
    stage0 = {
        str(entry.get("state")): entry.get("value")
        for entry in stages
        if int(entry.get("stage", 0)) == 0
    }
    color_op = str(stage0.get("D3DTSS_COLOROP", "D3DTOP_SELECTARG1"))
    color_arg1 = str(stage0.get("D3DTSS_COLORARG1", "D3DTA_DIFFUSE"))
    color_arg2 = str(stage0.get("D3DTSS_COLORARG2", "D3DTA_TEXTURE"))
    alpha_op = str(stage0.get("D3DTSS_ALPHAOP", "D3DTOP_SELECTARG1"))
    alpha_arg1 = str(stage0.get("D3DTSS_ALPHAARG1", color_arg1))
    alpha_arg2 = str(stage0.get("D3DTSS_ALPHAARG2", color_arg2))
    render_states = state.pipeline.metadata.get("d3d9_render_states", {})
    state.pipeline.metadata["d3d9_ffp_shader"] = {
        "color_op": color_op,
        "color_arg1": color_arg1,
        "color_arg2": color_arg2,
        "alpha_op": alpha_op,
        "alpha_arg1": alpha_arg1,
        "alpha_arg2": alpha_arg2,
        "texture_factor": _color(render_states.get("D3DRS_TEXTUREFACTOR", [255, 255, 255, 255])),
        "alpha_test_enable": bool(render_states.get("D3DRS_ALPHATESTENABLE", False)),
        "alpha_ref": int(render_states.get("D3DRS_ALPHAREF", 0)),
        "alpha_func": str(render_states.get("D3DRS_ALPHAFUNC", "D3DCMP_ALWAYS")),
        "alpha_blend_enable": bool(render_states.get("D3DRS_ALPHABLENDENABLE", False)),
        "src_blend": str(render_states.get("D3DRS_SRCBLEND", "D3DBLEND_ONE")),
        "dest_blend": str(render_states.get("D3DRS_DESTBLEND", "D3DBLEND_ZERO")),
        "wvp_matrix": state.pipeline.metadata.get("d3d9_wvp_matrix"),
        "depth": state.pipeline.metadata.get("d3d9_depth_state", {}),
    }


def apply_d3d9_event(state: RenderState, command: str, payload: dict[str, Any]) -> bool:
    if command in {"create_device", "reset", "create_swapchain"}:
        state.width = int(payload.get("width", state.width))
        state.height = int(payload.get("height", state.height))
        state.render_target_format = D3D9_FORMATS.get(
            str(payload.get("backbuffer_format", "D3DFMT_A8R8G8B8")),
            state.render_target_format,
        )
        state.pipeline.render_target_format = state.render_target_format
        state.pipeline.render_target_bound = True
        state.pipeline.swapchain_backbuffer_bound = command == "create_swapchain"
        state.pipeline.metadata["d3d9_present_parameters"] = payload
        return True

    if command in {"clear", "clear_render_target"}:
        state.clear_color = _color(payload.get("color", [0, 0, 0, 255]))
        state.pipeline.render_target_bound = True
        return True

    if command == "set_viewport":
        state.viewport = tuple(int(payload[k]) for k in ("x", "y", "width", "height"))
        state.pipeline.viewport_set = True
        return True

    if command == "set_scissor":
        state.scissor = tuple(int(payload[k]) for k in ("x", "y", "width", "height"))
        state.pipeline.scissor_set = True
        return True

    if command == "set_fvf":
        state.pipeline.input_layout = _fvf_layout(payload.get("fvf", []))
        _bind_fixed_function_shader(state)
        return True

    if command == "set_render_state":
        state.pipeline.metadata.setdefault("d3d9_render_states", {})[
            str(payload.get("state"))
        ] = payload.get("value")
        if str(payload.get("state")) in {
            "D3DRS_TEXTUREFACTOR",
            "D3DRS_ALPHATESTENABLE",
            "D3DRS_ALPHAREF",
            "D3DRS_ALPHAFUNC",
            "D3DRS_ALPHABLENDENABLE",
            "D3DRS_SRCBLEND",
            "D3DRS_DESTBLEND",
            "D3DRS_ZENABLE",
            "D3DRS_ZWRITEENABLE",
            "D3DRS_ZFUNC",
        }:
            _bind_fixed_function_shader(state)
            if str(payload.get("state")) in {"D3DRS_ZENABLE", "D3DRS_ZWRITEENABLE", "D3DRS_ZFUNC"}:
                _update_depth_metadata(state)
            _update_ffp_shader_metadata(state)
        return True

    if command == "set_texture_stage_state":
        state.pipeline.metadata.setdefault("d3d9_texture_stage_states", []).append(payload)
        _bind_fixed_function_shader(state)
        _update_ffp_shader_metadata(state)
        return True

    if command == "set_sampler_state":
        state.pipeline.metadata.setdefault("d3d9_sampler_states", []).append(payload)
        _update_sampler_metadata(state)
        return True

    if command == "set_transform":
        try:
            matrix = _matrix(payload.get("matrix"))
        except ValueError as exc:
            state.validation_errors.append(str(exc))
            return True
        state.pipeline.metadata.setdefault("d3d9_transforms", {})[
            str(payload.get("state"))
        ] = matrix
        _bind_fixed_function_shader(state)
        _update_wvp_metadata(state)
        _update_ffp_shader_metadata(state)
        return True

    if command in {"set_stream_source", "set_vertex_buffer"}:
        state.vertex_buffer = [_vertex(vertex) for vertex in payload.get("vertices", [])]
        state.pipeline.vertex_buffer_bound = bool(state.vertex_buffer)
        return True

    if command in {"set_indices", "set_index_buffer"}:
        state.index_buffer = [int(index) for index in payload.get("indices", [])]
        state.pipeline.index_buffer_bound = bool(state.index_buffer)
        index_format = str(payload.get("format", "D3DFMT_INDEX16"))
        state.pipeline.metadata["d3d9_index_format_raw"] = index_format
        state.pipeline.metadata["d3d9_index_format"] = "uint32" if index_format == "D3DFMT_INDEX32" else "uint16"
        return True

    if command == "set_texture":
        state.texture_size = (int(payload["width"]), int(payload["height"]))
        state.pipeline.metadata["d3d9_texture_format"] = D3D9_FORMATS.get(
            str(payload.get("format", "D3DFMT_A8R8G8B8")),
            str(payload.get("format", "unknown")).lower(),
        )
        state.texture = [_color(pixel) for pixel in payload.get("pixels", [])]
        state.pipeline.shader_resource_bound = True
        _bind_fixed_function_shader(state)
        _update_ffp_shader_metadata(state)
        return True

    if command == "set_vertex_shader":
        state.pipeline.vertex_shader = str(payload.get("shader", "d3d9-programmable-vs"))
        state.pipeline.metadata["translation_target"] = "d3d11"
        state.pipeline.metadata["programmable"] = True
        return True

    if command == "set_pixel_shader":
        state.shader = str(payload.get("emulation", "d3d9-programmable-texture-modulate"))
        state.pipeline.pixel_shader = str(payload.get("shader", "d3d9-programmable-ps"))
        state.pipeline.metadata["translation_target"] = "d3d11"
        state.pipeline.metadata["programmable"] = True
        return True

    if command == "create_depth_stencil":
        state.depth_format = D3D9_FORMATS.get(
            str(payload.get("format", "D3DFMT_D24S8")),
            "d24s8",
        )
        state.pipeline.depth_format = state.depth_format
        state.pipeline.depth_target_bound = True
        _update_depth_metadata(state)
        _update_ffp_shader_metadata(state)
        return True

    if command in {"draw_primitive", "draw_indexed_primitive"}:
        state.topology = D3D9_TOPOLOGIES.get(
            str(payload.get("primitive_type", "D3DPT_TRIANGLELIST")),
            "trianglelist",
        )
        if command == "draw_indexed_primitive":
            state.pipeline.metadata["d3d9_draw_range"] = {
                "indexed": True,
                "base_vertex_index": int(payload.get("base_vertex_index", 0)),
                "min_vertex_index": int(payload.get("min_vertex_index", 0)),
                "num_vertices": int(payload.get("num_vertices", len(state.vertex_buffer))),
                "start_index": int(payload.get("start_index", 0)),
                "primitive_count": int(payload.get("primitive_count", 0)),
            }
        else:
            state.pipeline.metadata["d3d9_draw_range"] = {
                "indexed": False,
                "start_vertex": int(payload.get("start_vertex", 0)),
                "primitive_count": int(payload.get("primitive_count", 0)),
            }
        if not state.pipeline.metadata.get("programmable"):
            _bind_fixed_function_shader(state)
            _update_ffp_shader_metadata(state)
        return True

    if command in {"begin_scene", "end_scene"}:
        state.pipeline.metadata[command] = True
        return True

    if command == "present":
        state.present_count += 1
        state.frame_index += 1
        state.pipeline.swapchain_backbuffer_bound = True
        return True

    return False
