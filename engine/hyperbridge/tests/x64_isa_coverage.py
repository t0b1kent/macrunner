#!/usr/bin/env python3
"""Bulk x86-64 decode/lift coverage against capstone."""

from __future__ import annotations

import argparse
import collections
import json
import random
import subprocess
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


HB_ROOT = Path(__file__).resolve().parents[1]
PROBE_C = HB_ROOT / "tests" / "hb_x64_probe.c"
PROBE_BIN = HB_ROOT / "tests" / "hb_x64_probe"
LIB = HB_ROOT / "libhyperbridge.a"
TAIL = bytes.fromhex("c0 7f 34 12 88 77 66 55 44 33 22 11 90 90 90")


@dataclass(frozen=True)
class Case:
    name: str
    group: str
    code: bytes


def run(cmd: list[str], cwd: Path) -> None:
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def build_probe() -> None:
    run(["make", "libhyperbridge.a"], HB_ROOT)
    needs_build = not PROBE_BIN.exists()
    if not needs_build:
        newest_input = max(PROBE_C.stat().st_mtime, LIB.stat().st_mtime)
        needs_build = PROBE_BIN.stat().st_mtime < newest_input
    if needs_build:
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I./include",
             str(PROBE_C.relative_to(HB_ROOT)), "libhyperbridge.a", "-o",
             str(PROBE_BIN.relative_to(HB_ROOT))], HB_ROOT)


def prefixes() -> list[tuple[str, bytes]]:
    return [
        ("none", b""),
        ("66", b"\x66"),
        ("67", b"\x67"),
        ("rexw", b"\x48"),
        ("rep", b"\xf3"),
        ("repne", b"\xf2"),
    ]


def generate_cases(random_count: int) -> list[Case]:
    cases: list[Case] = []
    for prefix_name, prefix in prefixes():
        for opcode in range(256):
            if opcode in (0x0f,):
                continue
            cases.append(Case(f"one-byte/{prefix_name}/{opcode:02x}",
                              "legacy one-byte", prefix + bytes([opcode]) + TAIL))
    for prefix_name, prefix in prefixes():
        for opcode in range(256):
            cases.append(Case(f"0f/{prefix_name}/{opcode:02x}",
                              "0f map", prefix + b"\x0f" + bytes([opcode]) + TAIL))
    for prefix_name, prefix in prefixes():
        for opcode in range(256):
            cases.append(Case(f"0f38/{prefix_name}/{opcode:02x}",
                              "0f38 map", prefix + b"\x0f\x38" + bytes([opcode]) + TAIL))
            cases.append(Case(f"0f3a/{prefix_name}/{opcode:02x}",
                              "0f3a map", prefix + b"\x0f\x3a" + bytes([opcode]) + TAIL))
    for opcode in range(0xd8, 0xe0):
        for modrm in range(0xc0, 0x100):
            cases.append(Case(f"x87/{opcode:02x}/{modrm:02x}",
                              "x87", bytes([opcode, modrm]) + TAIL))

    samples = {
        "vex/vzeroupper": "c5 f8 77",
        "vex/vpxor": "c5 f9 ef c0",
        "vex/vaddps": "c5 fc 58 c0",
        "vex/vaddpd": "c5 f5 58 c0",
        "vex/vsubps": "c5 fc 5c c0",
        "vex/vsubpd": "c5 f5 5c c2",
        "vex/vmulps": "c5 fc 59 c0",
        "vex/vmulpd": "c5 f5 59 da",
        "vex/vdivps": "c5 fc 5e c0",
        "vex/vdivpd": "c5 f5 5e e2",
        "vex/vminps": "c5 fc 5d c0",
        "vex/vminpd": "c5 f5 5d ea",
        "vex/vmaxps": "c5 fc 5f c0",
        "vex/vmaxpd": "c5 f5 5f f2",
        "vex/vsqrtps": "c5 fc 51 c1",
        "vex/vsqrtpd": "c5 fd 51 d1",
        "vex/vsqrtss": "c5 f2 51 c2",
        "vex/vsqrtsd": "c5 db 51 dd",
        "vex/vunpcklps": "c4 e1 70 14 c2",
        "vex/vunpckhps": "c4 e1 70 15 c2",
        "vex/vunpcklpd": "c4 e1 71 14 c2",
        "vex/vunpckhpd": "c4 e1 71 15 c2",
        "vex/vshufps": "c4 e1 70 c6 c2 1b",
        "vex/vshufpd": "c4 e1 71 c6 c2 1b",
        "vex/vhaddps": "c5 f3 7c c2",
        "vex/vhaddpd": "c5 f1 7c c2",
        "vex/vhsubps": "c5 f3 7d c2",
        "vex/vhsubpd": "c5 f1 7d c2",
        "vex/vaddsubps": "c5 f3 d0 c2",
        "vex/vaddsubpd": "c5 f1 d0 c2",
        "vex/vaddss": "c5 f2 58 c2",
        "vex/vaddsd": "c5 f3 58 c2",
        "vex/vsubss": "c5 f2 5c da",
        "vex/vsubsd": "c5 f3 5c da",
        "vex/vmulss": "c5 f2 59 e2",
        "vex/vmulsd": "c5 f3 59 e2",
        "vex/vdivss": "c5 f2 5e ea",
        "vex/vdivsd": "c5 f3 5e ea",
        "vex/vminss": "c5 f2 5d f2",
        "vex/vminsd": "c5 f3 5d f2",
        "vex/vmaxss": "c5 f2 5f fa",
        "vex/vmaxsd": "c5 f3 5f fa",
        "vex/vandps": "c5 fc 54 c0",
        "vex/vandpd": "c5 f5 54 c2",
        "vex/vandnps": "c5 fc 55 c0",
        "vex/vandnpd": "c5 f5 55 c2",
        "vex/vorps": "c5 fc 56 c0",
        "vex/vorpd": "c5 f5 56 c2",
        "vex/vxorps": "c5 fc 57 c0",
        "vex/vxorpd": "c5 fd 57 c0",
        "vex/vpand": "c5 f5 db c2",
        "vex/vpandn": "c5 f5 df c2",
        "vex/vpor": "c5 f5 eb c2",
        "vex/vpaddd": "c5 f5 fe c0",
        "vex/vpaddb": "c5 f5 fc c2",
        "vex/vpaddw": "c5 f5 fd c2",
        "vex/vpaddq": "c5 f5 d4 c2",
        "vex/vpsubb": "c5 f5 f8 c2",
        "vex/vpsubw": "c5 f5 f9 c2",
        "vex/vpsubd": "c5 f5 fa c0",
        "vex/vpsubq": "c5 f5 fb c2",
        "vex/vpsrlw": "c5 f5 d1 c2",
        "vex/vpsrld": "c5 f5 d2 c2",
        "vex/vpsrlq": "c5 f5 d3 c2",
        "vex/vpsrad": "c5 dd e2 dd",
        "vex/vpsllw": "c5 f5 f1 c2",
        "vex/vpslld": "c5 f5 f2 c2",
        "vex/vpsllq": "c5 f5 f3 f2",
        "vex/vpaddusb": "c5 f5 dc c2",
        "vex/vpaddusw": "c5 f5 dd c2",
        "vex/vpaddsb": "c5 f5 ec c2",
        "vex/vpaddsw": "c5 dd ed dd",
        "vex/vpavgb": "c5 f5 e0 f2",
        "vex/vpavgw": "c5 f5 e3 c2",
        "vex/vpmullw": "c5 f5 d5 c2",
        "vex/vpmulhw": "c5 f5 e5 c2",
        "vex/vpmulhuw": "c5 dd e4 dd",
        "vex/vpmaddwd": "c5 f5 f5 f2",
        "vex/vpmaddubsw": "c4 e2 75 04 c2",
        "vex/vpmuldq": "c4 e2 75 28 c2",
        "vex/vpmulld": "c4 e2 75 40 c2",
        "vex/vpcmpeqb": "c5 f5 74 c2",
        "vex/vpcmpeqw": "c5 f5 75 c2",
        "vex/vpcmpgtw": "c5 dd 65 dd",
        "vex/vpcmpeqd": "c5 f5 76 f2",
        "vex/vpcmpeqq": "c4 e2 75 29 c2",
        "vex/vpcmpgtb": "c5 f5 64 c2",
        "vex/vpcmpgtd": "c5 f5 66 c2",
        "vex/vpcmpgtq": "c4 e2 75 37 c2",
        "vex/vpunpcklbw": "c5 f1 60 c2",
        "vex/vpunpckhwd": "c5 f5 69 c2",
        "vex/vpunpckldq_mem": "c5 f5 62 05 00 00 00 00",
        "vex/vpacksswb": "c5 d1 63 de",
        "vex/vpackuswb": "c5 dd 67 dd",
        "vex/vpackssdw_mem": "c5 dd 6b 1d 00 00 00 00",
        "vex/vmovdqu": "c5 fa 6f c1",
        "vex/vmovdqa": "c5 fd 6f d1",
        "vex/vmovdqa_load": "c5 fd 6f 05 00 00 00 00",
        "vex/vmovdqa_store": "c5 fd 7f 05 00 00 00 00",
        "vex/vmovups": "c5 fc 10 c1",
        "vex/vmovaps_load": "c5 fc 28 05 00 00 00 00",
        "vex/vmovaps_store": "c5 fc 29 05 00 00 00 00",
        "vex/vmovupd": "c5 fd 10 d1",
        "vex/vmovapd": "c5 fd 28 d1",
        "vex/vmovntps": "c5 f8 2b 08",
        "vex/vmovntpd": "c5 f9 2b 08",
        "vex/vmovntdq": "c5 f9 e7 08",
        "vex/vlddqu": "c5 fb f0 00",
        "vex/vmovntdqa": "c4 e2 79 2a 00",
        "vex/vucomiss": "c5 f8 2e c1",
        "vex/vcomiss": "c5 f8 2f c1",
        "vex/vucomisd": "c5 f9 2e c1",
        "vex/vcomisd": "c5 f9 2f c1",
        "vex/vpshufd": "c5 f9 70 c1 1b",
        "vex/vpshufhw": "c5 fa 70 c1 1b",
        "vex/vpshuflw": "c5 fb 70 c1 1b",
        "vex/vmovsldup": "c5 fa 12 c1",
        "vex/vmovshdup": "c5 fa 16 c1",
        "vex/vmovddup": "c5 fb 12 c1",
        "vex/vcvtps2pd_xmm": "c5 f8 5a c1",
        "vex/vcvtps2pd_ymm": "c5 fc 5a c1",
        "vex/vcvtpd2ps_xmm": "c5 f9 5a c1",
        "vex/vcvtpd2ps_ymm": "c5 fd 5a c1",
        "vex/vcvtdq2ps_xmm": "c5 f8 5b c1",
        "vex/vcvtdq2ps_ymm": "c5 fc 5b c1",
        "vex/vcvtps2dq_xmm": "c5 f9 5b c1",
        "vex/vcvtps2dq_ymm": "c5 fd 5b c1",
        "vex/vcvttps2dq_xmm": "c5 fa 5b c1",
        "vex/vcvttps2dq_ymm": "c5 fe 5b c1",
        "vex/vcvtdq2pd_xmm": "c5 fa e6 c1",
        "vex/vcvtdq2pd_ymm": "c5 fe e6 c1",
        "vex/vcvtpd2dq_xmm": "c5 fb e6 c1",
        "vex/vcvtpd2dq_ymm": "c5 ff e6 c1",
        "vex/vcvttpd2dq_xmm": "c5 f9 e6 c1",
        "vex/vcvttpd2dq_ymm": "c5 fd e6 c1",
        "vex/vcvtss2sd": "c5 f2 5a c2",
        "vex/vcvtsd2ss": "c5 f3 5a c2",
        "vex/vcvtsi2ss_eax": "c5 f2 2a c0",
        "vex/vcvtsi2sd_eax": "c5 f3 2a c0",
        "vex/vcvtsi2ss_rax": "c4 e1 f2 2a c0",
        "vex/vcvtsi2sd_rax": "c4 e1 f3 2a c0",
        "vex/vcvttss2si_eax": "c5 fa 2c c1",
        "vex/vcvttsd2si_eax": "c5 fb 2c c1",
        "vex/vcvtss2si_eax": "c5 fa 2d c1",
        "vex/vcvtsd2si_eax": "c5 fb 2d c1",
        "vex/vcvttss2si_rax": "c4 e1 fa 2c c1",
        "vex/vcvttsd2si_rax": "c4 e1 fb 2c c1",
        "vex/vcvtss2si_rax": "c4 e1 fa 2d c1",
        "vex/vcvtsd2si_rax": "c4 e1 fb 2d c1",
        "vex/vrsqrtps_xmm": "c5 f8 52 c1",
        "vex/vrsqrtps_ymm": "c5 fc 52 c1",
        "vex/vrsqrtss": "c5 f2 52 c2",
        "vex/vrcpps_xmm": "c5 f8 53 c1",
        "vex/vrcpps_ymm": "c5 fc 53 c1",
        "vex/vrcpss": "c5 f2 53 c2",
        "vex/vroundps_xmm": "c4 e3 79 08 c1 03",
        "vex/vroundps_ymm": "c4 e3 7d 08 c1 03",
        "vex/vroundpd_xmm": "c4 e3 79 09 c1 03",
        "vex/vroundpd_ymm": "c4 e3 7d 09 c1 03",
        "vex/vroundss": "c4 e3 71 0a c2 03",
        "vex/vroundsd": "c4 e3 71 0b c2 03",
        "vex/vroundps_w1": "c4 e3 f9 08 c1 03",
        "vex/vroundpd_w1": "c4 e3 f9 09 c1 03",
        "vex/vroundss_w1": "c4 e3 f1 0a c2 03",
        "vex/vroundsd_w1": "c4 e3 f1 0b c2 03",
        "vex/vdpps_xmm": "c4 e3 71 40 c2 ff",
        "vex/vdpps_ymm": "c4 e3 75 40 c2 ff",
        "vex/vdppd_xmm": "c4 e3 71 41 c2 33",
        "vex/vinsertps": "c4 e3 71 21 c2 b4",
        "vex/vinsertps_w1": "c4 e3 f1 21 c2 7f",
        "vex/vextractps": "c4 e3 79 17 c1 02",
        "vex/vextractps_w1": "c4 e3 f9 17 c1 02",
        "vex/vpinsrb": "c4 e3 71 20 c1 02",
        "vex/vpinsrw": "c5 f1 c4 c1 02",
        "vex/vpinsrd": "c4 e3 71 22 c1 02",
        "vex/vpinsrq": "c4 e3 f1 22 c1 01",
        "vex/vpextrb": "c4 e3 79 14 c1 02",
        "vex/vpextrw": "c5 f9 c5 c1 02",
        "vex/vpextrd": "c4 e3 79 16 c1 02",
        "vex/vpextrq": "c4 e3 f9 16 c1 01",
        "vex/vgf2p8affineqb_xmm": "c4 e3 f1 ce c2 27",
        "vex/vgf2p8affineinvqb_xmm": "c4 e3 f1 cf c2 27",
        "vex/vgf2p8affineqb_ymm": "c4 e3 f5 ce c2 27",
        "vex/vgf2p8affineinvqb_ymm": "c4 e3 f5 cf c2 27",
        "vex/vmpsadbw": "c4 e3 71 42 c2 7f",
        "0f38/sha1nexte": "0f 38 c8 c1",
        "0f38/sha1msg1": "0f 38 c9 c1",
        "0f38/sha1msg2": "0f 38 ca c1",
        "0f38/sha256rnds2": "0f 38 cb d1",
        "0f38/sha256msg1": "0f 38 cc c1",
        "0f38/sha256msg2": "0f 38 cd c1",
        "0f3a/sha1rnds4": "0f 3a cc c1 02",
        "0f3a/mpsadbw": "66 0f 3a 42 c1 7f",
        "0f38/crc32_r8d_r9b": "f2 45 0f 38 f0 c1",
        "0f38/crc32_r8d_r9w": "66 f2 45 0f 38 f1 c1",
        "0f38/crc32_r8d_r9d": "f2 45 0f 38 f1 c1",
        "0f38/crc32_r8_r9": "f2 4d 0f 38 f1 c1",
        "0f38/movbe_dx_m16": "66 0f 38 f0 10",
        "0f38/movbe_edx_m32": "0f 38 f0 10",
        "0f38/movbe_rdx_m64": "48 0f 38 f0 10",
        "0f38/movbe_m16_dx": "66 0f 38 f1 10",
        "0f38/movbe_m32_edx": "0f 38 f1 10",
        "0f38/movbe_m64_rdx": "48 0f 38 f1 10",
        "0f38/movdiri_m32_edx": "0f 38 f9 10",
        "0f38/movdiri_66_m32_edx": "66 0f 38 f9 10",
        "0f38/movdiri_f2_m32_edx": "f2 0f 38 f9 10",
        "0f38/movdiri_f3_m32_edx": "f3 0f 38 f9 10",
        "0f38/movdiri_m64_rdx": "48 0f 38 f9 10",
        "0f38/movdiri_rex66_m32_edx": "48 66 0f 38 f9 10",
        "0f38/movdiri_rexf2_m32_edx": "48 f2 0f 38 f9 10",
        "0f38/movdiri_rexf3_m32_edx": "48 f3 0f 38 f9 10",
        "0f38/movdiri_66rex_m64_rdx": "66 48 0f 38 f9 10",
        "0f38/movdiri_f2rex_m64_rdx": "f2 48 0f 38 f9 10",
        "0f38/movdiri_f3rex_m64_rdx": "f3 48 0f 38 f9 10",
        "0f38/movntdqa_xmm0_m128": "66 0f 38 2a 00",
        "0f38/movntdqa_rex66_xmm0_m128": "48 66 0f 38 2a 00",
        "0f38/movntdqa_66rex_xmm0_m128": "66 48 0f 38 2a 00",
        "0f38/movdir64b_rdi_m512_rsi": "66 0f 38 f8 3e",
        "0f38/movdir64b_rex66_rdi_m512_rsi": "48 66 0f 38 f8 3e",
        "0f38/movdir64b_66rex_rdi_m512_rsi": "66 48 0f 38 f8 3e",
        "0f38/adcx_r8_r9": "66 4d 0f 38 f6 c1",
        "0f38/adox_r8_r9": "f3 4d 0f 38 f6 c1",
        "vex/andn_eax_ecx_edx": "c4 e2 70 f2 c2",
        "vex/andn_rax_rcx_rdx": "c4 e2 f0 f2 c2",
        "vex/bextr_eax_edx_ecx": "c4 e2 70 f7 c2",
        "vex/bextr_rax_rdx_rcx": "c4 e2 f0 f7 c2",
        "vex/blsi_eax_edx": "c4 e2 78 f3 da",
        "vex/blsi_rax_rdx": "c4 e2 f8 f3 da",
        "vex/blsmsk_eax_edx": "c4 e2 78 f3 d2",
        "vex/blsmsk_rax_rdx": "c4 e2 f8 f3 d2",
        "vex/blsr_eax_edx": "c4 e2 78 f3 ca",
        "vex/blsr_rax_rdx": "c4 e2 f8 f3 ca",
        "vex/bzhi_eax_edx_ecx": "c4 e2 70 f5 c2",
        "vex/bzhi_rax_rdx_rcx": "c4 e2 f0 f5 c2",
        "vex/pext_eax_ecx_edx": "c4 e2 72 f5 c2",
        "vex/pdep_eax_ecx_edx": "c4 e2 73 f5 c2",
        "vex/mulx_eax_ecx_edx": "c4 e2 73 f6 c2",
        "vex/mulx_rax_rcx_rdx": "c4 e2 f3 f6 c2",
        "vex/sarx_eax_edx_ecx": "c4 e2 72 f7 c2",
        "vex/sarx_rax_rdx_rcx": "c4 e2 f2 f7 c2",
        "vex/shlx_eax_edx_ecx": "c4 e2 71 f7 c2",
        "vex/shlx_rax_rdx_rcx": "c4 e2 f1 f7 c2",
        "vex/shrx_eax_edx_ecx": "c4 e2 73 f7 c2",
        "vex/shrx_rax_rdx_rcx": "c4 e2 f3 f7 c2",
        "vex/rorx_eax_edx_1": "c4 e3 7b f0 c2 01",
        "vex/rorx_rax_rdx_1": "c4 e3 fb f0 c2 01",
        "vex/vmovss": "c5 f2 10 c2",
        "vex/vmovsd": "c5 f3 10 c2",
        "vex/vmovss_load": "c5 fa 10 05 00 00 00 00",
        "vex/vmovss_store": "c5 fa 11 05 00 00 00 00",
        "vex/vmovsd_load": "c5 fb 10 05 00 00 00 00",
        "vex/vmovsd_store": "c5 fb 11 05 00 00 00 00",
        "vex/vpsubusb": "c5 f5 d8 c0",
        "vex/vpsubusw": "c5 f5 d9 c2",
        "vex/vpsubsb": "c5 f5 e8 c2",
        "vex/vpsubsw": "c5 f5 e9 c2",
        "vex/vpminub": "c5 f5 da c0",
        "vex/vpminsw": "c5 f5 ea c2",
        "vex/vpmaxub": "c5 f5 de c2",
        "vex/vpmaxsw": "c5 f5 ee c2",
        "vex/vpminsb": "c4 e2 75 38 c2",
        "vex/vpminsd": "c4 e2 75 39 c2",
        "vex/vpminuw": "c4 e2 75 3a c2",
        "vex/vpminud": "c4 e2 75 3b c2",
        "vex/vpmaxsb": "c4 e2 75 3c c2",
        "vex/vpmaxsd": "c4 e2 75 3d c2",
        "vex/vpmaxuw": "c4 e2 75 3e c2",
        "vex/vpmaxud": "c4 e2 75 3f c2",
        "vex/vpmuludq": "c5 f5 f4 c0",
        "vex/vpsadbw": "c5 f5 f6 c0",
        "vex/vpbroadcastb": "c4 e2 79 78 c1",
        "vex/vpbroadcastw": "c4 e2 7d 79 d3",
        "vex/vpbroadcastd": "c4 e2 7d 58 e5",
        "vex/vpbroadcastq": "c4 e2 7d 59 f7",
        "vex/vbroadcastss_xmm_m32": "c4 e2 79 18 00",
        "vex/vbroadcastss_ymm_m32": "c4 e2 7d 18 00",
        "vex/vbroadcastss_ymm_xmm": "c4 e2 7d 18 c1",
        "vex/vbroadcastsd_ymm_m64": "c4 e2 7d 19 00",
        "vex/vbroadcastsd_ymm_xmm": "c4 e2 7d 19 c1",
        "vex/vbroadcastf128": "c4 e2 7d 1a 00",
        "vex/vpsrlw_xmm_imm": "c5 f1 71 d0 02",
        "vex/vpsrlw_ymm_imm": "c5 f5 71 d0 02",
        "vex/vpsraw_xmm_imm": "c5 f1 71 e0 02",
        "vex/vpsraw_ymm_imm": "c5 f5 71 e0 02",
        "vex/vpsllw_xmm_imm": "c5 f1 71 f0 02",
        "vex/vpsllw_ymm_imm": "c5 f5 71 f0 02",
        "vex/vpsrld_xmm_imm": "c5 f1 72 d0 02",
        "vex/vpsrld_ymm_imm": "c5 f5 72 d0 02",
        "vex/vpsrad_xmm_imm": "c5 f1 72 e0 02",
        "vex/vpsrad_ymm_imm": "c5 f5 72 e0 02",
        "vex/vpslld_xmm_imm": "c5 f1 72 f0 02",
        "vex/vpslld_ymm_imm": "c5 f5 72 f0 02",
        "vex/vpsrlq_xmm_imm": "c5 f1 73 d0 02",
        "vex/vpsrlq_ymm_imm": "c5 f5 73 d0 02",
        "vex/vpsrldq_xmm_imm": "c5 f1 73 d8 02",
        "vex/vpsrldq_ymm_imm": "c5 f5 73 d8 02",
        "vex/vpsllq_xmm_imm": "c5 f1 73 f0 02",
        "vex/vpsllq_ymm_imm": "c5 f5 73 f0 02",
        "vex/vpslldq_xmm_imm": "c5 f1 73 f8 02",
        "vex/vpslldq_ymm_imm": "c5 f5 73 f8 02",
        "vex/vpsrlw_xmm_xmm": "c5 f1 d1 c2",
        "vex/vpsrlw_ymm_xmm": "c5 f5 d1 c2",
        "vex/vpsraw_xmm_xmm": "c5 f1 e1 c2",
        "vex/vpsraw_ymm_xmm": "c5 f5 e1 c2",
        "vex/vpsllw_xmm_xmm": "c5 f1 f1 c2",
        "vex/vpsllw_ymm_xmm": "c5 f5 f1 c2",
        "vex/vpsrld_xmm_xmm": "c5 f1 d2 c2",
        "vex/vpsrld_ymm_xmm": "c5 f5 d2 c2",
        "vex/vpsrad_xmm_xmm": "c5 f1 e2 c2",
        "vex/vpsrad_ymm_xmm": "c5 f5 e2 c2",
        "vex/vpslld_xmm_xmm": "c5 f1 f2 c2",
        "vex/vpslld_ymm_xmm": "c5 f5 f2 c2",
        "vex/vpsrlq_xmm_xmm": "c5 f1 d3 c2",
        "vex/vpsrlq_ymm_xmm": "c5 f5 d3 c2",
        "vex/vpsllq_xmm_xmm": "c5 f1 f3 c2",
        "vex/vpsllq_ymm_xmm": "c5 f5 f3 c2",
        "vex/vmovlps_load": "c4 e1 70 12 00",
        "vex/vmovhps_load": "c4 e1 70 16 00",
        "vex/vmovlpd_load": "c4 e1 71 12 00",
        "vex/vmovhpd_load": "c4 e1 71 16 00",
        "vex/vmovlps_store": "c5 f8 13 00",
        "vex/vmovhps_store": "c5 f8 17 00",
        "vex/vmovlpd_store": "c5 f9 13 00",
        "vex/vmovhpd_store": "c5 f9 17 00",
        "vex/vbroadcasti128": "c4 e2 7d 5a 10",
        "vex/vpblendd": "c4 e3 75 02 c2 aa",
        "vex/vpermilps_imm": "c4 e3 79 04 c1 1b",
        "vex/vpermilpd_imm": "c4 e3 79 05 c1 1b",
        "vex/vpermilps_var": "c4 e2 71 0c c2",
        "vex/vpermilpd_var": "c4 e2 71 0d c2",
        "vex/vblendvps": "c4 e3 69 4a cb 10",
        "vex/vblendvpd": "c4 e3 69 4b cb 10",
        "vex/vpblendvb": "c4 e3 69 4c cb 10",
        "vex/vcmpps": "c5 f0 c2 c2 01",
        "vex/vcmppd": "c5 f1 c2 c2 01",
        "vex/vcmpss": "c5 f2 c2 c2 01",
        "vex/vcmpsd": "c5 f3 c2 c2 01",
        "vex/vcmpps_w1": "c4 e1 f0 c2 c2 01",
        "vex/vcmppd_w1": "c4 e1 f1 c2 c2 01",
        "vex/vcmpss_w1": "c4 e1 f2 c2 c2 01",
        "vex/vcmpsd_w1": "c4 e1 f3 c2 c2 01",
        "vex/vmovd_xmm_gpr": "c4 e1 79 6e c1",
        "vex/vmovq_xmm_gpr": "c4 e1 f9 6e c1",
        "vex/vmovd_gpr_xmm": "c4 e1 79 7e c1",
        "vex/vmovq_gpr_xmm": "c4 e1 f9 7e c1",
        "vex/vmovq_f3_xmm_xmm": "c4 e1 7a 7e c1",
        "vex/vmovq_d6_xmm_xmm": "c4 e1 79 d6 c1",
        "vex/vmovd_m32_xmm": "c4 e1 79 7e 00",
        "vex/vmovq_m64_xmm": "c4 e1 f9 7e 00",
        "vex/vmovq_f3_xmm_m64": "c4 e1 7a 7e 00",
        "vex/vmovq_d6_m64_xmm": "c4 e1 f9 d6 00",
        "vex/vmovmskps": "c4 e1 78 50 c1",
        "vex/vmovmskpd": "c4 e1 79 50 c1",
        "vex/vpmovmskb": "c4 e1 79 d7 c1",
        "vex/vtestps_xmm": "c4 e2 79 0e c1",
        "vex/vtestps_ymm": "c4 e2 7d 0e c1",
        "vex/vtestpd_xmm": "c4 e2 79 0f c1",
        "vex/vtestpd_ymm": "c4 e2 7d 0f c1",
        "vex/vmaskmovdqu": "c4 e1 79 f7 c1",
        "vex/vldmxcsr": "c4 e1 78 ae 10",
        "vex/vstmxcsr": "c4 e1 78 ae 18",
        "0f38/pshufb_f2rex": "f2 48 0f 38 00 c1",
        "0f38/phaddw_f2rex": "f2 48 0f 38 01 c1",
        "0f38/pabsd_f2rex": "f2 48 0f 38 1e c1",
        "0f38/pshufb_f3rex": "f3 48 0f 38 00 c1",
        "0f38/phaddw_f3rex": "f3 48 0f 38 01 c1",
        "0f38/pabsd_f3rex": "f3 48 0f 38 1e c1",
        "0f38/movbe_f3rex_load": "f3 48 0f 38 f0 10",
        "0f38/movbe_f3rex_store": "f3 48 0f 38 f1 10",
        "0f3a/palignr_f2rex": "f2 48 0f 3a 0f c1 04",
        "0f3a/palignr_f3rex": "f3 48 0f 3a 0f c1 04",
        "vex/vmovhlps": "c4 e1 70 12 c2",
        "vex/vmovhlps_w1": "c4 e1 f0 12 c2",
        "vex/vmovlhps": "c4 e1 70 16 c2",
        "vex/vmovlhps_w1": "c4 e1 f0 16 c2",
        "vex/vfmaddsub132ps": "c4 e2 75 96 c2",
        "vex/vfmaddsub132pd": "c4 e2 f5 96 c2",
        "vex/vfmsubadd132ps": "c4 e2 75 97 c2",
        "vex/vfmsubadd132pd": "c4 e2 f5 97 c2",
        "vex/vfmaddsub213ps": "c4 e2 75 a6 c2",
        "vex/vfmaddsub213pd": "c4 e2 f5 a6 c2",
        "vex/vfmsubadd213ps": "c4 e2 75 a7 c2",
        "vex/vfmsubadd213pd": "c4 e2 f5 a7 c2",
        "vex/vfmaddsub231ps": "c4 e2 75 b6 c2",
        "vex/vfmaddsub231pd": "c4 e2 f5 b6 c2",
        "vex/vfmsubadd231ps": "c4 e2 75 b7 c2",
        "vex/vfmsubadd231pd": "c4 e2 f5 b7 c2",
        "vex/vfnmadd132ps": "c4 e2 75 9c c2",
        "vex/vfnmadd132pd": "c4 e2 f5 9c c2",
        "vex/vfnmadd132ss": "c4 e2 71 9d c2",
        "vex/vfnmadd132sd": "c4 e2 f1 9d c2",
        "vex/vfnmsub132ps": "c4 e2 75 9e c2",
        "vex/vfnmsub132pd": "c4 e2 f5 9e c2",
        "vex/vfnmsub132ss": "c4 e2 71 9f c2",
        "vex/vfnmsub132sd": "c4 e2 f1 9f c2",
        "vex/vfnmadd213ps": "c4 e2 75 ac c2",
        "vex/vfnmadd213pd": "c4 e2 f5 ac c2",
        "vex/vfnmadd213ss": "c4 e2 71 ad c2",
        "vex/vfnmadd213sd": "c4 e2 f1 ad c2",
        "vex/vfnmsub213ps": "c4 e2 75 ae c2",
        "vex/vfnmsub213pd": "c4 e2 f5 ae c2",
        "vex/vfnmsub213ss": "c4 e2 71 af c2",
        "vex/vfnmsub213sd": "c4 e2 f1 af c2",
        "vex/vfnmadd231ps": "c4 e2 75 bc c2",
        "vex/vfnmadd231pd": "c4 e2 f5 bc c2",
        "vex/vfnmadd231ss": "c4 e2 71 bd c2",
        "vex/vfnmadd231sd": "c4 e2 f1 bd c2",
        "vex/vfnmsub231ps": "c4 e2 75 be c2",
        "vex/vfnmsub231pd": "c4 e2 f5 be c2",
        "vex/vfnmsub231ss": "c4 e2 71 bf c2",
        "vex/vfnmsub231sd": "c4 e2 f1 bf c2",
        "vex/vfmadd132ss_l1": "c4 e2 75 99 c2",
        "vex/vfmsub132ss_l1": "c4 e2 75 9b c2",
        "vex/vfnmadd132ss_l1": "c4 e2 75 9d c2",
        "vex/vfnmsub132ss_l1": "c4 e2 75 9f c2",
        "vex/vfmadd213ss_l1": "c4 e2 75 a9 c2",
        "vex/vfmsub213ss_l1": "c4 e2 75 ab c2",
        "vex/vfnmadd213ss_l1": "c4 e2 75 ad c2",
        "vex/vfnmsub213ss_l1": "c4 e2 75 af c2",
        "vex/vfmadd231ss_l1": "c4 e2 75 b9 c2",
        "vex/vfmsub231ss_l1": "c4 e2 75 bb c2",
        "vex/vfnmadd231ss_l1": "c4 e2 75 bd c2",
        "vex/vfnmsub231ss_l1": "c4 e2 75 bf c2",
        "vex/vfmadd132sd_l1": "c4 e2 f5 99 c2",
        "vex/vfmsub132sd_l1": "c4 e2 f5 9b c2",
        "vex/vfnmadd132sd_l1": "c4 e2 f5 9d c2",
        "vex/vfnmsub132sd_l1": "c4 e2 f5 9f c2",
        "vex/vfmadd213sd_l1": "c4 e2 f5 a9 c2",
        "vex/vfmsub213sd_l1": "c4 e2 f5 ab c2",
        "vex/vfnmadd213sd_l1": "c4 e2 f5 ad c2",
        "vex/vfnmsub213sd_l1": "c4 e2 f5 af c2",
        "vex/vfmadd231sd_l1": "c4 e2 f5 b9 c2",
        "vex/vfmsub231sd_l1": "c4 e2 f5 bb c2",
        "vex/vfnmadd231sd_l1": "c4 e2 f5 bd c2",
        "vex/vfnmsub231sd_l1": "c4 e2 f5 bf c2",
        "vex/vpermq": "c4 e3 fd 00 c2 1b",
        "vex/vpermpd": "c4 e3 fd 01 c2 1b",
        "vex/vinsertf128": "c4 e3 75 18 c2 01",
        "vex/vinserti128": "c4 e3 75 38 c2 01",
        "vex/vextractf128": "c4 e3 7d 19 c1 01",
        "vex/vextracti128": "c4 e3 7d 39 c1 01",
        "vex/vperm2f128": "c4 e3 75 06 c2 31",
        "vex/vperm2i128": "c4 e3 75 46 c2 31",
        "vex/vpsrlvd": "c4 e2 71 45 c2",
        "vex/vpsrlvq": "c4 e2 f5 45 c2",
        "vex/vpsravd": "c4 e2 71 46 c2",
        "vex/vpsllvd": "c4 e2 71 47 c2",
        "vex/vpsllvq": "c4 e2 f5 47 c2",
        "vex/vpclmulqdq": "c4 e3 71 44 c2 10",
        "vex/vaeskeygenassist": "c4 e3 79 df c2 1b",
        "vex/vpermps": "c4 e2 75 16 c2",
        "vex/vpermd": "c4 e2 75 36 c2",
        "vex/vmaskmovps_load": "c4 e2 6d 2c 08",
        "vex/vmaskmovpd_store": "c4 e2 49 2f 68 20",
        "vex/vpmaskmovd_load": "c4 e2 75 8c 18",
        "vex/vpmaskmovq_store": "c4 e2 dd 8e 98 80 00 00 00",
        "vex/vpgatherdd": "c4 e2 5d 90 1c 90",
        "vex/vpgatherdq": "c4 e2 dd 90 1c 90",
        "vex/vpgatherqd": "c4 e2 5d 91 1c 90",
        "vex/vpgatherqq": "c4 e2 dd 91 1c 90",
        "vex/vgatherdps": "c4 e2 5d 92 1c 90",
        "vex/vgatherdpd": "c4 e2 dd 92 1c 90",
        "vex/vgatherqps": "c4 e2 5d 93 1c 90",
        "vex/vgatherqpd": "c4 e2 dd 93 1c 90",
        "vex/vaesimc": "c4 e2 79 db c1",
        "vex/vaesenc": "c4 e2 75 dc c2",
        "vex/vaesenclast": "c4 e2 75 dd c2",
        "vex/vaesdec": "c4 e2 75 de c2",
        "vex/vaesdeclast": "c4 e2 75 df c2",
        "vex/vgf2p8mulb": "c4 e2 75 cf c2",
        "vex/vmovq": "c4 e1 f9 6e c0",
        "evex/vaddps": "62 f1 7c 48 58 c0",
        "evex/vaddps_k1": "62 f1 7c 49 58 c0",
        "evex/vaddps_k1z": "62 f1 7c c9 58 c0",
        "evex/vaddps_rn_sae": "62 f1 7c 18 58 c0",
        "evex/vaddps_k1_rn_sae": "62 f1 7c 19 58 c0",
        "evex/vaddps_k1z_rn_sae": "62 f1 7c 99 58 c0",
        "evex/vaddps_rd_sae": "62 f1 7c 38 58 c0",
        "evex/vaddps_ru_sae": "62 f1 7c 58 58 c0",
        "evex/vaddps_rz_sae": "62 f1 7c 78 58 c0",
        "evex/vaddps_k1_rd_sae": "62 f1 7c 39 58 c0",
        "evex/vaddps_k1_ru_sae": "62 f1 7c 59 58 c0",
        "evex/vaddps_k1_rz_sae": "62 f1 7c 79 58 c0",
        "evex/vaddps_k1z_rd_sae": "62 f1 7c b9 58 c0",
        "evex/vaddps_k1z_ru_sae": "62 f1 7c d9 58 c0",
        "evex/vaddps_k1z_rz_sae": "62 f1 7c f9 58 c0",
        "evex/vaddps_m32bcst": "62 f1 7c 58 58 00",
        "evex/vaddps_k1_m32bcst": "62 f1 7c 59 58 00",
        "evex/vaddps_k1z_m32bcst": "62 f1 7c d9 58 00",
        "evex/vsubps_m32bcst": "62 f1 7c 58 5c 00",
        "evex/vsubps_k1_m32bcst": "62 f1 7c 59 5c 00",
        "evex/vsubps_k1z_m32bcst": "62 f1 7c d9 5c 00",
        "evex/vmulps_m32bcst": "62 f1 7c 58 59 00",
        "evex/vmulps_k1_m32bcst": "62 f1 7c 59 59 00",
        "evex/vmulps_k1z_m32bcst": "62 f1 7c d9 59 00",
        "evex/vdivps_m32bcst": "62 f1 7c 58 5e 00",
        "evex/vdivps_k1_m32bcst": "62 f1 7c 59 5e 00",
        "evex/vdivps_k1z_m32bcst": "62 f1 7c d9 5e 00",
        "evex/vaddpd_m64bcst": "62 f1 fd 58 58 00",
        "evex/vaddpd_k1_m64bcst": "62 f1 fd 59 58 00",
        "evex/vaddpd_k1z_m64bcst": "62 f1 fd d9 58 00",
        "evex/vsubpd_m64bcst": "62 f1 fd 58 5c 00",
        "evex/vsubpd_k1_m64bcst": "62 f1 fd 59 5c 00",
        "evex/vsubpd_k1z_m64bcst": "62 f1 fd d9 5c 00",
        "evex/vmulpd_m64bcst": "62 f1 fd 58 59 00",
        "evex/vmulpd_k1_m64bcst": "62 f1 fd 59 59 00",
        "evex/vmulpd_k1z_m64bcst": "62 f1 fd d9 59 00",
        "evex/vdivpd_m64bcst": "62 f1 fd 58 5e 00",
        "evex/vdivpd_k1_m64bcst": "62 f1 fd 59 5e 00",
        "evex/vdivpd_k1z_m64bcst": "62 f1 fd d9 5e 00",
        "evex/vmulps": "62 f1 7c 48 59 c0",
        "evex/vmulps_k1": "62 f1 7c 49 59 c0",
        "evex/vmulps_k1z": "62 f1 7c c9 59 c0",
        "evex/vsubps": "62 f1 7c 48 5c c0",
        "evex/vsubps_k1": "62 f1 7c 49 5c c0",
        "evex/vsubps_k1z": "62 f1 7c c9 5c c0",
        "evex/vdivps": "62 f1 7c 48 5e c0",
        "evex/vdivps_k1": "62 f1 7c 49 5e c0",
        "evex/vdivps_k1z": "62 f1 7c c9 5e c0",
        "evex/vaddpd": "62 f1 fd 48 58 c0",
        "evex/vaddpd_k1": "62 f1 fd 49 58 c0",
        "evex/vaddpd_k1z": "62 f1 fd c9 58 c0",
        "evex/vmulpd": "62 f1 fd 48 59 c0",
        "evex/vmulpd_k1": "62 f1 fd 49 59 c0",
        "evex/vmulpd_k1z": "62 f1 fd c9 59 c0",
        "evex/vsubpd": "62 f1 fd 48 5c c0",
        "evex/vsubpd_k1": "62 f1 fd 49 5c c0",
        "evex/vsubpd_k1z": "62 f1 fd c9 5c c0",
        "evex/vdivpd": "62 f1 fd 48 5e c0",
        "evex/vdivpd_k1": "62 f1 fd 49 5e c0",
        "evex/vdivpd_k1z": "62 f1 fd c9 5e c0",
        "evex/vsqrtps": "62 f1 7c 48 51 c1",
        "evex/vsqrtps_k1": "62 f1 7c 49 51 c1",
        "evex/vsqrtps_k1z": "62 f1 7c c9 51 c1",
        "evex/vsqrtpd": "62 f1 fd 48 51 c1",
        "evex/vsqrtpd_k1": "62 f1 fd 49 51 c1",
        "evex/vsqrtpd_k1z": "62 f1 fd c9 51 c1",
        "evex/vminps": "62 f1 7c 48 5d c1",
        "evex/vminps_k1": "62 f1 7c 49 5d c1",
        "evex/vminps_k1z": "62 f1 7c c9 5d c1",
        "evex/vmaxps": "62 f1 7c 48 5f c1",
        "evex/vmaxps_k1": "62 f1 7c 49 5f c1",
        "evex/vmaxps_k1z": "62 f1 7c c9 5f c1",
        "evex/vminpd": "62 f1 fd 48 5d c1",
        "evex/vminpd_k1": "62 f1 fd 49 5d c1",
        "evex/vminpd_k1z": "62 f1 fd c9 5d c1",
        "evex/vmaxpd": "62 f1 fd 48 5f c1",
        "evex/vmaxpd_k1": "62 f1 fd 49 5f c1",
        "evex/vmaxpd_k1z": "62 f1 fd c9 5f c1",
        "evex/vminps_m32bcst": "62 f1 7c 58 5d 00",
        "evex/vminps_k1_m32bcst": "62 f1 7c 59 5d 00",
        "evex/vminps_k1z_m32bcst": "62 f1 7c d9 5d 00",
        "evex/vmaxps_m32bcst": "62 f1 7c 58 5f 00",
        "evex/vmaxps_k1_m32bcst": "62 f1 7c 59 5f 00",
        "evex/vmaxps_k1z_m32bcst": "62 f1 7c d9 5f 00",
        "evex/vminpd_m64bcst": "62 f1 fd 58 5d 00",
        "evex/vminpd_k1_m64bcst": "62 f1 fd 59 5d 00",
        "evex/vminpd_k1z_m64bcst": "62 f1 fd d9 5d 00",
        "evex/vmaxpd_m64bcst": "62 f1 fd 58 5f 00",
        "evex/vmaxpd_k1_m64bcst": "62 f1 fd 59 5f 00",
        "evex/vmaxpd_k1z_m64bcst": "62 f1 fd d9 5f 00",
        "evex/vandps": "62 f1 7c 48 54 c1",
        "evex/vandps_k1": "62 f1 7c 49 54 c1",
        "evex/vandps_k1z": "62 f1 7c c9 54 c1",
        "evex/vandnps": "62 f1 7c 48 55 c1",
        "evex/vandnps_k1": "62 f1 7c 49 55 c1",
        "evex/vandnps_k1z": "62 f1 7c c9 55 c1",
        "evex/vorps": "62 f1 7c 48 56 c1",
        "evex/vorps_k1": "62 f1 7c 49 56 c1",
        "evex/vorps_k1z": "62 f1 7c c9 56 c1",
        "evex/vxorps": "62 f1 7c 48 57 c1",
        "evex/vxorps_k1": "62 f1 7c 49 57 c1",
        "evex/vxorps_k1z": "62 f1 7c c9 57 c1",
        "evex/vandpd": "62 f1 fd 48 54 c1",
        "evex/vandpd_k1": "62 f1 fd 49 54 c1",
        "evex/vandpd_k1z": "62 f1 fd c9 54 c1",
        "evex/vandnpd": "62 f1 fd 48 55 c1",
        "evex/vandnpd_k1": "62 f1 fd 49 55 c1",
        "evex/vandnpd_k1z": "62 f1 fd c9 55 c1",
        "evex/vorpd": "62 f1 fd 48 56 c1",
        "evex/vorpd_k1": "62 f1 fd 49 56 c1",
        "evex/vorpd_k1z": "62 f1 fd c9 56 c1",
        "evex/vxorpd": "62 f1 fd 48 57 c1",
        "evex/vxorpd_k1": "62 f1 fd 49 57 c1",
        "evex/vxorpd_k1z": "62 f1 fd c9 57 c1",
        "evex/vandps_m32bcst": "62 f1 7c 58 54 00",
        "evex/vandps_k1_m32bcst": "62 f1 7c 59 54 00",
        "evex/vandps_k1z_m32bcst": "62 f1 7c d9 54 00",
        "evex/vandnps_m32bcst": "62 f1 7c 58 55 00",
        "evex/vandnps_k1_m32bcst": "62 f1 7c 59 55 00",
        "evex/vandnps_k1z_m32bcst": "62 f1 7c d9 55 00",
        "evex/vorps_m32bcst": "62 f1 7c 58 56 00",
        "evex/vorps_k1_m32bcst": "62 f1 7c 59 56 00",
        "evex/vorps_k1z_m32bcst": "62 f1 7c d9 56 00",
        "evex/vxorps_m32bcst": "62 f1 7c 58 57 00",
        "evex/vxorps_k1_m32bcst": "62 f1 7c 59 57 00",
        "evex/vxorps_k1z_m32bcst": "62 f1 7c d9 57 00",
        "evex/vandpd_m64bcst": "62 f1 fd 58 54 00",
        "evex/vandpd_k1_m64bcst": "62 f1 fd 59 54 00",
        "evex/vandpd_k1z_m64bcst": "62 f1 fd d9 54 00",
        "evex/vandnpd_m64bcst": "62 f1 fd 58 55 00",
        "evex/vandnpd_k1_m64bcst": "62 f1 fd 59 55 00",
        "evex/vandnpd_k1z_m64bcst": "62 f1 fd d9 55 00",
        "evex/vorpd_m64bcst": "62 f1 fd 58 56 00",
        "evex/vorpd_k1_m64bcst": "62 f1 fd 59 56 00",
        "evex/vorpd_k1z_m64bcst": "62 f1 fd d9 56 00",
        "evex/vxorpd_m64bcst": "62 f1 fd 58 57 00",
        "evex/vxorpd_k1_m64bcst": "62 f1 fd 59 57 00",
        "evex/vxorpd_k1z_m64bcst": "62 f1 fd d9 57 00",
        "evex/vunpcklps": "62 f1 7c 48 14 c1",
        "evex/vunpcklps_k1": "62 f1 7c 49 14 c1",
        "evex/vunpcklps_k1z": "62 f1 7c c9 14 c1",
        "evex/vunpckhps": "62 f1 7c 48 15 c1",
        "evex/vunpckhps_k1": "62 f1 7c 49 15 c1",
        "evex/vunpckhps_k1z": "62 f1 7c c9 15 c1",
        "evex/vunpcklpd": "62 f1 fd 48 14 c1",
        "evex/vunpcklpd_k1": "62 f1 fd 49 14 c1",
        "evex/vunpcklpd_k1z": "62 f1 fd c9 14 c1",
        "evex/vunpckhpd": "62 f1 fd 48 15 c1",
        "evex/vunpckhpd_k1": "62 f1 fd 49 15 c1",
        "evex/vunpckhpd_k1z": "62 f1 fd c9 15 c1",
        "evex/vshufps": "62 f1 7c 48 c6 c1 1b",
        "evex/vshufps_k1": "62 f1 7c 49 c6 c1 1b",
        "evex/vshufps_k1z": "62 f1 7c c9 c6 c1 1b",
        "evex/vshufpd": "62 f1 fd 48 c6 c1 1b",
        "evex/vshufpd_k1": "62 f1 fd 49 c6 c1 1b",
        "evex/vshufpd_k1z": "62 f1 fd c9 c6 c1 1b",
        "evex/vmovups": "62 f1 7c 48 10 c1",
        "evex/vmovups_k1": "62 f1 7c 49 10 c1",
        "evex/vmovups_k1z": "62 f1 7c c9 10 c1",
        "evex/vmovups_k1_m512": "62 f1 7c 49 10 00",
        "evex/vmovups_k1z_m512": "62 f1 7c c9 10 00",
        "evex/vmovups_m512_k1": "62 f1 7c 49 11 08",
        "evex/vmovaps": "62 f1 7c 48 28 c1",
        "evex/vmovaps_k1": "62 f1 7c 49 28 c1",
        "evex/vmovaps_k1z": "62 f1 7c c9 28 c1",
        "evex/vmovaps_k1_m512": "62 f1 7c 49 28 00",
        "evex/vmovaps_k1z_m512": "62 f1 7c c9 28 00",
        "evex/vmovaps_m512_k1": "62 f1 7c 49 29 08",
        "evex/vmovupd": "62 f1 fd 48 10 c1",
        "evex/vmovupd_k1": "62 f1 fd 49 10 c1",
        "evex/vmovupd_k1z": "62 f1 fd c9 10 c1",
        "evex/vmovupd_k1_m512": "62 f1 fd 49 10 00",
        "evex/vmovupd_k1z_m512": "62 f1 fd c9 10 00",
        "evex/vmovupd_m512_k1": "62 f1 fd 49 11 08",
        "evex/vmovapd": "62 f1 fd 48 28 c1",
        "evex/vmovapd_k1": "62 f1 fd 49 28 c1",
        "evex/vmovapd_k1z": "62 f1 fd c9 28 c1",
        "evex/vmovapd_k1_m512": "62 f1 fd 49 28 00",
        "evex/vmovapd_k1z_m512": "62 f1 fd c9 28 00",
        "evex/vmovapd_m512_k1": "62 f1 fd 49 29 08",
        "evex/vpandd": "62 f1 7d 48 db c1",
        "evex/vpandd_k1": "62 f1 7d 49 db c1",
        "evex/vpandd_k1z": "62 f1 7d c9 db c1",
        "evex/vpandnd": "62 f1 7d 48 df c1",
        "evex/vpandnd_k1": "62 f1 7d 49 df c1",
        "evex/vpandnd_k1z": "62 f1 7d c9 df c1",
        "evex/vpord": "62 f1 7d 48 eb c1",
        "evex/vpord_k1": "62 f1 7d 49 eb c1",
        "evex/vpord_k1z": "62 f1 7d c9 eb c1",
        "evex/vpxord": "62 f1 7d 48 ef c1",
        "evex/vpxord_k1": "62 f1 7d 49 ef c1",
        "evex/vpxord_k1z": "62 f1 7d c9 ef c1",
        "evex/vpandq": "62 f1 fd 48 db c1",
        "evex/vpandq_k1": "62 f1 fd 49 db c1",
        "evex/vpandq_k1z": "62 f1 fd c9 db c1",
        "evex/vpandnq": "62 f1 fd 48 df c1",
        "evex/vpandnq_k1": "62 f1 fd 49 df c1",
        "evex/vpandnq_k1z": "62 f1 fd c9 df c1",
        "evex/vporq": "62 f1 fd 48 eb c1",
        "evex/vporq_k1": "62 f1 fd 49 eb c1",
        "evex/vporq_k1z": "62 f1 fd c9 eb c1",
        "evex/vpxorq": "62 f1 fd 48 ef c1",
        "evex/vpxorq_k1": "62 f1 fd 49 ef c1",
        "evex/vpxorq_k1z": "62 f1 fd c9 ef c1",
        "evex/vpandd_m32bcst": "62 f1 7d 58 db 00",
        "evex/vpandd_k1_m32bcst": "62 f1 7d 59 db 00",
        "evex/vpandd_k1z_m32bcst": "62 f1 7d d9 db 00",
        "evex/vpandnd_m32bcst": "62 f1 7d 58 df 00",
        "evex/vpandnd_k1_m32bcst": "62 f1 7d 59 df 00",
        "evex/vpandnd_k1z_m32bcst": "62 f1 7d d9 df 00",
        "evex/vpord_m32bcst": "62 f1 7d 58 eb 00",
        "evex/vpord_k1_m32bcst": "62 f1 7d 59 eb 00",
        "evex/vpord_k1z_m32bcst": "62 f1 7d d9 eb 00",
        "evex/vpxord_m32bcst": "62 f1 7d 58 ef 00",
        "evex/vpxord_k1_m32bcst": "62 f1 7d 59 ef 00",
        "evex/vpxord_k1z_m32bcst": "62 f1 7d d9 ef 00",
        "evex/vpandq_m64bcst": "62 f1 fd 58 db 00",
        "evex/vpandq_k1_m64bcst": "62 f1 fd 59 db 00",
        "evex/vpandq_k1z_m64bcst": "62 f1 fd d9 db 00",
        "evex/vpandnq_m64bcst": "62 f1 fd 58 df 00",
        "evex/vpandnq_k1_m64bcst": "62 f1 fd 59 df 00",
        "evex/vpandnq_k1z_m64bcst": "62 f1 fd d9 df 00",
        "evex/vporq_m64bcst": "62 f1 fd 58 eb 00",
        "evex/vporq_k1_m64bcst": "62 f1 fd 59 eb 00",
        "evex/vporq_k1z_m64bcst": "62 f1 fd d9 eb 00",
        "evex/vpxorq_m64bcst": "62 f1 fd 58 ef 00",
        "evex/vpxorq_k1_m64bcst": "62 f1 fd 59 ef 00",
        "evex/vpxorq_k1z_m64bcst": "62 f1 fd d9 ef 00",
        "evex/vmovdqa32": "62 f1 7d 48 6f c0",
        "evex/vmovdqa32_k1": "62 f1 7d 49 6f c1",
        "evex/vmovdqa32_k1z": "62 f1 7d c9 6f c1",
        "evex/vmovdqa32_k1_m512": "62 f1 7d 49 6f 00",
        "evex/vmovdqa32_k1z_m512": "62 f1 7d c9 6f 00",
        "evex/vmovdqa32_m512_k1": "62 f1 7d 49 7f 08",
        "evex/vmovdqa64": "62 f1 fd 48 6f c0",
        "evex/vmovdqa64_k1": "62 f1 fd 49 6f c1",
        "evex/vmovdqa64_k1z": "62 f1 fd c9 6f c1",
        "evex/vmovdqa64_k1_m512": "62 f1 fd 49 6f 00",
        "evex/vmovdqa64_k1z_m512": "62 f1 fd c9 6f 00",
        "evex/vmovdqa64_m512_k1": "62 f1 fd 49 7f 08",
        "evex/vpxord_zmm16_zmm17_zmm24": "62 81 75 40 ef c0",
        "evex/vpandq_zmm24_k1z_zmm16_zmm17": "62 21 fd c1 db c1",
        "evex/vmovdqa32_zmm16_zmm24": "62 81 7d 48 6f c0",
        "evex/vmovdqa64_zmm24_zmm16": "62 21 fd 48 6f c0",
        "evex/vmovdqa32_zmm16_m512": "62 e1 7d 48 6f 00",
        "evex/vmovdqa32_m512_zmm16": "62 e1 7d 48 7f 00",
        "evex/vcmpeqps_k1_zmm0_zmm1": "62 f1 7c 48 c2 c9 00",
        "evex/vcmpltps_k2_k1_zmm0_zmm1": "62 f1 7c 49 c2 d1 01",
        "evex/vcmplepd_k3_zmm0_zmm1": "62 f1 fd 48 c2 d9 02",
        "evex/vcmpneqps_k4_zmm0_m32bcst": "62 f1 7c 58 c2 20 04",
        "evex/vcmpeqss_k5_xmm0_xmm1": "62 f1 7e 08 c2 e9 00",
        "evex/vcmpeqsd_k6_k1_xmm0_xmm1": "62 f1 ff 09 c2 f1 00",
        "evex/vaddss": "62 f1 7e 08 58 c1",
        "evex/vaddss_k1": "62 f1 7e 09 58 c1",
        "evex/vaddss_k1z": "62 f1 7e 89 58 c1",
        "evex/vsubss": "62 f1 7e 08 5c c1",
        "evex/vsubss_k1": "62 f1 7e 09 5c c1",
        "evex/vsubss_k1z": "62 f1 7e 89 5c c1",
        "evex/vmulss": "62 f1 7e 08 59 c1",
        "evex/vmulss_k1": "62 f1 7e 09 59 c1",
        "evex/vmulss_k1z": "62 f1 7e 89 59 c1",
        "evex/vdivss": "62 f1 7e 08 5e c1",
        "evex/vdivss_k1": "62 f1 7e 09 5e c1",
        "evex/vdivss_k1z": "62 f1 7e 89 5e c1",
        "evex/vaddsd": "62 f1 ff 08 58 c1",
        "evex/vaddsd_k1": "62 f1 ff 09 58 c1",
        "evex/vaddsd_k1z": "62 f1 ff 89 58 c1",
        "evex/vsubsd": "62 f1 ff 08 5c c1",
        "evex/vsubsd_k1": "62 f1 ff 09 5c c1",
        "evex/vsubsd_k1z": "62 f1 ff 89 5c c1",
        "evex/vmulsd": "62 f1 ff 08 59 c1",
        "evex/vmulsd_k1": "62 f1 ff 09 59 c1",
        "evex/vmulsd_k1z": "62 f1 ff 89 59 c1",
        "evex/vdivsd": "62 f1 ff 08 5e c1",
        "evex/vdivsd_k1": "62 f1 ff 09 5e c1",
        "evex/vdivsd_k1z": "62 f1 ff 89 5e c1",
        "evex/vsqrtss": "62 f1 7e 08 51 c1",
        "evex/vsqrtss_k1": "62 f1 7e 09 51 c1",
        "evex/vsqrtss_k1z": "62 f1 7e 89 51 c1",
        "evex/vsqrtsd": "62 f1 ff 08 51 c1",
        "evex/vsqrtsd_k1": "62 f1 ff 09 51 c1",
        "evex/vsqrtsd_k1z": "62 f1 ff 89 51 c1",
        "evex/vminss": "62 f1 7e 08 5d c1",
        "evex/vminss_k1": "62 f1 7e 09 5d c1",
        "evex/vminss_k1z": "62 f1 7e 89 5d c1",
        "evex/vmaxss": "62 f1 7e 08 5f c1",
        "evex/vmaxss_k1": "62 f1 7e 09 5f c1",
        "evex/vmaxss_k1z": "62 f1 7e 89 5f c1",
        "evex/vminsd": "62 f1 ff 08 5d c1",
        "evex/vminsd_k1": "62 f1 ff 09 5d c1",
        "evex/vminsd_k1z": "62 f1 ff 89 5d c1",
        "evex/vmaxsd": "62 f1 ff 08 5f c1",
        "evex/vmaxsd_k1": "62 f1 ff 09 5f c1",
        "evex/vmaxsd_k1z": "62 f1 ff 89 5f c1",
        "evex/vmovss_rr_10": "62 f1 7e 08 10 c1",
        "evex/vmovss_rr_10_k1": "62 f1 7e 09 10 c1",
        "evex/vmovss_rr_10_k1z": "62 f1 7e 89 10 c1",
        "evex/vmovsd_rr_10": "62 f1 ff 08 10 c1",
        "evex/vmovsd_rr_10_k1": "62 f1 ff 09 10 c1",
        "evex/vmovsd_rr_10_k1z": "62 f1 ff 89 10 c1",
        "evex/vmovss_rr_11": "62 f1 7e 08 11 c1",
        "evex/vmovss_rr_11_k1": "62 f1 7e 09 11 c1",
        "evex/vmovss_rr_11_k1z": "62 f1 7e 89 11 c1",
        "evex/vmovsd_rr_11": "62 f1 ff 08 11 c1",
        "evex/vmovsd_rr_11_k1": "62 f1 ff 09 11 c1",
        "evex/vmovsd_rr_11_k1z": "62 f1 ff 89 11 c1",
        "evex/vmovss_m32_load": "62 f1 7e 08 10 00",
        "evex/vmovss_m32_load_k1": "62 f1 7e 09 10 00",
        "evex/vmovss_m32_load_k1z": "62 f1 7e 89 10 00",
        "evex/vmovsd_m64_load": "62 f1 ff 08 10 00",
        "evex/vmovsd_m64_load_k1": "62 f1 ff 09 10 00",
        "evex/vmovsd_m64_load_k1z": "62 f1 ff 89 10 00",
        "evex/vmovss_m32_store": "62 f1 7e 08 11 00",
        "evex/vmovss_m32_store_k1": "62 f1 7e 09 11 00",
        "evex/vmovsd_m64_store": "62 f1 ff 08 11 00",
        "evex/vmovsd_m64_store_k1": "62 f1 ff 09 11 00",
        "evex/vpbroadcastb_m8": "62 f2 7d 48 78 00",
        "evex/vpbroadcastb_m8_k1": "62 f2 7d 49 78 00",
        "evex/vpbroadcastb_m8_k1z": "62 f2 7d c9 78 00",
        "evex/vpbroadcastw_m16": "62 f2 7d 48 79 00",
        "evex/vpbroadcastw_m16_k1": "62 f2 7d 49 79 00",
        "evex/vpbroadcastw_m16_k1z": "62 f2 7d c9 79 00",
        "evex/vpbroadcastd_m32": "62 f2 7d 48 58 00",
        "evex/vpbroadcastd_m32_k1": "62 f2 7d 49 58 00",
        "evex/vpbroadcastd_m32_k1z": "62 f2 7d c9 58 00",
        "evex/vpbroadcastq_m64": "62 f2 fd 48 59 00",
        "evex/vpbroadcastq_m64_k1": "62 f2 fd 49 59 00",
        "evex/vpbroadcastq_m64_k1z": "62 f2 fd c9 59 00",
        "evex/vbroadcastss_m32": "62 f2 7d 48 18 00",
        "evex/vbroadcastss_m32_k1": "62 f2 7d 49 18 00",
        "evex/vbroadcastss_m32_k1z": "62 f2 7d c9 18 00",
        "evex/vbroadcastss_xmm": "62 f2 7d 48 18 c1",
        "evex/vbroadcastss_xmm_k1": "62 f2 7d 49 18 c1",
        "evex/vbroadcastss_xmm_k1z": "62 f2 7d c9 18 c1",
        "evex/vbroadcastsd_m64": "62 f2 fd 48 19 00",
        "evex/vbroadcastsd_m64_k1": "62 f2 fd 49 19 00",
        "evex/vbroadcastsd_m64_k1z": "62 f2 fd c9 19 00",
        "evex/vbroadcastsd_xmm": "62 f2 fd 48 19 c1",
        "evex/vbroadcastsd_xmm_k1": "62 f2 fd 49 19 c1",
        "evex/vbroadcastsd_xmm_k1z": "62 f2 fd c9 19 c1",
        "evex/vbroadcastf32x4_m128": "62 f2 7d 48 1a 00",
        "evex/vbroadcastf32x4_m128_k1": "62 f2 7d 49 1a 00",
        "evex/vbroadcastf32x4_m128_k1z": "62 f2 7d c9 1a 00",
        "evex/vbroadcastf64x4_m256": "62 f2 fd 48 1b 00",
        "evex/vbroadcastf64x4_m256_k1": "62 f2 fd 49 1b 00",
        "evex/vbroadcastf64x4_m256_k1z": "62 f2 fd c9 1b 00",
        "evex/vpbroadcastb_xmm_reg": "62 f2 7d 08 78 c1",
        "evex/vpbroadcastb_ymm_reg_k1": "62 f2 7d 29 78 c1",
        "evex/vpbroadcastb_zmm_reg_k1z": "62 f2 7d c9 78 c1",
        "evex/vpbroadcastw_xmm_reg": "62 f2 7d 08 79 c1",
        "evex/vpbroadcastw_ymm_reg_k1": "62 f2 7d 29 79 c1",
        "evex/vpbroadcastw_zmm_reg_k1z": "62 f2 7d c9 79 c1",
        "evex/vpbroadcastd_xmm_reg": "62 f2 7d 08 58 c1",
        "evex/vpbroadcastd_ymm_reg_k1": "62 f2 7d 29 58 c1",
        "evex/vpbroadcastd_zmm_reg_k1z": "62 f2 7d c9 58 c1",
        "evex/vpbroadcastq_xmm_reg": "62 f2 fd 08 59 c1",
        "evex/vpbroadcastq_ymm_reg_k1": "62 f2 fd 29 59 c1",
        "evex/vpbroadcastq_zmm_reg_k1z": "62 f2 fd c9 59 c1",
        "evex/vbroadcasti32x2_xmm_reg": "62 f2 7d 08 59 c1",
        "evex/vbroadcasti32x2_ymm_reg_k1": "62 f2 7d 29 59 c1",
        "evex/vbroadcasti32x2_zmm_reg_k1z": "62 f2 7d c9 59 c1",
        "evex/vbroadcastf32x2_ymm_reg": "62 f2 7d 28 19 c1",
        "evex/vbroadcastf32x2_zmm_reg_k1": "62 f2 7d 49 19 c1",
        "evex/vbroadcastf32x2_zmm_reg_k1z": "62 f2 7d c9 19 c1",
        "evex/vbroadcastf64x2_ymm_m128": "62 f2 fd 28 1a 00",
        "evex/vbroadcastf64x2_zmm_m128_k1": "62 f2 fd 49 1a 00",
        "evex/vbroadcastf64x2_zmm_m128_k1z": "62 f2 fd c9 1a 00",
        "evex/vbroadcastf32x8_zmm_m256": "62 f2 7d 48 1b 00",
        "evex/vbroadcastf32x8_zmm_m256_k1": "62 f2 7d 49 1b 00",
        "evex/vbroadcastf32x8_zmm_m256_k1z": "62 f2 7d c9 1b 00",
        "evex/vpshufd_zmm": "62 f1 7d 48 70 c0 02",
        "evex/vpshufd_zmm_k1": "62 f1 7d 49 70 c0 02",
        "evex/vpshufd_zmm_k1z": "62 f1 7d c9 70 c0 02",
        "evex/vpshufhw_zmm": "62 f1 7e 48 70 c0 02",
        "evex/vpshufhw_zmm_k1": "62 f1 7e 49 70 c0 02",
        "evex/vpshufhw_zmm_k1z": "62 f1 7e c9 70 c0 02",
        "evex/vpshuflw_zmm": "62 f1 7f 48 70 c0 02",
        "evex/vpshuflw_zmm_k1": "62 f1 7f 49 70 c0 02",
        "evex/vpshuflw_zmm_k1z": "62 f1 7f c9 70 c0 02",
        "evex/vpunpcklbw_xmm": "62 f1 7d 08 60 c1",
        "evex/vpunpcklwd_ymm_k1": "62 f1 7d 29 61 c1",
        "evex/vpunpckldq_zmm_k1z": "62 f1 7d c9 62 c1",
        "evex/vpunpcklqdq_zmm_k1z": "62 f1 fd c9 6c c1",
        "evex/vpunpckhbw_xmm": "62 f1 7d 08 68 c1",
        "evex/vpunpckhwd_ymm_k1": "62 f1 7d 29 69 c1",
        "evex/vpunpckhdq_zmm_k1z": "62 f1 7d c9 6a c1",
        "evex/vpunpckhqdq_zmm_k1z": "62 f1 fd c9 6d c1",
        "evex/vpacksswb_xmm": "62 f1 7d 08 63 c1",
        "evex/vpackssdw_ymm_k1": "62 f1 7d 29 6b c1",
        "evex/vpackuswb_zmm_k1z": "62 f1 7d c9 67 c1",
        "evex/vpaddb_xmm": "62 f1 7d 08 fc c1",
        "evex/vpaddw_ymm_k1": "62 f1 7d 29 fd c1",
        "evex/vpaddd_zmm_k1z": "62 f1 7d c9 fe c1",
        "evex/vpaddq_zmm_k1z": "62 f1 fd c9 d4 c1",
        "evex/vpsubb_xmm": "62 f1 7d 08 f8 c1",
        "evex/vpsubw_ymm_k1": "62 f1 7d 29 f9 c1",
        "evex/vpsubd_zmm_k1z": "62 f1 7d c9 fa c1",
        "evex/vpsubq_zmm_k1z": "62 f1 fd c9 fb c1",
        "evex/vpmullw_xmm": "62 f1 7d 08 d5 c1",
        "evex/vpmulhw_ymm_k1": "62 f1 7d 29 e5 c1",
        "evex/vpmulhuw_zmm_k1z": "62 f1 7d c9 e4 c1",
        "evex/vpmuludq_zmm_k1z": "62 f1 fd c9 f4 c1",
        "evex/vpmaddwd_zmm_k1z": "62 f1 7d c9 f5 c1",
        "evex/vpsadbw_zmm": "62 f1 7d 48 f6 c1",
        "evex/vpsubusb_xmm": "62 f1 7d 08 d8 c1",
        "evex/vpsubusw_ymm_k1": "62 f1 7d 29 d9 c1",
        "evex/vpsubsb_zmm_k1z": "62 f1 7d c9 e8 c1",
        "evex/vpsubsw_zmm_k1z": "62 f1 7d c9 e9 c1",
        "evex/vpaddusb_xmm": "62 f1 7d 08 dc c1",
        "evex/vpaddusw_ymm_k1": "62 f1 7d 29 dd c1",
        "evex/vpaddsb_zmm_k1z": "62 f1 7d c9 ec c1",
        "evex/vpaddsw_zmm_k1z": "62 f1 7d c9 ed c1",
        "evex/vpavgb_xmm": "62 f1 7d 08 e0 c1",
        "evex/vpavgw_zmm_k1z": "62 f1 7d c9 e3 c1",
        "evex/vpminub_ymm_k1": "62 f1 7d 29 da c1",
        "evex/vpminsw_zmm_k1z": "62 f1 7d c9 ea c1",
        "evex/vpmaxub_ymm_k1": "62 f1 7d 29 de c1",
        "evex/vpmaxsw_zmm_k1z": "62 f1 7d c9 ee c1",
        "evex/vpshufb_xmm": "62 f2 7d 08 00 c1",
        "evex/vpshufb_zmm_k1z": "62 f2 7d c9 00 c1",
        "evex/vpmaddubsw_xmm": "62 f2 7d 08 04 c1",
        "evex/vpmaddubsw_zmm_k1z": "62 f2 7d c9 04 c1",
        "evex/vpmulhrsw_zmm_k1z": "62 f2 7d c9 0b c1",
        "evex/vpabsb_xmm": "62 f2 7d 08 1c c1",
        "evex/vpabsw_ymm_k1": "62 f2 7d 29 1d c1",
        "evex/vpabsd_zmm_k1z": "62 f2 7d c9 1e c1",
        "evex/vmovdqu8_zmm_k1": "62 f1 7f 49 6f c1",
        "evex/vmovdqu8_zmm_k1z": "62 f1 7f c9 6f c1",
        "evex/vmovdqu8_zmm_m512": "62 f1 7f 49 6f 00",
        "evex/vmovdqu8_m512_zmm": "62 f1 7f 49 7f 08",
        "evex/vmovdqu16_zmm_k1": "62 f1 ff 49 6f c1",
        "evex/vmovdqu16_zmm_k1z": "62 f1 ff c9 6f c1",
        "evex/vmovdqu16_zmm_m512": "62 f1 ff 49 6f 00",
        "evex/vmovdqu16_m512_zmm": "62 f1 ff 49 7f 08",
        "evex/vmovdqu32_zmm_k1": "62 f1 7e 49 6f c1",
        "evex/vmovdqu32_zmm_k1z": "62 f1 7e c9 6f c1",
        "evex/vmovdqu32_zmm_m512": "62 f1 7e 49 6f 00",
        "evex/vmovdqu32_m512_zmm": "62 f1 7e 49 7f 08",
        "evex/vmovdqu64_zmm_k1": "62 f1 fe 49 6f c1",
        "evex/vmovdqu64_zmm_k1z": "62 f1 fe c9 6f c1",
        "evex/vmovdqu64_zmm_m512": "62 f1 fe 49 6f 00",
        "evex/vmovdqu64_m512_zmm": "62 f1 fe 49 7f 08",
        "evex/vaesenc": "62 f2 75 48 dc c2",
        "evex/vaesenc_k1": "62 f2 75 49 dc c2",
        "evex/vaesenc_k1z": "62 f2 75 c9 dc c2",
        "evex/vgf2p8mulb": "62 f2 75 48 cf c2",
        "evex/vgf2p8mulb_k1": "62 f2 75 49 cf c2",
        "evex/vgf2p8mulb_k1z": "62 f2 75 c9 cf c2",
    }
    for name, hex_bytes in samples.items():
        cases.append(Case(name, name.split("/")[0].upper(), bytes.fromhex(hex_bytes) + TAIL))

    rng = random.Random(0x4862)
    for i in range(random_count):
        n = rng.randint(1, 15)
        cases.append(Case(f"random/{i:04d}", "random valid capstone", bytes(rng.randrange(256) for _ in range(n))))
    return cases


COND_JUMPS = {
    "ja", "jae", "jb", "jbe", "jc", "je", "jg", "jge", "jl", "jle", "jna",
    "jnae", "jnb", "jnbe", "jnc", "jne", "jng", "jnge", "jnl", "jnle", "jno",
    "jnp", "jns", "jnz", "jo", "jp", "jpe", "jpo", "js", "jz",
}


def normalize_capstone(mnemonic: str) -> str:
    m = mnemonic.lower()
    if m in COND_JUMPS:
        return "jcc"
    if m.startswith("cmov") and m != "cmov":
        return "cmovcc"
    if m.startswith("set") and m != "set":
        return "setcc"
    if m.startswith("ret"):
        if m.startswith("retf"):
            return "retf"
        return "ret"
    if m.startswith("iret"):
        return "iret"
    if m in {"pushfq", "pushfd", "pushfw"}:
        return "pushf"
    if m in {"popfq", "popfd", "popfw"}:
        return "popf"
    if m == "fnop":
        return "nop"
    if m in {"feni8087_nop", "fdisi8087_nop", "fsetpm"}:
        return "nop"
    if m in {"sldt", "str", "lldt", "ltr", "verr", "verw", "sgdt", "sidt", "lgdt",
             "lidt", "smsw", "lmsw", "invlpg", "enclv", "lar", "lsl", "syscall",
             "clts", "sysret", "invd", "wbinvd", "wrmsr", "rdtsc", "rdmsr", "rdpmc",
             "sysenter", "sysexit", "getsec", "montmul", "xstore", "rsm",
             "vmread", "vmwrite", "rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase",
             "popcnt"}:
        return "sys"
    if m in {"ud0", "ud1", "ud2"}:
        return "ud"
    if m in {"femms", "emms", "cvtpi2ps", "cvttps2pi", "cvtps2pi", "punpcklbw",
             "punpcklwd", "punpckldq", "packsswb", "pcmpgtb", "pcmpgtw", "pcmpgtd",
             "packuswb", "punpckhbw", "punpckhwd", "punpckhdq", "packssdw",
             "movd", "pshufw", "pcmpeqb", "pcmpeqw", "pcmpeqd", "cmpps", "pinsrw",
             "pextrw", "psrlw", "psrld", "psrlq", "paddq", "pmullw", "pmovmskb",
             "psubusb", "psubusw", "pminub", "pand", "paddusb", "paddusw",
             "pmaxub", "pandn", "pavgb", "psraw", "psrad", "pavgw", "pmulhuw",
             "pmulhw", "psubsb", "psubsw", "pminsw", "por", "paddsb", "paddsw",
             "pmaxsw", "pxor", "psllw", "pslld", "psllq", "pmuludq", "pmaddwd",
              "psadbw", "maskmovq", "psubb", "psubw", "psubd", "psubq", "paddb",
             "paddw", "paddd"}:
        return "mmx"
    implemented_vec = {
        "phaddw", "phaddd", "phaddsw", "phsubw", "phsubd", "phsubsw",
        "pmaddubsw", "pshufb", "psignb", "psignw", "psignd", "pmulhrsw",
        "pabsb", "pabsw", "pabsd", "ptest", "pmovsxbw", "pmovsxbd",
        "pmovsxbq", "pmovsxwd", "pmovsxwq", "pmovsxdq", "pmuldq",
        "pcmpeqq", "packusdw", "pmovzxbw", "pmovzxbd", "pmovzxbq",
        "pmovzxwd", "pmovzxwq", "pmovzxdq", "pcmpgtq", "pminsb",
        "pminsd", "pminuw", "pminud", "pmaxsb", "pmaxsd", "pmaxuw",
        "pmaxud", "pmulld", "phminposuw", "palignr", "pblendw",
        "blendps", "blendpd", "pblendvb", "blendvps", "blendvpd",
        "psubusb", "psubusw", "psubsb", "psubsw", "pminub", "pminsw",
        "pmaxub", "pmaxsw", "pmuludq", "psadbw",
        "aesimc", "aesenc", "aesenclast", "aesdec", "aesdeclast",
        "gf2p8mulb",
        "sha1nexte", "sha1msg1", "sha1msg2", "sha256rnds2",
        "sha256msg1", "sha256msg2", "sha1rnds4",
        "mpsadbw",
        "pcmpestrm", "pcmpestri", "pcmpistrm", "pcmpistri",
    }
    if m in implemented_vec:
        return m
    if m == "vzeroupper":
        return "vzeroupper"
    if m.startswith("vcmp"):
        if m.endswith("ps"):
            return "cmpps"
        if m.endswith("pd"):
            return "cmppd"
        if m.endswith("ss"):
            return "cmpss"
        if m.endswith("sd"):
            return "cmpsd"
    if m in {"vucomiss", "vcomiss"}:
        return "comiss"
    if m in {"vucomisd", "vcomisd"}:
        return "comisd"
    if m in {"vandps", "vandpd"}:
        return "xmm_and"
    if m in {"vandnps", "vandnpd"}:
        return "xmm_andn"
    if m in {"vorps", "vorpd"}:
        return "xmm_or"
    if m == "vxorpd":
        return "xorps"
    if m == "vpand":
        return "xmm_and"
    if m == "vpandn":
        return "xmm_andn"
    if m == "vpor":
        return "xmm_or"
    if m in {"vmovhlps", "vmovlhps"}:
        return m[1:]
    if m.startswith(("vfmadd", "vfmsub", "vfnmadd", "vfnmsub")):
        return m[1:]
    implemented_vex = {
        "addps", "addpd", "subps", "subpd", "mulps", "mulpd", "divps", "divpd",
        "addss", "addsd", "subss", "subsd", "mulss", "mulsd", "divss", "divsd",
        "minps", "minpd", "minss", "minsd", "maxps", "maxpd", "maxss", "maxsd",
        "sqrtps", "sqrtpd", "sqrtss", "sqrtsd", "rsqrtps", "rsqrtss", "rcpps", "rcpss",
        "roundps", "roundpd", "roundss", "roundsd", "dpps", "dppd", "insertps", "extractps",
        "pinsrb", "pinsrw", "pinsrd", "pinsrq", "pextrb", "pextrw", "pextrd", "pextrq",
        "gf2p8affineqb", "gf2p8affineinvqb",
        "unpcklps", "unpckhps", "unpcklpd", "unpckhpd", "shufps", "shufpd",
        "pshufd", "pshufhw", "pshuflw",
        "movsldup", "movshdup", "movddup",
        "cvtps2pd", "cvtpd2ps", "cvtdq2ps", "cvtps2dq", "cvttps2dq",
        "cvtdq2pd", "cvtpd2dq", "cvttpd2dq",
        "cvtss2sd", "cvtsd2ss", "cvtsi2ss", "cvtsi2sd",
        "cvtss2si", "cvtsd2si", "cvttss2si", "cvttsd2si",
        "haddps", "haddpd", "hsubps", "hsubpd", "addsubps", "addsubpd",
        "andps", "andnps", "orps", "xorps", "pxor",
        "paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq",
        "psrlw", "psrld", "psrlq", "psrad", "psllw", "pslld", "psllq",
        "paddusb", "paddusw", "paddsb", "paddsw", "pavgb", "pavgw",
        "pmullw", "pmulhw", "pmulhuw", "pmaddwd", "pmaddubsw", "pmuldq", "pmulld",
        "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpeqq", "pcmpgtb", "pcmpgtw", "pcmpgtd", "pcmpgtq",
        "punpcklbw", "punpckhwd", "punpckldq", "packsswb", "packuswb", "packssdw",
        "psubusb", "psubusw", "psubsb", "psubsw",
        "pminub", "pminsw", "pminsb", "pminsd", "pminuw", "pminud",
        "pmaxub", "pmaxsw", "pmaxsb", "pmaxsd", "pmaxuw", "pmaxud",
        "pmuludq", "psadbw", "pbroadcastb", "pbroadcastw", "pbroadcastd",
        "pbroadcastq", "broadcasti128", "pblendd", "permq", "permpd",
        "permilps", "permilpd", "blendvps", "blendvpd", "pblendvb",
        "insertf128", "inserti128", "extractf128", "extracti128",
        "perm2f128", "perm2i128",
        "psrlvd", "psrlvq", "psravd", "psllvd", "psllvq",
        "pclmulqdq", "aeskeygenassist",
        "aesenc", "aesenclast", "aesdec", "aesdeclast",
        "gf2p8mulb",
        "mpsadbw",
        "permps", "permd",
        "maskmovps", "maskmovpd", "pmaskmovd", "pmaskmovq",
        "gatherdps", "gatherdpd", "gatherqps", "gatherqpd",
        "pgatherdd", "pgatherdq", "pgatherqd", "pgatherqq",
    }
    if m.startswith("v") and m[1:] in implemented_vex:
        return m[1:]
    if m == "vmovq":
        return "movd"
    if m in {"vmovaps", "vmovups", "vmovapd", "vmovupd", "vmovss", "vmovsd", "vmovdqa", "vmovdqu",
             "vmovntps", "vmovntpd", "vmovntdq", "vlddqu", "vmovntdqa"}:
        return "sse_mov"
    if m in {"vmovdqa32", "vmovdqa64", "vmovdqu8", "vmovdqu16", "vmovdqu32", "vmovdqu64"}:
        return "sse_mov"
    if m in {"roundps", "roundpd", "roundss", "roundsd"}:
        return m
    if m in {"dpps", "dppd"}:
        return m
    if m in {"insertps", "extractps"}:
        return m
    if m in {"pinsrb", "pinsrw", "pinsrd", "pinsrq", "pextrb", "pextrw", "pextrd", "pextrq"}:
        return m
    if m in {"gf2p8affineqb", "gf2p8affineinvqb"}:
        return m
    if m in {"vbroadcastss", "vbroadcastsd", "vbroadcastf32x2", "vbroadcastf64x2",
             "vbroadcastf32x4", "vbroadcastf64x4", "vbroadcastf32x8", "vbroadcasti32x2"}:
        return m[1:]
    if m == "vbroadcastf128":
        return "broadcastf32x4"
    if m in {"vpsrlw", "vpsrld", "vpsrlq", "vpsraw", "vpsrad",
             "vpsllw", "vpslld", "vpsllq", "vpsrldq", "vpslldq"}:
        return m[1:]
    if m in {"vmovlps", "vmovhps", "vmovlpd", "vmovhpd"}:
        return m[1:]
    if m in {"vpshufd", "vpshufhw", "vpshuflw"}:
        return m[1:]
    if (m.startswith("sha") or m.startswith("phadd") or m.startswith("phsub") or
            m.startswith("round") or m.startswith("blend") or
            (m.startswith("v") and m not in {"vmread", "vmwrite"}) or
            m in {"palignr", "pmaddubsw", "pshufb", "psignb", "psignw", "psignd",
                  "pmulhrsw", "pblendvb", "ptest", "pmovsxbw", "pmovsxbd", "pmovsxbq",
                  "pmovsxwd", "pmovsxwq", "pmovsxdq", "pmuldq", "pcmpeqq", "movntdqa",
                  "packusdw", "pmovzxbw", "pmovzxbd", "pmovzxbq", "pmovzxwd",
                  "pmovzxwq", "pmovzxdq", "pcmpgtq", "pminsb", "pminsd", "pminuw",
                  "pminud", "pmaxsb", "pmaxsd", "pmaxuw", "pmaxud", "pmulld",
                  "phminposuw", "aesimc", "aesenc", "aesenclast", "aesdec", "aesdeclast",
                  "pclmulqdq", "pcmpestrm", "pcmpestri", "pcmpistrm", "pcmpistri",
                  "insertps", "dpps", "dppd", "mpsadbw", "cvtpi2pd", "cvttpd2pi",
                  "cvtpd2pi", "cvttpd2dq", "cvtpd2dq", "movsldup", "movshdup",
                  "movddup", "cvtss2si", "cvtsd2si", "cmppd", "cmpss", "cmpsd",
                  "extrq", "insertq", "haddpd", "hsubpd", "haddps", "hsubps",
                  "addsubpd", "addsubps", "maskmovdqu", "movq2dq", "movdq2q"}):
        return "vec"
    if m == "sal":
        return "shl"
    if m.startswith("fcmov"):
        return "fcmov"
    if m == "fstpnce":
        return "fstp"
    if m == "fucompp":
        return "fcompp"
    if m in {"fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr"}:
        return "fi"
    if m in {"fchs", "fabs", "ftst", "fxam", "fld1", "fldl2t", "fldl2e", "fldpi",
             "fldlg2", "fldln2", "fldz", "f2xm1", "fyl2x", "fptan", "fpatan",
             "fxtract", "fprem1", "fdecstp", "fincstp", "fprem", "fyl2xp1",
             "fsqrt", "fsincos", "fscale", "fsin", "fcos", "fldenv", "fnstenv",
             "frstor", "fnsave", "fbld", "fbstp"}:
        return "misc"
    if m in {"insb", "insw", "insd"}:
        return "ins"
    if m in {"outsb", "outsw", "outsd"}:
        return "outs"
    if m == "xlatb":
        return "xlat"
    if m == "movabs":
        return "mov"
    if m.startswith("loop"):
        return "loop"
    if m.startswith("movs") and m != "movsx" and m != "movsxd":
        return "movs"
    if m.startswith("cmps"):
        return "cmps"
    if m.startswith("lods"):
        return "lods"
    if m.startswith("scas"):
        return "scas"
    if m.startswith("stos"):
        return "stos"
    if m in {"movaps", "movups", "movss", "movsd", "movapd", "movupd", "movdqa", "movdqu"}:
        return "sse_mov"
    if m in {"andps", "andpd", "pand"}:
        return "xmm_and"
    if m in {"andnps", "andnpd", "pandn"}:
        return "xmm_andn"
    if m in {"orps", "orpd", "por"}:
        return "xmm_or"
    return m


def normalize_hb(op: str) -> str:
    low = op.lower()
    if low.startswith("vpbroadcast"):
        return low[1:]
    if low == "vbroadcasti128":
        return "broadcasti128"
    if low in {"vbroadcastss", "vbroadcastsd", "vbroadcastf32x2", "vbroadcastf64x2",
               "vbroadcastf32x4", "vbroadcastf64x4", "vbroadcastf32x8", "vbroadcasti32x2"}:
        return low[1:]
    if low in {"vmovlps", "vmovhps", "vmovlpd", "vmovhpd"}:
        return low[1:]
    if low in {"vmovhlps", "vmovlhps"}:
        return low[1:]
    if low.startswith(("vfmadd", "vfmsub", "vfnmadd", "vfnmsub")):
        return low[1:]
    if low in {"vpblendd", "vpermq", "vpermpd", "vinsertf128", "vinserti128",
               "vpermilps", "vpermilpd", "vblendvps", "vblendvpd", "vpblendvb",
               "vextractf128", "vextracti128", "vperm2f128", "vperm2i128",
               "vhaddps", "vhaddpd", "vhsubps", "vhsubpd", "vaddsubps", "vaddsubpd",
               "vmovsldup", "vmovshdup", "vmovddup",
               "vpsrlvd", "vpsrlvq", "vpsravd", "vpsllvd", "vpsllvq",
               "vpclmulqdq", "vaeskeygenassist", "vpermps", "vpermd",
               "vaesimc", "vaesenc", "vaesenclast", "vaesdec", "vaesdeclast",
               "vgf2p8mulb", "vgf2p8affineqb", "vgf2p8affineinvqb",
               "vmpsadbw",
               "vmaskmovps", "vmaskmovpd", "vpmaskmovd", "vpmaskmovq",
               "vgatherdps", "vgatherdpd", "vgatherqps", "vgatherqpd",
               "vpgatherdd", "vpgatherdq", "vpgatherqd", "vpgatherqq"}:
        return low[1:]
    if low in {"vcmpps", "vcmppd", "vcmpss", "vcmpsd"}:
        return low[1:]
    if low in {"mov_seg", "mov_cr", "mov_dr"}:
        return "mov"
    if low == "push_seg":
        return "push"
    if low == "pop_seg":
        return "pop"
    if low.startswith("x87_"):
        return low[4:]
    return low


def capstone_first(md: Cs, code: bytes):
    insns = list(md.disasm(code, 0x100000000, count=1))
    return insns[0] if insns else None


def probe(cases: list[Case]) -> list[dict]:
    payload = "\n".join(c.code.hex() for c in cases) + "\n"
    p = subprocess.run([str(PROBE_BIN)], input=payload, text=True, cwd=HB_ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return [json.loads(line) for line in p.stdout.splitlines()]


def summarize(cases: list[Case], rows: list[dict]) -> dict:
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    summary = collections.defaultdict(lambda: {
        "cases": 0,
        "capstone_valid": 0,
        "decoded": 0,
        "length_match": 0,
        "mnemonic_match": 0,
        "lifted": 0,
        "missing": collections.Counter(),
        "mismatch": collections.Counter(),
        "examples": [],
    })
    all_missing = collections.Counter()
    for case, row in zip(cases, rows):
        insn = capstone_first(md, case.code)
        group = summary[case.group]
        group["cases"] += 1
        if insn is None:
            continue
        group["capstone_valid"] += 1
        cap_norm = normalize_capstone(insn.mnemonic)
        if row["decode"] == 0:
            group["decoded"] += 1
            if row["len"] == insn.size:
                group["length_match"] += 1
            hb_norm = normalize_hb(row["op"])
            legacy_66_vec = case.name.startswith("0f/66/") and hb_norm == insn.mnemonic.lower()
            if hb_norm == cap_norm or legacy_66_vec:
                group["mnemonic_match"] += 1
            else:
                group["mismatch"][f"{insn.mnemonic}->{row['op']}"] += 1
                if len(group["examples"]) < 6:
                    group["examples"].append({
                        "case": case.name,
                        "hex": case.code[:insn.size].hex(),
                        "capstone": f"{insn.mnemonic} {insn.op_str}".strip(),
                        "hb": row["op"],
                        "hb_len": row["len"],
                        "cs_len": insn.size,
                        "lift": row["lift"],
                    })
            if row["lift"] == 0:
                group["lifted"] += 1
        else:
            group["missing"][insn.mnemonic] += 1
            all_missing[insn.mnemonic] += 1
            if len(group["examples"]) < 6:
                group["examples"].append({
                    "case": case.name,
                    "hex": case.code[:insn.size].hex(),
                    "capstone": f"{insn.mnemonic} {insn.op_str}".strip(),
                    "hb": f"decode {row['decode']}",
                    "cs_len": insn.size,
                })
    result = {"groups": {}, "top_missing": all_missing.most_common(30)}
    for name, value in sorted(summary.items()):
        result["groups"][name] = {
            "cases": value["cases"],
            "capstone_valid": value["capstone_valid"],
            "decoded": value["decoded"],
            "length_match": value["length_match"],
            "mnemonic_match": value["mnemonic_match"],
            "lifted": value["lifted"],
            "top_missing": value["missing"].most_common(12),
            "top_mismatch": value["mismatch"].most_common(12),
            "examples": value["examples"],
        }
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--random", type=int, default=4000, help="random byte streams to include")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of compact text")
    args = ap.parse_args()

    build_probe()
    cases = generate_cases(args.random)
    rows = probe(cases)
    result = summarize(cases, rows)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for group, data in result["groups"].items():
            cv = data["capstone_valid"]
            print(f"{group}: capstone={cv} decoded={data['decoded']} "
                  f"len={data['length_match']} mnemonic={data['mnemonic_match']} "
                  f"lifted={data['lifted']}")
        print("top_missing:", ", ".join(f"{k}:{v}" for k, v in result["top_missing"][:12]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
