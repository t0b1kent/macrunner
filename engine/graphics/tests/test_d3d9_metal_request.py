import json
from pathlib import Path

from engine.graphics.runtime_backend.metal_executor import MetalExecutor
from engine.graphics.tools.d3d_trace_replay import load_trace

ROOT = Path(__file__).resolve().parents[1]
XNA_ALPHA_TRACE = ROOT / "traces/runtime_samples/d3d9_xna_alpha_blend_sprite_runtime.jsonl"
FORMAT_SWEEP_TRACE = ROOT / "traces/runtime_samples/d3d9_format_sweep_runtime.jsonl"
TEXTURE_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_texture_blend_runtime.jsonl"
TRANSFORM_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_transform_runtime.jsonl"
INDEX32_TRACE = ROOT / "traces/runtime_samples/d3d9_index32_triangle_runtime.jsonl"


def test_d3d9_xna_alpha_blend_writes_metal_request_contract(tmp_path):
    state = load_trace(XNA_ALPHA_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(XNA_ALPHA_TRACE),
        name="xna-alpha",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["shader"] == "d3d9-programmable-texture-modulate"
    assert payload["render_target_format"] == "bgra8"
    assert payload["vertex_count"] == 4
    assert payload["index_count"] == 6
    assert len(payload["vertices"]) == 4
    assert payload["indices"] == [0, 1, 2, 2, 1, 3]
    assert payload["texture_size"] == [2, 2]
    assert payload["texture_pixels"][0] == [255, 255, 255, 128]
    assert payload["present_count"] == 1
    assert payload["pipeline"]["vertex_shader"] == "xna_sprite_vs_3_0"
    assert payload["pipeline"]["pixel_shader"] == "xna_sprite_ps_3_0"
    assert payload["d3d9"]["translation_target"] == "d3d11"
    assert payload["d3d9"]["programmable"] is True
    assert payload["d3d9"]["render_states"]["D3DRS_ALPHABLENDENABLE"] is True
    assert payload["d3d9"]["render_states"]["D3DRS_SRCBLEND"] == "D3DBLEND_SRCALPHA"
    assert payload["d3d9"]["render_states"]["D3DRS_DESTBLEND"] == "D3DBLEND_INVSRCALPHA"
    assert payload["d3d9"]["texture_format"] == "bgra8"


def test_d3d9_format_sweep_writes_metal_format_contract(tmp_path):
    state = load_trace(FORMAT_SWEEP_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FORMAT_SWEEP_TRACE),
        name="format-sweep",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["render_target_format"] == "rgba10a2"
    assert payload["depth_format"] == "d32f"
    assert payload["texture_size"] == [2, 2]
    assert payload["texture_pixels"][0] == [0, 255, 255, 255]
    assert payload["d3d9"]["texture_format"] == "bc3"
    assert payload["d3d9"]["ffp_shader"]["texture_factor"] == [255, 255, 255, 255]
    assert payload["d3d9"]["present_parameters"]["backbuffer_format"] == "D3DFMT_A2R10G10B10"


def test_d3d9_texture_alpha_blend_writes_metal_combiner_contract(tmp_path):
    state = load_trace(TEXTURE_BLEND_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(TEXTURE_BLEND_TRACE),
        name="texture-alpha-blend",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["shader"] == "d3d9-fixed-function"
    assert payload["texture_pixels"][1] == [64, 255, 64, 128]
    assert payload["d3d9"]["ffp_shader"]["color_op"] == "D3DTOP_BLENDTEXTUREALPHA"
    assert payload["d3d9"]["ffp_shader"]["color_arg1"] == "D3DTA_TEXTURE"
    assert payload["d3d9"]["ffp_shader"]["color_arg2"] == "D3DTA_DIFFUSE"


def test_d3d9_transform_writes_metal_wvp_contract(tmp_path):
    state = load_trace(TRANSFORM_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(TRANSFORM_TRACE),
        name="ffp-transform",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "indexed_triangle"
    assert payload["vertex_count"] == 3
    assert payload["d3d9"]["transforms"]["D3DTS_WORLD"][12] == 0.35
    assert payload["d3d9"]["wvp_matrix"][12] == 0.35
    assert payload["d3d9"]["ffp_shader"]["wvp_matrix"][12] == 0.35


def test_d3d9_index32_writes_metal_index_contract(tmp_path):
    state = load_trace(INDEX32_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(INDEX32_TRACE),
        name="index32",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "indexed_triangle"
    assert payload["index_format"] == "uint32"
    assert payload["indices"] == [0, 1, 2]
    assert payload["d3d9"]["index_format"] == "uint32"
    assert payload["d3d9"]["index_format_raw"] == "D3DFMT_INDEX32"
