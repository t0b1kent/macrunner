from pathlib import Path

from engine.graphics.d3d9_to_d3d11 import D3D9_FORMATS
from engine.graphics.runtime_backend.mock_executor import MockExecutor
from engine.graphics.tools.d3d_trace_replay import load_trace, replay

ROOT = Path(__file__).resolve().parents[1]
TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_triangle_runtime.jsonl"
D3D8_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_fixed_function_runtime.jsonl"
D3D8_STRIP_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_triangle_strip_runtime.jsonl"
D3D8_FOG_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_fog_runtime.jsonl"
D3D8_ALPHA_TEST_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_alpha_test_runtime.jsonl"
D3D8_ALPHA_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_alpha_blend_runtime.jsonl"
D3D8_DEPTH_CULL_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_depth_cull_runtime.jsonl"
D3D8_SHADEMODE_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_shademode_flat_runtime.jsonl"
D3D8_TEXTURE_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_texture_blend_runtime.jsonl"
TEXTURE_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_texture_modulate_runtime.jsonl"
TEXTURE_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_texture_blend_runtime.jsonl"
FFP_ARG_MODIFIER_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_arg_modifier_runtime.jsonl"
FFP_ADDSIGNED_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_addsigned_runtime.jsonl"
FFP_BLENDFACTOR_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_blendfactoralpha_runtime.jsonl"
FFP_BLENDCURRENT_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_blendcurrentalpha_runtime.jsonl"
FFP_BLENDDIFFUSE_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_blenddiffusealpha_runtime.jsonl"
FFP_DOTPRODUCT_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_dotproduct3_runtime.jsonl"
FFP_MODULATE4X_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_modulate4x_runtime.jsonl"
FFP_MULTIPLYADD_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_multiplyadd_runtime.jsonl"
FFP_LERP_TRACE = ROOT / "traces/runtime_samples/d3d9_ffp_lerp_runtime.jsonl"
TRANSFORM_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_transform_runtime.jsonl"
INDEX32_TRACE = ROOT / "traces/runtime_samples/d3d9_index32_triangle_runtime.jsonl"
SAMPLER_TRACE = ROOT / "traces/runtime_samples/d3d9_sampler_linear_wrap_runtime.jsonl"
STRIP_TRACE = ROOT / "traces/runtime_samples/d3d9_triangle_strip_runtime.jsonl"
SCISSOR_TRACE = ROOT / "traces/runtime_samples/d3d9_scissor_test_runtime.jsonl"
DRAW_STRIP_TRACE = ROOT / "traces/runtime_samples/d3d9_drawprimitive_triangle_strip_runtime.jsonl"
XNA_TRACE = ROOT / "traces/runtime_samples/d3d9_xna_programmable_sprite_runtime.jsonl"
ALPHA_TEST_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_alpha_test_runtime.jsonl"
FOG_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_fog_runtime.jsonl"
BASE_VERTEX_TRACE = ROOT / "traces/runtime_samples/d3d9_base_vertex_runtime.jsonl"
COLOR_WRITE_TRACE = ROOT / "traces/runtime_samples/d3d9_color_write_mask_runtime.jsonl"
CULL_TRACE = ROOT / "traces/runtime_samples/d3d9_cullmode_runtime.jsonl"
DEPTH_TRACE = ROOT / "traces/runtime_samples/d3d9_depth_test_runtime.jsonl"
DRAW_RANGE_TRACE = ROOT / "traces/runtime_samples/d3d9_drawprimitive_range_runtime.jsonl"
INDEX_RANGE_TRACE = ROOT / "traces/runtime_samples/d3d9_indexed_range_runtime.jsonl"
ALPHA_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d9_xna_alpha_blend_sprite_runtime.jsonl"
FORMAT_SWEEP_TRACE = ROOT / "traces/runtime_samples/d3d9_format_sweep_runtime.jsonl"
LEGACY_FORMAT_TRACE = ROOT / "traces/runtime_samples/d3d9_legacy_format_expansion_runtime.jsonl"


def _ppm_pixel(path: Path, x: int, y: int, width: int) -> tuple[int, int, int]:
    data = path.read_bytes()
    header_end = data.find(b"\n255\n") + len(b"\n255\n")
    offset = header_end + (y * width + x) * 3
    return (data[offset], data[offset + 1], data[offset + 2])


def test_d3d9_fixed_function_trace_translates_to_d3d11_state(tmp_path):
    result = replay(TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d9"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1000
    assert result["unsupported_calls"] == 0
    assert result["validation_errors"] == []


def test_d3d9_translation_records_fixed_function_metadata():
    state = load_trace(TRACE)
    assert state.pipeline.metadata["translation_target"] == "d3d11"
    assert state.pipeline.metadata["fixed_function"] is True
    assert state.pipeline.input_layout == ["xyz", "diffuse", "tex1"]
    assert state.pipeline.metadata["d3d9_render_states"]["D3DRS_LIGHTING"] is False
    assert state.pipeline.metadata["d3d9_texture_stage_states"][0]["state"] == "D3DTSS_COLOROP"


def test_d3d8_renderware_trace_uses_d3d9_translation_path(tmp_path):
    result = replay(D3D8_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d8"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 900
    assert result["unsupported_calls"] == 0

    state = load_trace(D3D8_TRACE)
    assert state.pipeline.metadata["translation_target"] == "d3d11"
    assert state.pipeline.metadata["d3d9_wvp_matrix"][12] == -0.2
    assert state.pipeline.metadata["d3d9_sampler"]["address_u"] == "D3DTADDRESS_CLAMP"
    assert state.pipeline.metadata["d3d9_texture_format"] == "bgra8"


def test_d3d8_renderware_triangle_strip_uses_legacy_topology_path(tmp_path):
    result = replay(D3D8_STRIP_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d8"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 2000
    assert result["unsupported_calls"] == 0

    state = load_trace(D3D8_STRIP_TRACE)
    assert state.topology == "trianglestrip"
    assert state.pipeline.metadata["translation_target"] == "d3d11"
    assert state.pipeline.metadata["d3d9_draw_range"]["primitive_count"] == 2
    assert state.pipeline.metadata["d3d9_texture_format"] == "bgra8"


def test_d3d8_renderware_fog_uses_legacy_render_state_path(tmp_path):
    result = replay(D3D8_FOG_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d8"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (128, 128, 0)

    state = load_trace(D3D8_FOG_TRACE)
    fog = state.pipeline.metadata["d3d9_fog_state"]
    assert fog["enable"] is True
    assert fog["color"] == (255, 0, 0, 255)
    assert fog["vertex_mode"] == "D3DFOG_LINEAR"
    assert fog["start"] == 0.0
    assert fog["end"] == 1.0
    assert state.pipeline.metadata["d3d9_ffp_shader"]["fog"] == fog


def test_d3d8_renderware_alpha_test_uses_legacy_alpha_state_path(tmp_path):
    result = replay(D3D8_ALPHA_TEST_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d8"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 500

    state = load_trace(D3D8_ALPHA_TEST_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert ffp["alpha_test_enable"] is True
    assert ffp["alpha_ref"] == 128
    assert ffp["alpha_func"] == "D3DCMP_GREATER"


def test_d3d8_renderware_alpha_blend_uses_legacy_output_merger_path(tmp_path):
    result = replay(D3D8_ALPHA_BLEND_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d8"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (128, 0, 127)

    state = load_trace(D3D8_ALPHA_BLEND_TRACE)
    render_states = state.pipeline.metadata["d3d9_render_states"]
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert render_states["D3DRS_ALPHABLENDENABLE"] is True
    assert render_states["D3DRS_SRCBLEND"] == "D3DBLEND_SRCALPHA"
    assert render_states["D3DRS_DESTBLEND"] == "D3DBLEND_INVSRCALPHA"
    assert ffp["alpha_blend_enable"] is True
    assert ffp["src_blend"] == "D3DBLEND_SRCALPHA"
    assert ffp["dest_blend"] == "D3DBLEND_INVSRCALPHA"


def test_d3d8_renderware_depth_cull_uses_legacy_render_state_path(tmp_path):
    result = replay(D3D8_DEPTH_CULL_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d8"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (255, 25, 25)
    assert _ppm_pixel(Path(result["ppm_path"]), 50, 40, result["width"]) == (4, 8, 16)

    state = load_trace(D3D8_DEPTH_CULL_TRACE)
    render_states = state.pipeline.metadata["d3d9_render_states"]
    depth = state.pipeline.metadata["d3d9_depth_state"]
    assert render_states["D3DRS_CULLMODE"] == "D3DCULL_CW"
    assert depth["z_enable"] is True
    assert depth["z_write_enable"] is False
    assert depth["z_func"] == "D3DCMP_LESSEQUAL"
    assert state.pipeline.depth_target_bound is True


def test_d3d8_renderware_shademode_flat_uses_first_vertex_color(tmp_path):
    result = replay(D3D8_SHADEMODE_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d8"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 2500
    assert _ppm_pixel(Path(result["ppm_path"]), 16, 48, result["width"]) == (255, 0, 0)
    assert _ppm_pixel(Path(result["ppm_path"]), 48, 16, result["width"]) == (0, 0, 255)

    state = load_trace(D3D8_SHADEMODE_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert state.pipeline.metadata["d3d9_render_states"]["D3DRS_SHADEMODE"] == "D3DSHADE_FLAT"
    assert ffp["shade_mode"] == "D3DSHADE_FLAT"


def test_d3d8_renderware_texture_alpha_blend_uses_legacy_tss_path(tmp_path):
    result = replay(D3D8_TEXTURE_BLEND_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["api"] == "d3d8"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200

    state = load_trace(D3D8_TEXTURE_BLEND_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert state.render_target_format == "bgra8"
    assert state.pipeline.metadata["d3d9_texture_format"] == "bgra8"
    assert ffp["color_op"] == "D3DTOP_BLENDTEXTUREALPHA"
    assert ffp["color_arg1"] == "D3DTA_TEXTURE"
    assert ffp["color_arg2"] == "D3DTA_DIFFUSE"
    assert ffp["alpha_op"] == "D3DTOP_SELECTARG1"


def test_d3d9_fixed_function_texture_modulate_emits_shader_metadata(tmp_path):
    result = replay(TEXTURE_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200

    state = load_trace(TEXTURE_TRACE)
    assert state.render_target_format == "bgra8"
    assert state.depth_format == "d24s8"
    assert state.pipeline.metadata["d3d9_texture_format"] == "bgra8"
    assert state.pipeline.metadata["d3d9_ffp_shader"]["color_op"] == "D3DTOP_MODULATE"
    assert state.pipeline.metadata["d3d9_ffp_shader"]["color_arg1"] == "D3DTA_TEXTURE"
    assert state.pipeline.metadata["d3d9_ffp_shader"]["color_arg2"] == "D3DTA_DIFFUSE"


def test_d3d9_fixed_function_texture_alpha_blend_emits_shader_metadata(tmp_path):
    result = replay(TEXTURE_BLEND_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200

    state = load_trace(TEXTURE_BLEND_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert state.render_target_format == "bgra8"
    assert ffp["color_op"] == "D3DTOP_BLENDTEXTUREALPHA"
    assert ffp["color_arg1"] == "D3DTA_TEXTURE"
    assert ffp["color_arg2"] == "D3DTA_DIFFUSE"


def test_d3d9_ffp_arg_modifiers_affect_shading(tmp_path):
    result = replay(FFP_ARG_MODIFIER_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (191, 127, 63)

    state = load_trace(FFP_ARG_MODIFIER_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert ffp["color_arg1"] == "D3DTA_TEXTURE|D3DTA_COMPLEMENT"
    assert ffp["alpha_arg1"] == "D3DTA_TEXTURE|D3DTA_ALPHAREPLICATE"


def test_d3d9_ffp_addsigned_ops_affect_shading(tmp_path):
    result = replay(FFP_ADDSIGNED_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (64, 128, 192)

    state = load_trace(FFP_ADDSIGNED_TRACE)
    assert state.pipeline.metadata["d3d9_ffp_shader"]["color_op"] == "D3DTOP_ADDSIGNED2X"
    assert MockExecutor()._apply_d3d9_op(  # noqa: SLF001 - sibling combiner regression guard.
        "D3DTOP_ADDSIGNED",
        (96, 128, 160, 255),
        (64, 64, 64, 255),
        (96, 128, 160, 255),
        (96, 128, 160, 255),
        (64, 64, 64, 255),
    ) == (32, 64, 96, 255)


def test_d3d9_ffp_dotproduct3_affects_shading(tmp_path):
    result = replay(FFP_DOTPRODUCT_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (255, 255, 255)

    state = load_trace(FFP_DOTPRODUCT_TRACE)
    assert state.pipeline.metadata["d3d9_ffp_shader"]["color_op"] == "D3DTOP_DOTPRODUCT3"


def test_d3d9_ffp_blendfactoralpha_affects_shading(tmp_path):
    result = replay(FFP_BLENDFACTOR_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (100, 0, 100)

    state = load_trace(FFP_BLENDFACTOR_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert ffp["color_op"] == "D3DTOP_BLENDFACTORALPHA"
    assert ffp["texture_factor"] == (0, 0, 0, 128)


def test_d3d9_ffp_blenddiffusealpha_affects_shading(tmp_path):
    result = replay(FFP_BLENDDIFFUSE_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (100, 0, 100)

    state = load_trace(FFP_BLENDDIFFUSE_TRACE)
    assert state.pipeline.metadata["d3d9_ffp_shader"]["color_op"] == "D3DTOP_BLENDDIFFUSEALPHA"


def test_d3d9_ffp_blendcurrentalpha_uses_stage0_current_alpha(tmp_path):
    result = replay(FFP_BLENDCURRENT_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (100, 0, 100)

    state = load_trace(FFP_BLENDCURRENT_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert ffp["color_op"] == "D3DTOP_BLENDCURRENTALPHA"
    assert ffp["color_arg2"] == "D3DTA_CURRENT"


def test_d3d9_ffp_modulate4x_affects_shading(tmp_path):
    result = replay(FFP_MODULATE4X_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (129, 255, 64)

    state = load_trace(FFP_MODULATE4X_TRACE)
    assert state.pipeline.metadata["d3d9_ffp_shader"]["color_op"] == "D3DTOP_MODULATE4X"


def test_d3d9_ffp_multiplyadd_ops_affect_shading(tmp_path):
    result = replay(FFP_MULTIPLYADD_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (96, 144, 255)

    state = load_trace(FFP_MULTIPLYADD_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert ffp["color_op"] == "D3DTOP_MULTIPLYADD"
    assert ffp["color_arg0"] == "D3DTA_TFACTOR"
    assert ffp["color_arg1"] == "D3DTA_TEXTURE"
    assert ffp["color_arg2"] == "D3DTA_CURRENT"
    assert MockExecutor()._apply_d3d9_op(  # noqa: SLF001 - sibling combiner regression guard.
        "D3DTOP_MULTIPLYADD",
        (32, 16, 128, 255),
        (64, 128, 160, 255),
        (255, 255, 255, 255),
        (64, 128, 160, 255),
        None,
    ) == (96, 144, 255, 255)


def test_d3d9_ffp_lerp_ops_affect_shading(tmp_path):
    result = replay(FFP_LERP_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (128, 127, 0)

    state = load_trace(FFP_LERP_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert ffp["color_op"] == "D3DTOP_LERP"
    assert ffp["color_arg0"] == "D3DTA_TFACTOR"
    assert ffp["color_arg1"] == "D3DTA_TEXTURE"
    assert ffp["color_arg2"] == "D3DTA_CURRENT"
    assert MockExecutor()._apply_d3d9_op(  # noqa: SLF001 - sibling combiner regression guard.
        "D3DTOP_LERP",
        (128, 128, 128, 255),
        (255, 0, 0, 255),
        (0, 255, 0, 255),
        (64, 128, 160, 255),
        None,
    ) == (128, 127, 0, 255)


def test_d3d9_fixed_function_transform_emits_wvp_metadata(tmp_path):
    result = replay(TRANSFORM_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 900

    state = load_trace(TRANSFORM_TRACE)
    transforms = state.pipeline.metadata["d3d9_transforms"]
    assert transforms["D3DTS_WORLD"][12] == 0.35
    assert state.pipeline.metadata["d3d9_wvp_matrix"][12] == 0.35
    assert state.pipeline.metadata["d3d9_ffp_shader"]["wvp_matrix"][12] == 0.35


def test_d3d9_index32_trace_records_index_format(tmp_path):
    result = replay(INDEX32_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1000

    state = load_trace(INDEX32_TRACE)
    assert state.pipeline.metadata["d3d9_index_format_raw"] == "D3DFMT_INDEX32"
    assert state.pipeline.metadata["d3d9_index_format"] == "uint32"
    assert state.index_buffer == [0, 1, 2]


def test_d3d9_sampler_state_records_filter_and_address(tmp_path):
    result = replay(SAMPLER_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1000

    state = load_trace(SAMPLER_TRACE)
    sampler = state.pipeline.metadata["d3d9_sampler"]
    assert sampler["address_u"] == "D3DTADDRESS_WRAP"
    assert sampler["address_v"] == "D3DTADDRESS_WRAP"
    assert sampler["min_filter"] == "D3DTEXF_LINEAR"
    assert sampler["mag_filter"] == "D3DTEXF_LINEAR"


def test_d3d9_scissor_state_clips_rasterization(tmp_path):
    result = replay(SCISSOR_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] == 1024
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (26, 255, 51)
    assert _ppm_pixel(Path(result["ppm_path"]), 8, 32, result["width"]) == (4, 8, 16)

    state = load_trace(SCISSOR_TRACE)
    assert state.scissor == (16, 16, 32, 32)
    assert state.pipeline.metadata["d3d9_render_states"]["D3DRS_SCISSORTESTENABLE"] is True


def test_d3d9_programmable_xna_sprite_uses_texture_modulate_shader(tmp_path):
    result = replay(XNA_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 2000

    state = load_trace(XNA_TRACE)
    assert state.pipeline.metadata["programmable"] is True
    assert state.shader == "d3d9-programmable-texture-modulate"
    assert state.pipeline.vertex_shader == "xna_sprite_vs_3_0"
    assert state.pipeline.pixel_shader == "xna_sprite_ps_3_0"


def test_d3d9_fixed_function_alpha_test_discards_pixels(tmp_path):
    result = replay(ALPHA_TEST_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert 500 < result["non_background_pixels"] < 1700

    state = load_trace(ALPHA_TEST_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert ffp["alpha_test_enable"] is True
    assert ffp["alpha_ref"] == 128
    assert ffp["alpha_func"] == "D3DCMP_GREATER"


def test_d3d9_fixed_function_linear_fog_blends_rgb(tmp_path):
    result = replay(FOG_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (128, 128, 0)

    state = load_trace(FOG_TRACE)
    fog = state.pipeline.metadata["d3d9_fog_state"]
    assert fog["enable"] is True
    assert fog["color"] == (255, 0, 0, 255)
    assert fog["vertex_mode"] == "D3DFOG_LINEAR"
    assert fog["start"] == 0.0
    assert fog["end"] == 1.0
    assert state.pipeline.metadata["d3d9_ffp_shader"]["fog"] == fog


def test_d3d9_depth_state_rejects_later_far_pixels(tmp_path):
    result = replay(DEPTH_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1000
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"])[2] > 200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"])[0] < 80

    state = load_trace(DEPTH_TRACE)
    depth = state.pipeline.metadata["d3d9_depth_state"]
    assert depth["z_enable"] is True
    assert depth["z_write_enable"] is True
    assert depth["z_func"] == "D3DCMP_LESSEQUAL"
    assert state.pipeline.depth_target_bound is True


def test_d3d9_indexed_draw_range_limits_primitives(tmp_path):
    result = replay(INDEX_RANGE_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 350
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (4, 8, 16)

    state = load_trace(INDEX_RANGE_TRACE)
    draw = state.pipeline.metadata["d3d9_draw_range"]
    assert draw["start_index"] == 3
    assert draw["primitive_count"] == 1
    assert state.index_buffer == [0, 1, 2, 3, 4, 5]


def test_d3d9_base_vertex_offsets_indices(tmp_path):
    result = replay(BASE_VERTEX_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 350
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (4, 8, 16)

    state = load_trace(BASE_VERTEX_TRACE)
    draw = state.pipeline.metadata["d3d9_draw_range"]
    assert draw["base_vertex_index"] == 3
    assert state.index_buffer == [0, 1, 2]


def test_d3d9_drawprimitive_range_limits_vertices(tmp_path):
    result = replay(DRAW_RANGE_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 350
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (4, 8, 16)

    state = load_trace(DRAW_RANGE_TRACE)
    draw = state.pipeline.metadata["d3d9_draw_range"]
    assert draw["indexed"] is False
    assert draw["start_vertex"] == 3
    assert draw["primitive_count"] == 1


def test_d3d9_triangle_strip_records_topology_and_range(tmp_path):
    result = replay(STRIP_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 2000

    state = load_trace(STRIP_TRACE)
    assert state.topology == "trianglestrip"
    assert state.pipeline.metadata["d3d9_draw_range"]["primitive_count"] == 2
    assert state.pipeline.metadata["d3d9_draw_range"]["indexed"] is True


def test_d3d9_drawprimitive_triangle_strip_slices_vertices(tmp_path):
    result = replay(DRAW_STRIP_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 2000

    state = load_trace(DRAW_STRIP_TRACE)
    draw = state.pipeline.metadata["d3d9_draw_range"]
    assert state.topology == "trianglestrip"
    assert draw["indexed"] is False
    assert draw["start_vertex"] == 1
    assert draw["primitive_count"] == 2


def test_d3d9_color_write_mask_preserves_disabled_channels(tmp_path):
    result = replay(COLOR_WRITE_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 1200
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (255, 64, 16)

    state = load_trace(COLOR_WRITE_TRACE)
    assert state.pipeline.metadata["d3d9_render_states"]["D3DRS_COLORWRITEENABLE"] == 3


def test_d3d9_cullmode_rejects_clockwise_triangles(tmp_path):
    result = replay(CULL_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 350
    assert _ppm_pixel(Path(result["ppm_path"]), 32, 32, result["width"]) == (4, 8, 16)

    state = load_trace(CULL_TRACE)
    assert state.pipeline.metadata["d3d9_render_states"]["D3DRS_CULLMODE"] == "D3DCULL_CW"
    assert state.pipeline.metadata["d3d9_draw_range"]["primitive_count"] == 2


def test_d3d9_xna_alpha_blend_sprite_records_output_merger_state(tmp_path):
    result = replay(ALPHA_BLEND_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 2200

    state = load_trace(ALPHA_BLEND_TRACE)
    ffp = state.pipeline.metadata["d3d9_ffp_shader"]
    assert ffp["alpha_blend_enable"] is True
    assert ffp["src_blend"] == "D3DBLEND_SRCALPHA"
    assert ffp["dest_blend"] == "D3DBLEND_INVSRCALPHA"


def test_d3d9_format_sweep_maps_legacy_formats(tmp_path):
    result = replay(FORMAT_SWEEP_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 900

    state = load_trace(FORMAT_SWEEP_TRACE)
    assert state.render_target_format == "rgba10a2"
    assert state.depth_format == "d32f"
    assert state.pipeline.metadata["d3d9_texture_format"] == "bc3"


def test_d3d9_legacy_format_expansion_maps_more_dxgi_contracts(tmp_path):
    result = replay(LEGACY_FORMAT_TRACE, tmp_path, "mock", fail_on_unsupported=True)
    assert result["status"] == "PASS"
    assert result["present_count"] == 1
    assert result["non_background_pixels"] > 900

    state = load_trace(LEGACY_FORMAT_TRACE)
    assert state.render_target_format == "rgba8"
    assert state.depth_format == "d16"
    assert state.pipeline.metadata["d3d9_texture_format"] == "rgba8"
    assert D3D9_FORMATS["D3DFMT_A8B8G8R8"] == "rgba8"
    assert D3D9_FORMATS["D3DFMT_A8P8"] == "rgba8"
    assert D3D9_FORMATS["D3DFMT_A4L4"] == "rg8"
    assert D3D9_FORMATS["D3DFMT_CxV8U8"] == "rg8_snorm"
    assert D3D9_FORMATS["D3DFMT_D24FS8"] == "d24s8"
    assert D3D9_FORMATS["D3DFMT_DF24"] == "d24x8"
    assert D3D9_FORMATS["D3DFMT_YUY2"] == "yuv422_yuy2"
