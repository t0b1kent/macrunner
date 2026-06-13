import json
from pathlib import Path

from engine.graphics.runtime_backend.metal_executor import MetalExecutor
from engine.graphics.tools.d3d_trace_replay import load_trace

ROOT = Path(__file__).resolve().parents[1]
D3D8_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_fixed_function_runtime.jsonl"
D3D8_STRIP_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_triangle_strip_runtime.jsonl"
D3D8_FOG_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_fog_runtime.jsonl"
D3D8_ALPHA_TEST_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_alpha_test_runtime.jsonl"
D3D8_ALPHA_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_alpha_blend_runtime.jsonl"
D3D8_DEPTH_CULL_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_depth_cull_runtime.jsonl"
D3D8_SHADEMODE_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_shademode_flat_runtime.jsonl"
D3D8_TEXTURE_BLEND_TRACE = ROOT / "traces/runtime_samples/d3d8_renderware_texture_blend_runtime.jsonl"
XNA_ALPHA_TRACE = ROOT / "traces/runtime_samples/d3d9_xna_alpha_blend_sprite_runtime.jsonl"
FORMAT_SWEEP_TRACE = ROOT / "traces/runtime_samples/d3d9_format_sweep_runtime.jsonl"
LEGACY_FORMAT_TRACE = ROOT / "traces/runtime_samples/d3d9_legacy_format_expansion_runtime.jsonl"
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
FOG_TRACE = ROOT / "traces/runtime_samples/d3d9_fixed_function_fog_runtime.jsonl"
INDEX32_TRACE = ROOT / "traces/runtime_samples/d3d9_index32_triangle_runtime.jsonl"
SAMPLER_TRACE = ROOT / "traces/runtime_samples/d3d9_sampler_linear_wrap_runtime.jsonl"
SCISSOR_TRACE = ROOT / "traces/runtime_samples/d3d9_scissor_test_runtime.jsonl"
DEPTH_TRACE = ROOT / "traces/runtime_samples/d3d9_depth_test_runtime.jsonl"
INDEX_RANGE_TRACE = ROOT / "traces/runtime_samples/d3d9_indexed_range_runtime.jsonl"
CULL_TRACE = ROOT / "traces/runtime_samples/d3d9_cullmode_runtime.jsonl"
BASE_VERTEX_TRACE = ROOT / "traces/runtime_samples/d3d9_base_vertex_runtime.jsonl"
COLOR_WRITE_TRACE = ROOT / "traces/runtime_samples/d3d9_color_write_mask_runtime.jsonl"
DRAW_RANGE_TRACE = ROOT / "traces/runtime_samples/d3d9_drawprimitive_range_runtime.jsonl"
STRIP_TRACE = ROOT / "traces/runtime_samples/d3d9_triangle_strip_runtime.jsonl"
DRAW_STRIP_TRACE = ROOT / "traces/runtime_samples/d3d9_drawprimitive_triangle_strip_runtime.jsonl"


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


def test_d3d8_renderware_writes_metal_request_contract(tmp_path):
    state = load_trace(D3D8_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(D3D8_TRACE),
        name="d3d8-renderware",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d8"
    assert payload["mode"] == "texture"
    assert payload["d3d9"]["translation_target"] == "d3d11"
    assert payload["d3d9"]["wvp_matrix"][12] == -0.2
    assert payload["d3d9"]["sampler"]["address_u"] == "D3DTADDRESS_CLAMP"
    assert payload["d3d9"]["texture_format"] == "bgra8"


def test_d3d8_renderware_triangle_strip_writes_metal_contract(tmp_path):
    state = load_trace(D3D8_STRIP_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(D3D8_STRIP_TRACE),
        name="d3d8-renderware-strip",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d8"
    assert payload["mode"] == "texture"
    assert payload["topology"] == "trianglestrip"
    assert payload["indices"] == [0, 1, 2, 3]
    assert payload["index_count"] == 4
    assert payload["d3d9"]["draw_range"]["primitive_count"] == 2
    assert payload["d3d9"]["texture_format"] == "bgra8"


def test_d3d8_renderware_fog_writes_metal_request_contract(tmp_path):
    state = load_trace(D3D8_FOG_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(D3D8_FOG_TRACE),
        name="d3d8-renderware-fog",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    fog = payload["d3d9"]["fog_state"]
    assert payload["source_api"] == "d3d8"
    assert payload["mode"] == "indexed_triangle"
    assert fog["enable"] is True
    assert fog["color"] == [255, 0, 0, 255]
    assert fog["vertex_mode"] == "D3DFOG_LINEAR"
    assert fog["start"] == 0.0
    assert fog["end"] == 1.0
    assert payload["d3d9"]["ffp_shader"]["fog"] == fog


def test_d3d8_renderware_alpha_test_writes_metal_request_contract(tmp_path):
    state = load_trace(D3D8_ALPHA_TEST_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(D3D8_ALPHA_TEST_TRACE),
        name="d3d8-renderware-alpha-test",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d8"
    assert payload["mode"] == "texture"
    assert payload["d3d9"]["render_states"]["D3DRS_ALPHATESTENABLE"] is True
    assert payload["d3d9"]["render_states"]["D3DRS_ALPHAREF"] == 128
    assert payload["d3d9"]["render_states"]["D3DRS_ALPHAFUNC"] == "D3DCMP_GREATER"
    assert payload["d3d9"]["ffp_shader"]["alpha_test_enable"] is True
    assert payload["d3d9"]["ffp_shader"]["alpha_ref"] == 128
    assert payload["d3d9"]["ffp_shader"]["alpha_func"] == "D3DCMP_GREATER"


def test_d3d8_renderware_alpha_blend_writes_metal_request_contract(tmp_path):
    state = load_trace(D3D8_ALPHA_BLEND_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(D3D8_ALPHA_BLEND_TRACE),
        name="d3d8-renderware-alpha-blend",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    render_states = payload["d3d9"]["render_states"]
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d8"
    assert payload["mode"] == "indexed_triangle"
    assert render_states["D3DRS_ALPHABLENDENABLE"] is True
    assert render_states["D3DRS_SRCBLEND"] == "D3DBLEND_SRCALPHA"
    assert render_states["D3DRS_DESTBLEND"] == "D3DBLEND_INVSRCALPHA"
    assert ffp["alpha_blend_enable"] is True
    assert ffp["src_blend"] == "D3DBLEND_SRCALPHA"
    assert ffp["dest_blend"] == "D3DBLEND_INVSRCALPHA"


def test_d3d8_renderware_depth_cull_writes_metal_request_contract(tmp_path):
    state = load_trace(D3D8_DEPTH_CULL_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(D3D8_DEPTH_CULL_TRACE),
        name="d3d8-renderware-depth-cull",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    render_states = payload["d3d9"]["render_states"]
    depth = payload["d3d9"]["depth_state"]
    assert payload["source_api"] == "d3d8"
    assert payload["mode"] == "indexed_triangle"
    assert payload["depth_format"] == "d24s8"
    assert render_states["D3DRS_CULLMODE"] == "D3DCULL_CW"
    assert depth["z_enable"] is True
    assert depth["z_write_enable"] is False
    assert depth["z_func"] == "D3DCMP_LESSEQUAL"


def test_d3d8_renderware_shademode_writes_metal_request_contract(tmp_path):
    state = load_trace(D3D8_SHADEMODE_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(D3D8_SHADEMODE_TRACE),
        name="d3d8-renderware-shademode-flat",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d8"
    assert payload["mode"] == "indexed_triangle"
    assert payload["indices"] == [0, 1, 2, 2, 1, 3]
    assert payload["d3d9"]["render_states"]["D3DRS_SHADEMODE"] == "D3DSHADE_FLAT"
    assert payload["d3d9"]["ffp_shader"]["shade_mode"] == "D3DSHADE_FLAT"


def test_d3d8_renderware_texture_alpha_blend_writes_metal_request_contract(tmp_path):
    state = load_trace(D3D8_TEXTURE_BLEND_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(D3D8_TEXTURE_BLEND_TRACE),
        name="d3d8-renderware-texture-blend",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d8"
    assert payload["mode"] == "texture"
    assert payload["d3d9"]["texture_format"] == "bgra8"
    assert ffp["color_op"] == "D3DTOP_BLENDTEXTUREALPHA"
    assert ffp["color_arg1"] == "D3DTA_TEXTURE"
    assert ffp["color_arg2"] == "D3DTA_DIFFUSE"


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


def test_d3d9_legacy_format_expansion_writes_metal_contract(tmp_path):
    state = load_trace(LEGACY_FORMAT_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(LEGACY_FORMAT_TRACE),
        name="legacy-format-expansion",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["render_target_format"] == "rgba8"
    assert payload["depth_format"] == "d16"
    assert payload["d3d9"]["texture_format"] == "rgba8"
    assert payload["d3d9"]["present_parameters"]["backbuffer_format"] == "D3DFMT_X8B8G8R8"


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


def test_d3d9_ffp_arg_modifiers_write_metal_contract(tmp_path):
    state = load_trace(FFP_ARG_MODIFIER_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_ARG_MODIFIER_TRACE),
        name="ffp-arg-modifier",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_size"] == [1, 1]
    assert payload["texture_pixels"][0] == [64, 128, 192, 224]
    assert ffp["color_arg1"] == "D3DTA_TEXTURE|D3DTA_COMPLEMENT"
    assert ffp["alpha_arg1"] == "D3DTA_TEXTURE|D3DTA_ALPHAREPLICATE"


def test_d3d9_ffp_addsigned_writes_metal_contract(tmp_path):
    state = load_trace(FFP_ADDSIGNED_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_ADDSIGNED_TRACE),
        name="ffp-addsigned",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_pixels"][0] == [96, 128, 160, 255]
    assert ffp["color_op"] == "D3DTOP_ADDSIGNED2X"


def test_d3d9_ffp_dotproduct3_writes_metal_contract(tmp_path):
    state = load_trace(FFP_DOTPRODUCT_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_DOTPRODUCT_TRACE),
        name="ffp-dotproduct3",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_pixels"][0] == [255, 128, 128, 255]
    assert ffp["color_op"] == "D3DTOP_DOTPRODUCT3"


def test_d3d9_ffp_blendfactoralpha_writes_metal_contract(tmp_path):
    state = load_trace(FFP_BLENDFACTOR_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_BLENDFACTOR_TRACE),
        name="ffp-blendfactoralpha",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_pixels"][0] == [200, 0, 0, 255]
    assert ffp["color_op"] == "D3DTOP_BLENDFACTORALPHA"
    assert ffp["texture_factor"] == [0, 0, 0, 128]


def test_d3d9_ffp_blenddiffusealpha_writes_metal_contract(tmp_path):
    state = load_trace(FFP_BLENDDIFFUSE_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_BLENDDIFFUSE_TRACE),
        name="ffp-blenddiffusealpha",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_pixels"][0] == [200, 0, 0, 255]
    assert ffp["color_op"] == "D3DTOP_BLENDDIFFUSEALPHA"


def test_d3d9_ffp_blendcurrentalpha_writes_metal_contract(tmp_path):
    state = load_trace(FFP_BLENDCURRENT_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_BLENDCURRENT_TRACE),
        name="ffp-blendcurrentalpha",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_pixels"][0] == [200, 0, 0, 255]
    assert ffp["color_op"] == "D3DTOP_BLENDCURRENTALPHA"
    assert ffp["color_arg2"] == "D3DTA_CURRENT"


def test_d3d9_ffp_modulate4x_writes_metal_contract(tmp_path):
    state = load_trace(FFP_MODULATE4X_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_MODULATE4X_TRACE),
        name="ffp-modulate4x",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_pixels"][0] == [64, 64, 64, 255]
    assert ffp["color_op"] == "D3DTOP_MODULATE4X"


def test_d3d9_ffp_multiplyadd_writes_metal_contract(tmp_path):
    state = load_trace(FFP_MULTIPLYADD_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_MULTIPLYADD_TRACE),
        name="ffp-multiplyadd",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_pixels"][0] == [64, 128, 160, 255]
    assert ffp["color_op"] == "D3DTOP_MULTIPLYADD"
    assert ffp["color_arg0"] == "D3DTA_TFACTOR"
    assert ffp["color_arg1"] == "D3DTA_TEXTURE"
    assert ffp["color_arg2"] == "D3DTA_CURRENT"


def test_d3d9_ffp_lerp_writes_metal_contract(tmp_path):
    state = load_trace(FFP_LERP_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FFP_LERP_TRACE),
        name="ffp-lerp",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    ffp = payload["d3d9"]["ffp_shader"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert payload["texture_pixels"][0] == [255, 0, 0, 255]
    assert ffp["color_op"] == "D3DTOP_LERP"
    assert ffp["color_arg0"] == "D3DTA_TFACTOR"
    assert ffp["color_arg1"] == "D3DTA_TEXTURE"
    assert ffp["color_arg2"] == "D3DTA_CURRENT"


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


def test_d3d9_fog_writes_metal_fog_contract(tmp_path):
    state = load_trace(FOG_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(FOG_TRACE),
        name="ffp-fog",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    fog = payload["d3d9"]["fog_state"]
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "indexed_triangle"
    assert fog["enable"] is True
    assert fog["color"] == [255, 0, 0, 255]
    assert fog["vertex_mode"] == "D3DFOG_LINEAR"
    assert fog["start"] == 0.0
    assert fog["end"] == 1.0
    assert payload["d3d9"]["ffp_shader"]["fog"] == fog


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


def test_d3d9_sampler_writes_metal_sampler_contract(tmp_path):
    state = load_trace(SAMPLER_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(SAMPLER_TRACE),
        name="sampler-linear-wrap",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "texture"
    assert len(payload["d3d9"]["sampler_states"]) == 4
    sampler = payload["d3d9"]["sampler"]
    assert sampler["address_u"] == "D3DTADDRESS_WRAP"
    assert sampler["address_v"] == "D3DTADDRESS_WRAP"
    assert sampler["min_filter"] == "D3DTEXF_LINEAR"
    assert sampler["mag_filter"] == "D3DTEXF_LINEAR"


def test_d3d9_scissor_writes_metal_scissor_contract(tmp_path):
    state = load_trace(SCISSOR_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(SCISSOR_TRACE),
        name="scissor",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["scissor"] == [16, 16, 32, 32]
    assert payload["d3d9"]["render_states"]["D3DRS_SCISSORTESTENABLE"] is True


def test_d3d9_depth_writes_metal_depth_contract(tmp_path):
    state = load_trace(DEPTH_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(DEPTH_TRACE),
        name="depth",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "indexed_triangle"
    assert payload["depth_format"] == "d24s8"
    depth = payload["d3d9"]["depth_state"]
    assert depth["z_enable"] is True
    assert depth["z_write_enable"] is True
    assert depth["z_func"] == "D3DCMP_LESSEQUAL"


def test_d3d9_indexed_range_writes_effective_metal_indices(tmp_path):
    state = load_trace(INDEX_RANGE_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(INDEX_RANGE_TRACE),
        name="indexed-range",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "indexed_triangle"
    assert payload["indices"] == [3, 4, 5]
    assert payload["index_count"] == 3
    assert payload["d3d9"]["draw_range"]["start_index"] == 3
    assert payload["d3d9"]["draw_range"]["primitive_count"] == 1


def test_d3d9_base_vertex_writes_effective_metal_indices(tmp_path):
    state = load_trace(BASE_VERTEX_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(BASE_VERTEX_TRACE),
        name="base-vertex",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["indices"] == [3, 4, 5]
    assert payload["d3d9"]["draw_range"]["base_vertex_index"] == 3
    assert payload["d3d9"]["draw_range"]["primitive_count"] == 1


def test_d3d9_drawprimitive_range_writes_effective_metal_vertices(tmp_path):
    state = load_trace(DRAW_RANGE_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(DRAW_RANGE_TRACE),
        name="drawprimitive-range",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "triangle"
    assert payload["vertex_count"] == 3
    assert payload["index_count"] == 0
    assert payload["vertices"][0]["position"] == [-0.95, -0.8, 0.0, 1.0]
    assert payload["d3d9"]["draw_range"]["indexed"] is False
    assert payload["d3d9"]["draw_range"]["start_vertex"] == 3


def test_d3d9_triangle_strip_writes_metal_topology_contract(tmp_path):
    state = load_trace(STRIP_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(STRIP_TRACE),
        name="triangle-strip",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["topology"] == "trianglestrip"
    assert payload["indices"] == [0, 1, 2, 3]
    assert payload["index_count"] == 4
    assert payload["d3d9"]["draw_range"]["primitive_count"] == 2


def test_d3d9_drawprimitive_triangle_strip_writes_sliced_vertices(tmp_path):
    state = load_trace(DRAW_STRIP_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(DRAW_STRIP_TRACE),
        name="drawprimitive-triangle-strip",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "triangle"
    assert payload["topology"] == "trianglestrip"
    assert payload["vertex_count"] == 4
    assert payload["index_count"] == 0
    assert payload["vertices"][0]["position"] == [-0.8, 0.8, 0.0, 1.0]
    assert payload["d3d9"]["draw_range"]["indexed"] is False
    assert payload["d3d9"]["draw_range"]["start_vertex"] == 1
    assert payload["d3d9"]["draw_range"]["primitive_count"] == 2


def test_d3d9_color_write_mask_writes_metal_render_state_contract(tmp_path):
    state = load_trace(COLOR_WRITE_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(COLOR_WRITE_TRACE),
        name="color-write-mask",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "indexed_triangle"
    assert payload["d3d9"]["render_states"]["D3DRS_COLORWRITEENABLE"] == 3


def test_d3d9_cullmode_writes_metal_render_state_contract(tmp_path):
    state = load_trace(CULL_TRACE)
    written = MetalExecutor(helper_path=tmp_path / "missing-metal-helper").write_request(
        state,
        tmp_path,
        trace_path=str(CULL_TRACE),
        name="cullmode",
    )

    payload = json.loads(Path(written["request_path"]).read_text())
    assert payload["source_api"] == "d3d9"
    assert payload["mode"] == "indexed_triangle"
    assert payload["indices"] == [0, 1, 2, 3, 4, 5]
    assert payload["d3d9"]["render_states"]["D3DRS_CULLMODE"] == "D3DCULL_CW"
