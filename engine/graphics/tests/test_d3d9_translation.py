from pathlib import Path

from engine.graphics.tools.d3d_trace_replay import load_trace, replay

ROOT = Path(__file__).resolve().parents[1]
TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_triangle_runtime.jsonl"
TEXTURE_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_texture_modulate_runtime.jsonl"
TEXTURE_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_texture_blend_runtime.jsonl"
XNA_TRACE = ROOT / "traces/runtime_samples/d3d9_xna_programmable_sprite_runtime.jsonl"
ALPHA_TEST_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_alpha_test_runtime.jsonl"
ALPHA_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d9_xna_alpha_blend_sprite_runtime.jsonl"
FORMAT_SWEEP_TRACE = ROOT / "traces/runtime_samples/d3d9_format_sweep_runtime.jsonl"


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
