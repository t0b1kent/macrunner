import hashlib
import json
import os
import re
import shlex
import struct
import threading
import time

import lldb


BLOCK_HEADER_SIZE = 32
IR_INSTR_SIZE = 184
CTX_PREFIX_SIZE = 640
MAX_INSTR_COUNT = 256


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def _read(process, address, size, label, errors):
    error = lldb.SBError()
    data = process.ReadMemory(address, size, error)
    received = len(data) if data is not None else 0
    if not error.Success() or received != size:
        errors.append({
            "label": label,
            "address": "0x%x" % address,
            "requested": size,
            "received": received,
            "error": error.GetCString() or "short read",
        })
        return None
    return bytes(data)


def _registers(frame):
    output = []
    groups = frame.GetRegisters()
    for group_index in range(groups.GetSize()):
        group = groups.GetValueAtIndex(group_index)
        values = []
        for register_index in range(group.GetNumChildren()):
            register = group.GetChildAtIndex(register_index)
            values.append({
                "name": register.GetName(),
                "value": register.GetValue(),
            })
        output.append({"group": group.GetName(), "values": values})
    return output


def _backtrace(thread):
    output = []
    for index in range(thread.GetNumFrames()):
        frame = thread.GetFrameAtIndex(index)
        module = frame.GetModule()
        output.append({
            "index": index,
            "pc": "0x%x" % frame.GetPC(),
            "function": frame.GetFunctionName(),
            "module": module.GetFileSpec().fullpath if module.IsValid() else None,
        })
    return output


def _modules(target):
    output = []
    for index in range(target.GetNumModules()):
        module = target.GetModuleAtIndex(index)
        header = module.GetObjectFileHeaderAddress()
        load = header.GetLoadAddress(target) if header.IsValid() else lldb.LLDB_INVALID_ADDRESS
        output.append({
            "path": module.GetFileSpec().fullpath,
            "load_address": None if load == lldb.LLDB_INVALID_ADDRESS else "0x%x" % load,
            "uuid": module.GetUUIDString(),
        })
    return output


def _write_blob(output_dir, name, data):
    path = os.path.join(output_dir, name)
    with open(path, "wb") as stream:
        stream.write(data)
    return {
        "path": name,
        "size": len(data),
        "sha256": _sha256(data),
    }


def _write_unknown(output_dir, process, thread, reason):
    capture = {
        "schema": "macrunner.hk.guest-loop-lldb-capture.v1",
        "capture_epoch": int(time.time()),
        "pid": process.GetProcessID(),
        "thread_id": thread.GetThreadID() if thread.IsValid() else None,
        "stop_reason": (thread.GetStopDescription(256)
                        if thread.IsValid() else None),
        "status": "UNKNOWN",
        "reason": reason,
    }
    capture_path = os.path.join(output_dir, "capture.json")
    with open(capture_path, "w", encoding="utf-8") as stream:
        json.dump(capture, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return capture_path


def hk_capture(debugger, command, exe_ctx, result, internal_dict):
    del internal_dict
    args = shlex.split(command)
    if len(args) != 1:
        result.SetError("usage: hk-capture OUTPUT_DIR")
        return

    output_dir = os.path.realpath(args[0])
    os.makedirs(output_dir, exist_ok=True)
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    thread = process.GetSelectedThread()
    frame = thread.GetFrameAtIndex(0)
    stop_reason = thread.GetStopReason()
    function_name = frame.GetFunctionName()
    if (stop_reason != lldb.eStopReasonBreakpoint or
            function_name != "hb_jit_helper_exec_two_block_loop"):
        capture_path = _write_unknown(
            output_dir, process, thread,
            "not stopped at required helper breakpoint")
        result.AppendMessage("hk-capture status=UNKNOWN path=%s" % capture_path)
        return
    errors = []
    optional_errors = []

    x0 = frame.FindRegister("x0").GetValueAsUnsigned()
    x1 = frame.FindRegister("x1").GetValueAsUnsigned()
    x2 = frame.FindRegister("x2").GetValueAsUnsigned()
    capture = {
        "schema": "macrunner.hk.guest-loop-lldb-capture.v1",
        "capture_epoch": int(time.time()),
        "pid": process.GetProcessID(),
        "thread_id": thread.GetThreadID(),
        "stop_reason": thread.GetStopDescription(256),
        "function": function_name,
        "x0": "0x%x" % x0,
        "x1": "0x%x" % x1,
        "x2": "0x%x" % x2,
        "registers": _registers(frame),
        "backtrace": _backtrace(thread),
        "modules": _modules(target),
        "required_read_errors": errors,
        "optional_read_errors": optional_errors,
        "files": {},
    }

    ctx_prefix = _read(process, x0, CTX_PREFIX_SIZE, "ctx-prefix", errors)
    if ctx_prefix is not None:
        capture["files"]["ctx_prefix"] = _write_blob(
            output_dir, "ctx-prefix.bin", ctx_prefix)
        capture["ctx_pc"] = "0x%x" % struct.unpack_from("<Q", ctx_prefix, 544)[0]

    for label, address in (("first", x1), ("second", x2)):
        header = _read(process, address, BLOCK_HEADER_SIZE,
                       label + "-block-header", errors)
        if header is None:
            continue
        capture["files"][label + "_header"] = _write_blob(
            output_dir, label + "-block-header.bin", header)
        guest_addr = struct.unpack_from("<Q", header, 8)[0]
        instrs = struct.unpack_from("<Q", header, 16)[0]
        instr_count = struct.unpack_from("<Q", header, 24)[0]
        capture[label + "_block"] = {
            "address": "0x%x" % address,
            "guest_addr": "0x%x" % guest_addr,
            "instrs": "0x%x" % instrs,
            "instr_count": instr_count,
        }
        if instr_count > MAX_INSTR_COUNT:
            errors.append({
                "label": label + "-instr-count",
                "value": instr_count,
                "maximum": MAX_INSTR_COUNT,
            })
            continue
        ir = _read(process, instrs, instr_count * IR_INSTR_SIZE,
                   label + "-ir", errors)
        if ir is not None:
            capture["files"][label + "_ir"] = _write_blob(
                output_dir, label + "-ir.bin", ir)
        guest = _read(process, guest_addr, 64, label + "-guest-bytes",
                      optional_errors)
        if guest is not None:
            capture["files"][label + "_guest_bytes"] = _write_blob(
                output_dir, label + "-guest-bytes.bin", guest)

    capture["status"] = "PASS" if not errors else "UNKNOWN"
    capture_path = os.path.join(output_dir, "capture.json")
    with open(capture_path, "w", encoding="utf-8") as stream:
        json.dump(capture, stream, indent=2, sort_keys=True)
        stream.write("\n")
    result.AppendMessage("hk-capture status=%s path=%s" %
                         (capture["status"], capture_path))


def hk_map_capture(debugger, command, exe_ctx, result, internal_dict):
    del internal_dict
    args = shlex.split(command)
    if len(args) != 3:
        result.SetError("usage: hk-map-capture OUTPUT_DIR TID TIMEOUT_SECONDS")
        return
    output_dir = os.path.realpath(args[0])
    tid = int(args[1], 0)
    timeout_seconds = int(args[2], 0)
    os.makedirs(output_dir, exist_ok=True)
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    command_text = (
        "breakpoint set --name hb_jit_helper_exec_two_block_loop "
        "--one-shot true --thread-id 0x%x" % tid)
    command_result = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand(command_text, command_result)
    output = command_result.GetOutput() or ""
    error_text = command_result.GetError() or ""
    match = re.search(r"Breakpoint ([0-9]+):", output)
    control = {
        "schema": "macrunner.hk.lldb-map-control.v1",
        "breakpoint_command": command_text,
        "breakpoint_output": output,
        "breakpoint_error": error_text,
        "pid": process.GetProcessID(),
        "tid": tid,
        "timeout_seconds": timeout_seconds,
        "started_epoch": int(time.time()),
    }
    if not command_result.Succeeded() or not match:
        control["status"] = "UNKNOWN"
        control["reason"] = "breakpoint creation failed"
        _write_unknown(output_dir, process, process.GetSelectedThread(),
                       control["reason"])
    else:
        control["breakpoint_id"] = int(match.group(1))
        continue_state = {}

        def continue_target():
            continue_state["error"] = process.Continue()

        continue_thread = threading.Thread(target=continue_target, daemon=True)
        continue_thread.start()
        deadline = time.monotonic() + timeout_seconds
        hit_thread = lldb.SBThread()
        while time.monotonic() < deadline:
            state = process.GetState()
            if state == lldb.eStateStopped:
                for index in range(process.GetNumThreads()):
                    thread = process.GetThreadAtIndex(index)
                    if (thread.GetThreadID() == tid and
                            thread.GetStopReason() == lldb.eStopReasonBreakpoint):
                        hit_thread = thread
                        break
                if hit_thread.IsValid():
                    break
            if state in (lldb.eStateExited, lldb.eStateCrashed,
                         lldb.eStateDetached):
                break
            time.sleep(0.05)
        if hit_thread.IsValid():
            continue_thread.join(timeout=5)
            process.SetSelectedThread(hit_thread)
            hk_capture(debugger, shlex.quote(output_dir), exe_ctx, result, {})
            control["status"] = "HIT"
        else:
            control["status"] = "TIMEOUT"
            stop_error = process.Stop()
            control["stop_error"] = stop_error.GetCString()
            for _ in range(100):
                if process.GetState() == lldb.eStateStopped:
                    break
                time.sleep(0.05)
            continue_thread.join(timeout=5)
            _write_unknown(output_dir, process, process.GetSelectedThread(),
                           "attach-to-hit timeout")
        continue_error = continue_state.get("error")
        control["continue_error"] = (continue_error.GetCString()
                                     if continue_error else None)
        control["continue_thread_alive"] = continue_thread.is_alive()
    detach_error = process.Detach()
    control["detach_error"] = detach_error.GetCString()
    control["detach_success"] = detach_error.Success()
    control["finished_epoch"] = int(time.time())
    with open(os.path.join(output_dir, "map-control.json"), "w",
              encoding="utf-8") as stream:
        json.dump(control, stream, indent=2, sort_keys=True)
        stream.write("\n")
    result.AppendMessage("hk-map-capture status=%s detach=%s" %
                         (control["status"], control["detach_success"]))


def __lldb_init_module(debugger, internal_dict):
    del internal_dict
    debugger.HandleCommand(
        "command script add -f hk_lldb_capture.hk_capture hk-capture")
    debugger.HandleCommand(
        "command script add -f hk_lldb_capture.hk_map_capture hk-map-capture")
