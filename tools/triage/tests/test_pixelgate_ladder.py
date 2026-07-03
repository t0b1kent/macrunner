"""Unit tests for the pixel-gate ladder rungs 12-14 and the hard swapchain
matcher. Run: python3 -m pytest tools/triage/tests/test_pixelgate_ladder.py
or: python3 tools/triage/tests/test_pixelgate_ladder.py"""
import os, sys, json, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import classify_run as C

REAL_GETBUFFER = ("macrunner-hb-dxgi-swapchain: method=GetBuffer slot=9 "
                  "swapchain=0xedcb05c90 rc=0x0 a1=0x0 a2=0x20054 ret_addr=0x87efd4fc75a pc=0x6f0000004670\n")
REAL_PRESENT = ("macrunner-hb-dxgi-swapchain: method=Present slot=8 "
                "swapchain=0xedcb05c90 rc=0x0 a1=0x0 a2=0x0 ret_addr=0x87efd4fc942 pc=0x6f0000004600\n")
CANDIDATE_SLOT8 = ("macrunner-hb-dxgi-swapchain: candidate method=Present slot=8 "
                   "object=0x109e1ec80 rc=0x0 a1=0x20054 a2=0x3 ret_addr=0x87efd4fc942 pc=0x6f0000004600 import=dxmt-com:0x87ef4052f80:8\n")
IAT_LINE = ("macrunner-hb-iatentry: dll=dxgi.dll import=dxgi.dll!CreateDXGIFactory1 slot=...\n")
PRESENT_FAILED = ("macrunner-hb-dxgi-swapchain: method=Present slot=8 "
                  "swapchain=0xedcb05c90 rc=0x887a0005 a1=0x0 ret_addr=0x87efd4fc942 pc=0x6f0000004600\n")
SWAPCHAIN_CREATE = ("macrunner-hb-dxgi-swapchain: create method=CreateSwapChainForHwnd slot=15 "
                    "factory=0x109e1ec80 swapchain=0xedcb05c90 rc=0x0 ret_addr=0x87efd4fc75a pc=0x6f0000004670\n")

def _rung(text):
    return C.ladder_rung(text)

def test_getbuffer_real_marker_lights_rung12():
    idx, name = _rung(SWAPCHAIN_CREATE + REAL_GETBUFFER)
    assert (idx, name) == (12, "getbuffer"), (idx, name)

def test_real_present_lights_rung14():
    idx, name = _rung(SWAPCHAIN_CREATE + REAL_GETBUFFER + REAL_PRESENT)
    assert (idx, name) == (14, "real-present"), (idx, name)

def test_candidate_slot8_does_not_light_present_rung():
    # The slot-8 MakeWindowAssociation ложняк must NOT credit rung 14.
    idx, name = _rung(SWAPCHAIN_CREATE + CANDIDATE_SLOT8)
    assert name != "real-present", (idx, name)
    assert idx < 14, (idx, name)

def test_iat_binding_does_not_light_rungs():
    # IAT-binding diagnostic lines name imports at load time, not real calls.
    idx, name = _rung(IAT_LINE)
    assert name in (None, "after-loader"), (idx, name)
    assert idx < 12, (idx, name)

def test_failed_present_does_not_light_rung14():
    # rc != 0 -> the Present call errored, no frame.
    idx, name = _rung(SWAPCHAIN_CREATE + REAL_GETBUFFER + PRESENT_FAILED)
    assert name != "real-present", (idx, name)
    assert idx < 14, (idx, name)

def test_swapchain_only_stops_at_rung11():
    idx, name = _rung(SWAPCHAIN_CREATE)
    assert (idx, name) == (11, "swapchain"), (idx, name)

def test_rtv_marker_is_trace_gap_placeholder():
    # The proposed RTV trace marker is the only thing that can light rung 13.
    rtv_line = "macrunner-hb-d3d-rtv: CreateRenderTargetView rc=0x0 swapchain=0xedcb05c90\n"
    idx, name = _rung(SWAPCHAIN_CREATE + REAL_GETBUFFER + rtv_line)
    assert (idx, name) == (13, "rtv"), (idx, name)
    # And it does NOT light from any existing log marker (no false positive).
    idx2, name2 = _rung(SWAPCHAIN_CREATE + REAL_GETBUFFER)
    assert name2 == "getbuffer", (idx2, name2)

def test_pixel_truth_confirmed_from_artifact():
    with tempfile.TemporaryDirectory() as d:
        sub = os.path.join(d, "pixel-truth-gate", "20260704T000000Z-1")
        os.makedirs(sub)
        json.dump({"verdict": "COLORFUL"}, open(os.path.join(sub, "result.json"), "w"))
        ok, path, verdict = C.pixel_truth_confirmed(d)
        assert ok and verdict == "COLORFUL", (ok, verdict)

def test_pixel_truth_not_confirmed_black():
    with tempfile.TemporaryDirectory() as d:
        sub = os.path.join(d, "pixel-truth-gate", "20260704T000000Z-1")
        os.makedirs(sub)
        json.dump({"verdict": "BLACK"}, open(os.path.join(sub, "result.json"), "w"))
        ok, path, verdict = C.pixel_truth_confirmed(d)
        assert not ok and verdict == "BLACK", (ok, verdict)

def test_pixel_truth_no_artifact():
    with tempfile.TemporaryDirectory() as d:
        ok, path, verdict = C.pixel_truth_confirmed(d)
        assert not ok and path is None, (ok, path)

def _run_main_on_text(text):
    """Drive classify_run.main() against a temp run dir containing a run.log."""
    import subprocess
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "run.log"), "w").write(text)
        r = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "..", "classify_run.py"), d],
                           capture_output=True, text=True)
        return r.stdout + r.stderr

def test_pixel_moment_requires_both_marker_and_artifact():
    # Real-Present marker present but NO pixel artifact -> PIXEL_MOMENT_REACHED: no
    out = _run_main_on_text(SWAPCHAIN_CREATE + REAL_GETBUFFER + REAL_PRESENT)
    assert "PIXEL_MOMENT_REACHED: no" in out, out
    assert "PIXEL_PENDING" in out, out

if __name__ == "__main__":
    import traceback
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for fn in fns:
        try:
            fn()
            passed += 1
            print(f"PASS {fn.__name__}")
        except Exception as e:
            failed += 1
            print(f"FAIL {fn.__name__}: {e}")
            traceback.print_exc()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
