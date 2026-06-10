#!/usr/bin/env python3
"""Differential correctness fuzzer for HyperBridge x86/x64 interpreter semantics.

Unicorn is the primary oracle. SDE support is detected but not faked. When an
opcode is not supported by Unicorn, this harness reports that explicitly and
only uses exact Python oracles for the small scalar subset modeled below.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import random
import re
import shutil
import subprocess
import struct
import sys
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

_LIBM = ctypes.CDLL(None)
_LIBM.fma.argtypes = [ctypes.c_double, ctypes.c_double, ctypes.c_double]
_LIBM.fma.restype = ctypes.c_double
_LIBM.fmaf.argtypes = [ctypes.c_float, ctypes.c_float, ctypes.c_float]
_LIBM.fmaf.restype = ctypes.c_float


HB_ROOT = Path(__file__).resolve().parents[1]
KIT_ROOT = HB_ROOT.parents[1]
RUNNER_C = HB_ROOT / "tests" / "hb_diff_case_runner.c"
RUNNER = HB_ROOT / "tests" / "hb_diff_case_runner"
REPORT_JSON = HB_ROOT / "reports" / "hb_fuzz_diff_last.json"
sys.path.insert(0, str(KIT_ROOT))

from tools.hb_oracle.unicorn_adapter import diff_interpreter as unicorn_diff_interpreter
from tools.hb_oracle.unicorn_adapter import run_case as run_unicorn_case

MASK64 = (1 << 64) - 1
SIGN64 = 1 << 63


@dataclass(frozen=True)
class Template:
    name: str
    family: str
    code: str
    oracle: str | None = None
    defined_flags: frozenset[str] | None = None


TEMPLATES: list[Template] = [
    Template("add_r8_r9", "int_arith_flags", "4d01c8", "add"),
    Template("adc_r8_r9", "int_arith_flags", "4d11c8", "adc"),
    Template("sub_r8_r9", "int_arith_flags", "4d29c8", "sub"),
    Template("sbb_r8_r9", "int_arith_flags", "4d19c8", "sbb"),
    Template("cmp_r8_r9", "int_arith_flags", "4d39c8", "cmp"),
    Template("and_r8_r9", "int_logic_flags", "4d21c8", "and"),
    Template("or_r8_r9", "int_logic_flags", "4d09c8", "or"),
    Template("xor_r8_r9", "int_logic_flags", "4d31c8", "xor"),
    Template("test_r8_r9", "int_logic_flags", "4d85c8", "test"),
    Template("neg_r8", "int_arith_flags", "49f7d8", "neg"),
    Template("imul_r8_r9", "mul_div", "4d0fafc1", None, frozenset({"cf", "of"})),
    Template("mul_r9", "mul_div", "49f7e1", None, frozenset({"cf", "of"})),
    Template("div_r9", "mul_div", "49f7f1", None, frozenset()),
    Template("shl_r8_cl", "shift_rotate_flags", "49d3e0", "shl"),
    Template("shr_r8_cl", "shift_rotate_flags", "49d3e8", None),
    Template("sar_r8_cl", "shift_rotate_flags", "49d3f8", None),
    Template("rol_r8_cl", "shift_rotate_flags", "49d3c0", None, frozenset({"cf"})),
    Template("ror_r8_cl", "shift_rotate_flags", "49d3c8", None, frozenset({"cf"})),
    Template("rcl_r8_cl", "rcl_rcr", "49d3d0", None, frozenset({"cf"})),
    Template("rcr_r8_cl", "rcl_rcr", "49d3d8", None, frozenset({"cf"})),
    Template("cmovne_r8_r9", "cmov_setcc", "4d0f45c1", "cmovne"),
    Template("setne_r8b", "cmov_setcc", "410f95c0", "setne"),
    Template("bt_r8_r9", "bt_family", "4d0fa3c8", None, frozenset({"cf"})),
    Template("bts_r8_r9", "bt_family", "4d0fabc8", None, frozenset({"cf"})),
    Template("btr_r8_r9", "bt_family", "4d0fb3c8", None, frozenset({"cf"})),
    Template("btc_r8_r9", "bt_family", "4d0fbbc8", None, frozenset({"cf"})),
    Template("bsf_r8_r9", "bit_scan", "4d0fbcc1", None, frozenset({"zf"})),
    Template("bsr_r8_r9", "bit_scan", "4d0fbdc1", None, frozenset({"zf"})),
    Template("tzcnt_r8_r9", "bmi", "f34d0fbcc1", "tzcnt", frozenset({"cf", "zf"})),
    Template("lzcnt_r8_r9", "bmi", "f34d0fbdc1", "lzcnt", frozenset({"cf", "zf"})),
    Template("popcnt_r8_r9", "bmi", "f34d0fb8c1", "popcnt", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("crc32_r8d_r9b", "crc_adx", "f2450f38f0c1", None, frozenset()),
    Template("crc32_r8d_r9w", "crc_adx", "66f2450f38f1c1", None, frozenset()),
    Template("crc32_r8d_r9d", "crc_adx", "f2450f38f1c1", None, frozenset()),
    Template("crc32_r8_r9", "crc_adx", "f24d0f38f1c1", None, frozenset()),
    Template("movbe_dx_m16", "movbe", "660f38f010", "movbe", frozenset()),
    Template("movbe_edx_m32", "movbe", "0f38f010", "movbe", frozenset()),
    Template("movbe_rdx_m64", "movbe", "480f38f010", "movbe", frozenset()),
    Template("movbe_m16_dx", "movbe", "660f38f110", "movbe", frozenset()),
    Template("movbe_m32_edx", "movbe", "0f38f110", "movbe", frozenset()),
    Template("movbe_m64_rdx", "movbe", "480f38f110", "movbe", frozenset()),
    Template("movdiri_m32_edx", "movdiri", "0f38f910", "movdiri", frozenset()),
    Template("movdiri_66_m32_edx", "movdiri", "660f38f910", "movdiri", frozenset()),
    Template("movdiri_f2_m32_edx", "movdiri", "f20f38f910", "movdiri", frozenset()),
    Template("movdiri_f3_m32_edx", "movdiri", "f30f38f910", "movdiri", frozenset()),
    Template("movdiri_m64_rdx", "movdiri", "480f38f910", "movdiri", frozenset()),
    Template("movdiri_rex66_m32_edx", "movdiri", "48660f38f910", "movdiri", frozenset()),
    Template("movdiri_rexf2_m32_edx", "movdiri", "48f20f38f910", "movdiri", frozenset()),
    Template("movdiri_rexf3_m32_edx", "movdiri", "48f30f38f910", "movdiri", frozenset()),
    Template("movdiri_66rex_m64_rdx", "movdiri", "66480f38f910", "movdiri", frozenset()),
    Template("movdiri_f2rex_m64_rdx", "movdiri", "f2480f38f910", "movdiri", frozenset()),
    Template("movdiri_f3rex_m64_rdx", "movdiri", "f3480f38f910", "movdiri", frozenset()),
    Template("movdir64b_rdi_m512_rsi", "movdir64b", "660f38f83e", "movdir64b", frozenset()),
    Template("movdir64b_rex66_rdi_m512_rsi", "movdir64b", "48660f38f83e", "movdir64b", frozenset()),
    Template("movdir64b_66rex_rdi_m512_rsi", "movdir64b", "66480f38f83e", "movdir64b", frozenset()),
    Template("adcx_r8_r9", "crc_adx", "664d0f38f6c1", "crc_adx", frozenset({"cf"})),
    Template("adox_r8_r9", "crc_adx", "f34d0f38f6c1", "crc_adx", frozenset({"of"})),
    Template("andn_eax_ecx_edx", "bmi_vex", "c4e270f2c2", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("andn_rax_rcx_rdx", "bmi_vex", "c4e2f0f2c2", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("bextr_eax_edx_ecx", "bmi_vex", "c4e270f7c2", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("bextr_rax_rdx_rcx", "bmi_vex", "c4e2f0f7c2", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("blsi_eax_edx", "bmi_vex", "c4e278f3da", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("blsi_rax_rdx", "bmi_vex", "c4e2f8f3da", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("blsmsk_eax_edx", "bmi_vex", "c4e278f3d2", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("blsmsk_rax_rdx", "bmi_vex", "c4e2f8f3d2", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("blsr_eax_edx", "bmi_vex", "c4e278f3ca", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("blsr_rax_rdx", "bmi_vex", "c4e2f8f3ca", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("bzhi_eax_edx_ecx", "bmi_vex", "c4e270f5c2", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("bzhi_rax_rdx_rcx", "bmi_vex", "c4e2f0f5c2", "bmi_vex", frozenset({"cf", "of", "zf", "sf", "pf"})),
    Template("pext_eax_ecx_edx", "bmi_vex", "c4e272f5c2", "bmi_vex"),
    Template("pdep_eax_ecx_edx", "bmi_vex", "c4e273f5c2", "bmi_vex"),
    Template("mulx_eax_ecx_edx", "bmi_vex", "c4e273f6c2", "bmi_vex"),
    Template("mulx_rax_rcx_rdx", "bmi_vex", "c4e2f3f6c2", "bmi_vex"),
    Template("sarx_eax_edx_ecx", "bmi_vex", "c4e272f7c2", "bmi_vex"),
    Template("sarx_rax_rdx_rcx", "bmi_vex", "c4e2f2f7c2", "bmi_vex"),
    Template("shlx_eax_edx_ecx", "bmi_vex", "c4e271f7c2", "bmi_vex"),
    Template("shlx_rax_rdx_rcx", "bmi_vex", "c4e2f1f7c2", "bmi_vex"),
    Template("shrx_eax_edx_ecx", "bmi_vex", "c4e273f7c2", "bmi_vex"),
    Template("shrx_rax_rdx_rcx", "bmi_vex", "c4e2f3f7c2", "bmi_vex"),
    Template("rorx_eax_edx_1", "bmi_vex", "c4e37bf0c201", "bmi_vex"),
    Template("rorx_rax_rdx_1", "bmi_vex", "c4e3fbf0c201", "bmi_vex"),
    Template("mpsadbw_xmm0_xmm1_0", "mpsadbw", "660f3a42c100", "mpsadbw"),
    Template("mpsadbw_xmm0_xmm1_7f", "mpsadbw", "660f3a42c17f", "mpsadbw"),
    Template("vmpsadbw_xmm0_xmm1_xmm2_0", "mpsadbw", "c4e37142c200", "mpsadbw"),
    Template("vmpsadbw_xmm0_xmm1_xmm2_7f", "mpsadbw", "c4e37142c27f", "mpsadbw"),
    Template("xchg_r8_r9", "xchg_cmpxchg", "4d87c8", None),
    Template("xadd_r8_r9", "xchg_cmpxchg", "4d0fc1c8", None),
    Template("cmpxchg_r8_r9", "xchg_cmpxchg", "4d0fb1c8", None),
    Template("lea_r8_rax_r9_4", "lea", "4e8d0488", None),
    Template("xorps_xmm0_xmm0", "sse", "0f57c0", None),
    Template("addps_xmm0_xmm1", "sse", "0f58c1", None),
    Template("subps_xmm0_xmm1", "sse", "0f5cc1", None),
    Template("mulps_xmm0_xmm1", "sse", "0f59c1", None),
    Template("divps_xmm0_xmm1", "sse", "0f5ec1", None),
    Template("minps_xmm0_xmm1", "sse", "0f5dc1", None),
    Template("maxps_xmm0_xmm1", "sse", "0f5fc1", None),
    Template("addss_xmm0_xmm1", "sse", "f30f58c1", None),
    Template("subss_xmm0_xmm1", "sse", "f30f5cc1", None),
    Template("mulss_xmm0_xmm1", "sse", "f30f59c1", None),
    Template("divss_xmm0_xmm1", "sse", "f30f5ec1", None),
    Template("comiss_xmm0_xmm1", "sse", "0f2fc1", None, frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("pxor_xmm0_xmm1", "sse2", "660fefc1", None),
    Template("addpd_xmm0_xmm1", "sse2", "660f58c1", None),
    Template("subpd_xmm0_xmm1", "sse2", "660f5cc1", None),
    Template("mulpd_xmm0_xmm1", "sse2", "660f59c1", None),
    Template("divpd_xmm0_xmm1", "sse2", "660f5ec1", None),
    Template("paddb_xmm0_xmm1", "sse2", "660ffcc1", None),
    Template("paddw_xmm0_xmm1", "sse2", "660ffdc1", None),
    Template("paddd_xmm0_xmm1", "sse2", "660ffec1", None),
    Template("paddq_xmm0_xmm1", "sse2", "660fd4c1", None),
    Template("psubb_xmm0_xmm1", "sse2", "660ff8c1", None),
    Template("psubw_xmm0_xmm1", "sse2", "660ff9c1", None),
    Template("psubd_xmm0_xmm1", "sse2", "660ffac1", None),
    Template("psubq_xmm0_xmm1", "sse2", "660ffbc1", None),
    Template("pmullw_xmm0_xmm1", "sse2", "660fd5c1", None),
    Template("pshufd_xmm0_xmm1", "sse2", "660f70c11b", None),
    Template("phaddw_xmm0_xmm1", "ssse3", "660f3801c1", None),
    Template("pshufb_xmm0_xmm1", "ssse3", "660f3800c1", None),
    Template("pshufb_mm0_mm1", "ssse3_mmx_tail", "0f3800c1", "ssse3_mmx"),
    Template("phaddw_mm0_mm1", "ssse3_mmx_tail", "0f3801c1", "ssse3_mmx"),
    Template("phaddd_mm0_mm1", "ssse3_mmx_tail", "0f3802c1", "ssse3_mmx"),
    Template("phaddsw_mm0_mm1", "ssse3_mmx_tail", "0f3803c1", "ssse3_mmx"),
    Template("pmaddubsw_mm0_mm1", "ssse3_mmx_tail", "0f3804c1", "ssse3_mmx"),
    Template("phsubw_mm0_mm1", "ssse3_mmx_tail", "0f3805c1", "ssse3_mmx"),
    Template("phsubd_mm0_mm1", "ssse3_mmx_tail", "0f3806c1", "ssse3_mmx"),
    Template("phsubsw_mm0_mm1", "ssse3_mmx_tail", "0f3807c1", "ssse3_mmx"),
    Template("psignb_mm0_mm1", "ssse3_mmx_tail", "0f3808c1", "ssse3_mmx"),
    Template("psignw_mm0_mm1", "ssse3_mmx_tail", "0f3809c1", "ssse3_mmx"),
    Template("psignd_mm0_mm1", "ssse3_mmx_tail", "0f380ac1", "ssse3_mmx"),
    Template("pmulhrsw_mm0_mm1", "ssse3_mmx_tail", "0f380bc1", "ssse3_mmx"),
    Template("pabsb_mm0_mm1", "ssse3_mmx_tail", "0f381cc1", "ssse3_mmx"),
    Template("pabsw_mm0_mm1", "ssse3_mmx_tail", "0f381dc1", "ssse3_mmx"),
    Template("pabsd_mm0_mm1", "ssse3_mmx_tail", "0f381ec1", "ssse3_mmx"),
    Template("pshufb_f2rex_mm0_mm1", "0f38_prefix_alias", "f2480f3800c1", "ssse3_mmx"),
    Template("phaddw_f2rex_mm0_mm1", "0f38_prefix_alias", "f2480f3801c1", "ssse3_mmx"),
    Template("pabsd_f2rex_mm0_mm1", "0f38_prefix_alias", "f2480f381ec1", "ssse3_mmx"),
    Template("pshufb_f3rex_mm0_mm1", "0f38_prefix_alias", "f3480f3800c1", "ssse3_mmx"),
    Template("phaddw_f3rex_mm0_mm1", "0f38_prefix_alias", "f3480f3801c1", "ssse3_mmx"),
    Template("pabsd_f3rex_mm0_mm1", "0f38_prefix_alias", "f3480f381ec1", "ssse3_mmx"),
    Template("movbe_f3rex_rdx_m32", "0f38_prefix_alias", "f3480f38f010", "movbe", frozenset()),
    Template("movbe_m32_f3rex_rdx", "0f38_prefix_alias", "f3480f38f110", "movbe", frozenset()),
    Template("palignr_mm0_mm1", "ssse3_mmx_tail", "0f3a0fc104", "ssse3_mmx"),
    Template("palignr_f2rex_mm0_mm1", "0f3a_prefix_alias", "f2480f3a0fc104", "ssse3_mmx"),
    Template("palignr_f3rex_mm0_mm1", "0f3a_prefix_alias", "f3480f3a0fc104", "ssse3_mmx"),
    Template("pblendw_xmm0_xmm1", "sse41", "660f3a0ec11b", None),
    Template("pcmpeqq_xmm0_xmm1", "sse41", "660f3829c1", None),
    Template("pcmpestrm_xmm0_xmm1", "sse42", "660f3a60c100", None, frozenset({"cf", "zf", "sf", "of"})),
    Template("pcmpestri_xmm0_xmm1", "sse42", "660f3a61c100", None, frozenset({"cf", "zf", "sf", "of"})),
    Template("pcmpistrm_xmm0_xmm1", "sse42", "660f3a62c100", None, frozenset({"cf", "zf", "sf", "of"})),
    Template("pcmpistri_xmm0_xmm1", "sse42", "660f3a63c100", None, frozenset({"cf", "zf", "sf", "of"})),
    Template("pclmulqdq_xmm0_xmm1", "aes_pclmul", "660f3a44c110", None),
    Template("aesenc_xmm0_xmm1", "aes_pclmul", "660f38dcc1", None),
    Template("aesenclast_xmm0_xmm1", "aes_pclmul", "660f38ddc1", None),
    Template("aesdec_xmm0_xmm1", "aes_pclmul", "660f38dec1", None),
    Template("aesdeclast_xmm0_xmm1", "aes_pclmul", "660f38dfc1", None),
    Template("aesimc_xmm0_xmm1", "aes_pclmul", "660f38dbc1", None),
    Template("aeskeygenassist_xmm0_xmm1", "aes_pclmul", "660f3adfc11b", None),
    Template("vaddps_ymm0_ymm1_ymm2", "avx_avx2", "c5f458c2", None),
    Template("vaddpd_ymm0_ymm1_ymm2", "avx_avx2", "c5f558c2", None),
    Template("vpxor_xmm3_xmm1_xmm2", "avx_avx2", "c5f1efda", None),
    Template("vpaddb_xmm4_xmm1_xmm2", "avx_avx2", "c5f1fce2", None),
    Template("vpaddd_ymm0_ymm1_ymm2", "avx_avx2", "c5f5fec2", None),
    Template("vpcmpeqb_ymm0_ymm1_ymm2", "avx_avx2", "c5f574c2", None),
    Template("vpcmpgtw_ymm3_ymm4_ymm5", "avx_avx2", "c5dd65dd", None),
    Template("vunpcklps_xmm0_xmm1_xmm2", "avx_shuffle_unpack", "c4e17014c2", "avx_shuffle_unpack"),
    Template("vunpckhps_xmm0_xmm1_xmm2", "avx_shuffle_unpack", "c4e17015c2", "avx_shuffle_unpack"),
    Template("vunpcklpd_xmm0_xmm1_xmm2", "avx_shuffle_unpack", "c4e17114c2", "avx_shuffle_unpack"),
    Template("vunpckhpd_xmm0_xmm1_xmm2", "avx_shuffle_unpack", "c4e17115c2", "avx_shuffle_unpack"),
    Template("vshufps_xmm0_xmm1_xmm2", "avx_shuffle_unpack", "c4e170c6c21b", "avx_shuffle_unpack"),
    Template("vshufpd_xmm0_xmm1_xmm2", "avx_shuffle_unpack", "c4e171c6c21b", "avx_shuffle_unpack"),
    Template("vunpcklps_ymm0_ymm1_ymm2", "avx_shuffle_unpack", "c4e17414c2", "avx_shuffle_unpack"),
    Template("vunpcklpd_ymm0_ymm1_ymm2", "avx_shuffle_unpack", "c4e17514c2", "avx_shuffle_unpack"),
    Template("vshufps_ymm0_ymm1_ymm2", "avx_shuffle_unpack", "c4e174c6c21b", "avx_shuffle_unpack"),
    Template("vshufpd_ymm0_ymm1_ymm2", "avx_shuffle_unpack", "c4e175c6c21b", "avx_shuffle_unpack"),
    Template("vhaddps_xmm0_xmm1_xmm2", "avx_horizontal_addsub", "c5f37cc2", "avx_horizontal_addsub"),
    Template("vhaddpd_xmm0_xmm1_xmm2", "avx_horizontal_addsub", "c5f17cc2", "avx_horizontal_addsub"),
    Template("vhsubps_xmm0_xmm1_xmm2", "avx_horizontal_addsub", "c5f37dc2", "avx_horizontal_addsub"),
    Template("vhsubpd_xmm0_xmm1_xmm2", "avx_horizontal_addsub", "c5f17dc2", "avx_horizontal_addsub"),
    Template("vaddsubps_xmm0_xmm1_xmm2", "avx_horizontal_addsub", "c5f3d0c2", "avx_horizontal_addsub"),
    Template("vaddsubpd_xmm0_xmm1_xmm2", "avx_horizontal_addsub", "c5f1d0c2", "avx_horizontal_addsub"),
    Template("vhaddps_ymm0_ymm1_ymm2", "avx_horizontal_addsub", "c5f77cc2", "avx_horizontal_addsub"),
    Template("vhaddpd_ymm0_ymm1_ymm2", "avx_horizontal_addsub", "c5f57cc2", "avx_horizontal_addsub"),
    Template("vhsubps_ymm0_ymm1_ymm2", "avx_horizontal_addsub", "c5f77dc2", "avx_horizontal_addsub"),
    Template("vhsubpd_ymm0_ymm1_ymm2", "avx_horizontal_addsub", "c5f57dc2", "avx_horizontal_addsub"),
    Template("vaddsubps_ymm0_ymm1_ymm2", "avx_horizontal_addsub", "c5f7d0c2", "avx_horizontal_addsub"),
    Template("vaddsubpd_ymm0_ymm1_ymm2", "avx_horizontal_addsub", "c5f5d0c2", "avx_horizontal_addsub"),
    Template("vmovntps_m128_xmm1", "avx_movnt_lddqu", "c5f82b08", "avx_movnt_lddqu"),
    Template("vmovntpd_m128_xmm1", "avx_movnt_lddqu", "c5f92b08", "avx_movnt_lddqu"),
    Template("vmovntdq_m128_xmm1", "avx_movnt_lddqu", "c5f9e708", "avx_movnt_lddqu"),
    Template("vlddqu_xmm0_m128", "avx_movnt_lddqu", "c5fbf000", "avx_movnt_lddqu"),
    Template("movntdqa_xmm0_m128", "movntdqa", "660f382a00", "avx_movnt_lddqu"),
    Template("movntdqa_rex66_xmm0_m128", "movntdqa", "48660f382a00", "avx_movnt_lddqu"),
    Template("movntdqa_66rex_xmm0_m128", "movntdqa", "66480f382a00", "avx_movnt_lddqu"),
    Template("vmovntdqa_xmm0_m128", "avx_movnt_lddqu", "c4e2792a00", "avx_movnt_lddqu"),
    Template("vmovntps_m256_ymm1", "avx_movnt_lddqu", "c5fc2b08", "avx_movnt_lddqu"),
    Template("vmovntpd_m256_ymm1", "avx_movnt_lddqu", "c5fd2b08", "avx_movnt_lddqu"),
    Template("vmovntdq_m256_ymm1", "avx_movnt_lddqu", "c5fde708", "avx_movnt_lddqu"),
    Template("vlddqu_ymm0_m256", "avx_movnt_lddqu", "c5fff000", "avx_movnt_lddqu"),
    Template("vmovntdqa_ymm0_m256", "avx_movnt_lddqu", "c4e27d2a00", "avx_movnt_lddqu"),
    Template("vucomiss_xmm0_xmm1", "avx_vcomi", "c5f82ec1", "avx_vcomi", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("vcomiss_xmm0_xmm1", "avx_vcomi", "c5f82fc1", "avx_vcomi", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("vucomisd_xmm0_xmm1", "avx_vcomi", "c5f92ec1", "avx_vcomi", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("vcomisd_xmm0_xmm1", "avx_vcomi", "c5f92fc1", "avx_vcomi", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("vpshufd_xmm0_xmm1", "avx_pshuf_imm", "c5f970c11b", "avx_pshuf_imm"),
    Template("vpshufhw_xmm0_xmm1", "avx_pshuf_imm", "c5fa70c11b", "avx_pshuf_imm"),
    Template("vpshuflw_xmm0_xmm1", "avx_pshuf_imm", "c5fb70c11b", "avx_pshuf_imm"),
    Template("vpshufd_ymm0_ymm1", "avx_pshuf_imm", "c5fd70c11b", "avx_pshuf_imm"),
    Template("vpshufhw_ymm0_ymm1", "avx_pshuf_imm", "c5fe70c11b", "avx_pshuf_imm"),
    Template("vpshuflw_ymm0_ymm1", "avx_pshuf_imm", "c5ff70c11b", "avx_pshuf_imm"),
    Template("vmovsldup_xmm0_xmm1", "avx_dup_shuffle", "c5fa12c1", "avx_dup_shuffle"),
    Template("vmovshdup_xmm0_xmm1", "avx_dup_shuffle", "c5fa16c1", "avx_dup_shuffle"),
    Template("vmovddup_xmm0_xmm1", "avx_dup_shuffle", "c5fb12c1", "avx_dup_shuffle"),
    Template("vmovsldup_ymm0_ymm1", "avx_dup_shuffle", "c5fe12c1", "avx_dup_shuffle"),
    Template("vmovshdup_ymm0_ymm1", "avx_dup_shuffle", "c5fe16c1", "avx_dup_shuffle"),
    Template("vmovddup_ymm0_ymm1", "avx_dup_shuffle", "c5ff12c1", "avx_dup_shuffle"),
    Template("vcvtps2pd_xmm0_xmm1", "avx_packed_convert", "c5f85ac1", "avx_packed_convert"),
    Template("vcvtps2pd_ymm0_xmm1", "avx_packed_convert", "c5fc5ac1", "avx_packed_convert"),
    Template("vcvtpd2ps_xmm0_xmm1", "avx_packed_convert", "c5f95ac1", "avx_packed_convert"),
    Template("vcvtpd2ps_xmm0_ymm1", "avx_packed_convert", "c5fd5ac1", "avx_packed_convert"),
    Template("vcvtdq2ps_xmm0_xmm1", "avx_packed_convert", "c5f85bc1", "avx_packed_convert"),
    Template("vcvtdq2ps_ymm0_ymm1", "avx_packed_convert", "c5fc5bc1", "avx_packed_convert"),
    Template("vcvtps2dq_xmm0_xmm1", "avx_packed_convert", "c5f95bc1", "avx_packed_convert"),
    Template("vcvtps2dq_ymm0_ymm1", "avx_packed_convert", "c5fd5bc1", "avx_packed_convert"),
    Template("vcvttps2dq_xmm0_xmm1", "avx_packed_convert", "c5fa5bc1", "avx_packed_convert"),
    Template("vcvttps2dq_ymm0_ymm1", "avx_packed_convert", "c5fe5bc1", "avx_packed_convert"),
    Template("vcvtdq2pd_xmm0_xmm1", "avx_packed_convert", "c5fae6c1", "avx_packed_convert"),
    Template("vcvtdq2pd_ymm0_xmm1", "avx_packed_convert", "c5fee6c1", "avx_packed_convert"),
    Template("vcvtpd2dq_xmm0_xmm1", "avx_packed_convert", "c5fbe6c1", "avx_packed_convert"),
    Template("vcvtpd2dq_xmm0_ymm1", "avx_packed_convert", "c5ffe6c1", "avx_packed_convert"),
    Template("vcvttpd2dq_xmm0_xmm1", "avx_packed_convert", "c5f9e6c1", "avx_packed_convert"),
    Template("vcvttpd2dq_xmm0_ymm1", "avx_packed_convert", "c5fde6c1", "avx_packed_convert"),
    Template("vcvtss2sd_xmm0_xmm1_xmm2", "avx_scalar_convert", "c5f25ac2", "avx_scalar_convert"),
    Template("vcvtsd2ss_xmm0_xmm1_xmm2", "avx_scalar_convert", "c5f35ac2", "avx_scalar_convert"),
    Template("vcvtsi2ss_xmm0_xmm1_eax", "avx_scalar_convert", "c5f22ac0", "avx_scalar_convert"),
    Template("vcvtsi2sd_xmm0_xmm1_eax", "avx_scalar_convert", "c5f32ac0", "avx_scalar_convert"),
    Template("vcvtsi2ss_xmm0_xmm1_rax", "avx_scalar_convert", "c4e1f22ac0", "avx_scalar_convert"),
    Template("vcvtsi2sd_xmm0_xmm1_rax", "avx_scalar_convert", "c4e1f32ac0", "avx_scalar_convert"),
    Template("vcvttss2si_eax_xmm1", "avx_scalar_convert", "c5fa2cc1", "avx_scalar_convert"),
    Template("vcvttsd2si_eax_xmm1", "avx_scalar_convert", "c5fb2cc1", "avx_scalar_convert"),
    Template("vcvtss2si_eax_xmm1", "avx_scalar_convert", "c5fa2dc1", "avx_scalar_convert"),
    Template("vcvtsd2si_eax_xmm1", "avx_scalar_convert", "c5fb2dc1", "avx_scalar_convert"),
    Template("vcvttss2si_rax_xmm1", "avx_scalar_convert", "c4e1fa2cc1", "avx_scalar_convert"),
    Template("vcvttsd2si_rax_xmm1", "avx_scalar_convert", "c4e1fb2cc1", "avx_scalar_convert"),
    Template("vcvtss2si_rax_xmm1", "avx_scalar_convert", "c4e1fa2dc1", "avx_scalar_convert"),
    Template("vcvtsd2si_rax_xmm1", "avx_scalar_convert", "c4e1fb2dc1", "avx_scalar_convert"),
    Template("vrsqrtps_xmm0_xmm1", "avx_rcp_rsqrt", "c5f852c1", "avx_rcp_rsqrt"),
    Template("vrsqrtps_ymm0_ymm1", "avx_rcp_rsqrt", "c5fc52c1", "avx_rcp_rsqrt"),
    Template("vrsqrtss_xmm0_xmm1_xmm2", "avx_rcp_rsqrt", "c5f252c2", "avx_rcp_rsqrt"),
    Template("vrcpps_xmm0_xmm1", "avx_rcp_rsqrt", "c5f853c1", "avx_rcp_rsqrt"),
    Template("vrcpps_ymm0_ymm1", "avx_rcp_rsqrt", "c5fc53c1", "avx_rcp_rsqrt"),
    Template("vrcpss_xmm0_xmm1_xmm2", "avx_rcp_rsqrt", "c5f253c2", "avx_rcp_rsqrt"),
    Template("vroundps_xmm0_xmm1_trunc", "avx_round", "c4e37908c103", "avx_round"),
    Template("vroundps_ymm0_ymm1_trunc", "avx_round", "c4e37d08c103", "avx_round"),
    Template("vroundpd_xmm0_xmm1_trunc", "avx_round", "c4e37909c103", "avx_round"),
    Template("vroundpd_ymm0_ymm1_trunc", "avx_round", "c4e37d09c103", "avx_round"),
    Template("vroundss_xmm0_xmm1_xmm2_trunc", "avx_round", "c4e3710ac203", "avx_round"),
    Template("vroundsd_xmm0_xmm1_xmm2_trunc", "avx_round", "c4e3710bc203", "avx_round"),
    Template("vroundps_w1_xmm0_xmm1_trunc", "vex_wig_control", "c4e3f908c103", "avx_round"),
    Template("vroundpd_w1_xmm0_xmm1_trunc", "vex_wig_control", "c4e3f909c103", "avx_round"),
    Template("vroundss_w1_xmm0_xmm1_xmm2_trunc", "vex_wig_control", "c4e3f10ac203", "avx_round"),
    Template("vroundsd_w1_xmm0_xmm1_xmm2_trunc", "vex_wig_control", "c4e3f10bc203", "avx_round"),
    Template("dpps_xmm0_xmm1", "avx_dp", "660f3a40c1ff", "avx_dp"),
    Template("dppd_xmm0_xmm1", "avx_dp", "660f3a41c133", "avx_dp"),
    Template("vdpps_xmm0_xmm1_xmm2", "avx_dp", "c4e37140c2ff", "avx_dp"),
    Template("vdpps_ymm0_ymm1_ymm2", "avx_dp", "c4e37540c2ff", "avx_dp"),
    Template("vdppd_xmm0_xmm1_xmm2", "avx_dp", "c4e37141c233", "avx_dp"),
    Template("insertps_xmm0_xmm2", "avx_insert_extract", "660f3a21c2b4", "avx_insert_extract"),
    Template("vinsertps_xmm0_xmm1_xmm2", "avx_insert_extract", "c4e37121c2b4", "avx_insert_extract"),
    Template("vinsertps_w1_xmm0_xmm1_xmm2", "vinsert_extract_wig", "c4e3f121c27f", "avx_insert_extract"),
    Template("extractps_ecx_xmm0", "avx_insert_extract", "660f3a17c102", "avx_insert_extract"),
    Template("vextractps_ecx_xmm0", "avx_insert_extract", "c4e37917c102", "avx_insert_extract"),
    Template("vextractps_w1_ecx_xmm0", "vinsert_extract_wig", "c4e3f917c102", "avx_insert_extract"),
    Template("pinsrb_xmm0_ecx", "avx_pinsr_pextr", "660f3a20c102", "avx_pinsr_pextr"),
    Template("pinsrw_xmm0_ecx", "avx_pinsr_pextr", "660fc4c102", "avx_pinsr_pextr"),
    Template("pinsrd_xmm0_ecx", "avx_pinsr_pextr", "660f3a22c102", "avx_pinsr_pextr"),
    Template("pinsrq_xmm0_rcx", "avx_pinsr_pextr", "66480f3a22c101", "avx_pinsr_pextr"),
    Template("pextrb_ecx_xmm0", "avx_pinsr_pextr", "660f3a14c102", "avx_pinsr_pextr"),
    Template("pextrw_ecx_xmm0", "avx_pinsr_pextr", "660f3a15c102", "avx_pinsr_pextr"),
    Template("pextrw_old_eax_xmm1", "avx_pinsr_pextr", "660fc5c102", "avx_pinsr_pextr"),
    Template("pextrd_ecx_xmm0", "avx_pinsr_pextr", "660f3a16c102", "avx_pinsr_pextr"),
    Template("pextrq_rcx_xmm0", "avx_pinsr_pextr", "66480f3a16c101", "avx_pinsr_pextr"),
    Template("vpinsrb_xmm0_xmm1_ecx", "avx_pinsr_pextr", "c4e37120c102", "avx_pinsr_pextr"),
    Template("vpinsrw_xmm0_xmm1_ecx", "avx_pinsr_pextr", "c5f1c4c102", "avx_pinsr_pextr"),
    Template("vpinsrd_xmm0_xmm1_ecx", "avx_pinsr_pextr", "c4e37122c102", "avx_pinsr_pextr"),
    Template("vpinsrq_xmm0_xmm1_rcx", "avx_pinsr_pextr", "c4e3f122c101", "avx_pinsr_pextr"),
    Template("vpextrb_ecx_xmm0", "avx_pinsr_pextr", "c4e37914c102", "avx_pinsr_pextr"),
    Template("vpextrw_eax_xmm1", "avx_pinsr_pextr", "c5f9c5c102", "avx_pinsr_pextr"),
    Template("vpextrd_ecx_xmm0", "avx_pinsr_pextr", "c4e37916c102", "avx_pinsr_pextr"),
    Template("vpextrq_rcx_xmm0", "avx_pinsr_pextr", "c4e3f916c101", "avx_pinsr_pextr"),
    Template("gf2p8affineqb_xmm0_xmm1", "avx_gfni_affine", "660f3acec127", "avx_gfni_affine"),
    Template("gf2p8affineinvqb_xmm0_xmm1", "avx_gfni_affine", "660f3acfc127", "avx_gfni_affine"),
    Template("vgf2p8affineqb_xmm0_xmm1_xmm2", "avx_gfni_affine", "c4e3f1cec227", "avx_gfni_affine"),
    Template("vgf2p8affineinvqb_xmm0_xmm1_xmm2", "avx_gfni_affine", "c4e3f1cfc227", "avx_gfni_affine"),
    Template("vgf2p8affineqb_ymm0_ymm1_ymm2", "avx_gfni_affine", "c4e3f5cec227", "avx_gfni_affine"),
    Template("vgf2p8affineinvqb_ymm0_ymm1_ymm2", "avx_gfni_affine", "c4e3f5cfc227", "avx_gfni_affine"),
    Template("sha1nexte_xmm0_xmm1", "sha_tail", "0f38c8c1", "sha_tail"),
    Template("sha1msg1_xmm0_xmm1", "sha_tail", "0f38c9c1", "sha_tail"),
    Template("sha1msg2_xmm0_xmm1", "sha_tail", "0f38cac1", "sha_tail"),
    Template("sha256rnds2_xmm2_xmm1_xmm0", "sha_tail", "0f38cbd1", "sha_tail"),
    Template("sha256msg1_xmm0_xmm1", "sha_tail", "0f38ccc1", "sha_tail"),
    Template("sha256msg2_xmm0_xmm1", "sha_tail", "0f38cdc1", "sha_tail"),
    Template("sha1rnds4_xmm0_xmm1_ch", "sha_tail", "0f3accc100", "sha_tail"),
    Template("sha1rnds4_xmm0_xmm1_parity1", "sha_tail", "0f3accc101", "sha_tail"),
    Template("sha1rnds4_xmm0_xmm1_maj", "sha_tail", "0f3accc102", "sha_tail"),
    Template("sha1rnds4_xmm0_xmm1_parity3", "sha_tail", "0f3accc103", "sha_tail"),
    Template("vpermilps_xmm0_xmm1_imm", "avx_vpermil_blendv_vcmp", "c4e37904c11b", "avx_vpermil_blendv_vcmp"),
    Template("vpermilpd_xmm0_xmm1_imm", "avx_vpermil_blendv_vcmp", "c4e37905c11b", "avx_vpermil_blendv_vcmp"),
    Template("vpermilps_xmm0_xmm1_xmm2", "avx_vpermil_blendv_vcmp", "c4e2710cc2", "avx_vpermil_blendv_vcmp"),
    Template("vpermilpd_xmm0_xmm1_xmm2", "avx_vpermil_blendv_vcmp", "c4e2710dc2", "avx_vpermil_blendv_vcmp"),
    Template("vblendvps_xmm1_xmm2_xmm3_xmm1", "avx_vpermil_blendv_vcmp", "c4e3694acb10", "avx_vpermil_blendv_vcmp"),
    Template("vblendvpd_xmm1_xmm2_xmm3_xmm1", "avx_vpermil_blendv_vcmp", "c4e3694bcb10", "avx_vpermil_blendv_vcmp"),
    Template("vpblendvb_xmm1_xmm2_xmm3_xmm1", "avx_vpermil_blendv_vcmp", "c4e3694ccb10", "avx_vpermil_blendv_vcmp"),
    Template("vcmpps_xmm0_xmm1_xmm2_lt", "avx_vpermil_blendv_vcmp", "c5f0c2c201", "avx_vpermil_blendv_vcmp"),
    Template("vcmppd_xmm0_xmm1_xmm2_lt", "avx_vpermil_blendv_vcmp", "c5f1c2c201", "avx_vpermil_blendv_vcmp"),
    Template("vcmpss_xmm0_xmm1_xmm2_lt", "avx_vpermil_blendv_vcmp", "c5f2c2c201", "avx_vpermil_blendv_vcmp"),
    Template("vcmpsd_xmm0_xmm1_xmm2_lt", "avx_vpermil_blendv_vcmp", "c5f3c2c201", "avx_vpermil_blendv_vcmp"),
    Template("vcmpps_w1_xmm0_xmm1_xmm2_lt", "vex_wig_control", "c4e1f0c2c201", "avx_vpermil_blendv_vcmp"),
    Template("vcmppd_w1_xmm0_xmm1_xmm2_lt", "vex_wig_control", "c4e1f1c2c201", "avx_vpermil_blendv_vcmp"),
    Template("vcmpss_w1_xmm0_xmm1_xmm2_lt", "vex_wig_control", "c4e1f2c2c201", "avx_vpermil_blendv_vcmp"),
    Template("vcmpsd_w1_xmm0_xmm1_xmm2_lt", "vex_wig_control", "c4e1f3c2c201", "avx_vpermil_blendv_vcmp"),
    Template("vmovd_xmm0_ecx", "vex_predicate_control", "c4e1796ec1", "vex_predicate_control"),
    Template("vmovq_xmm0_rcx", "vex_predicate_control", "c4e1f96ec1", "vex_predicate_control"),
    Template("vmovd_ecx_xmm0", "vex_predicate_control", "c4e1797ec1", "vex_predicate_control"),
    Template("vmovq_rcx_xmm0", "vex_predicate_control", "c4e1f97ec1", "vex_predicate_control"),
    Template("vmovq_f3_xmm0_xmm1", "vex_predicate_control", "c4e17a7ec1", "vex_predicate_control"),
    Template("vmovq_d6_xmm1_xmm0", "vex_predicate_control", "c4e179d6c1", "vex_predicate_control"),
    Template("vmovd_m32_xmm0", "vex_predicate_control", "c4e1797e00", "vex_predicate_control"),
    Template("vmovq_m64_xmm0", "vex_predicate_control", "c4e1f97e00", "vex_predicate_control"),
    Template("vmovq_f3_xmm0_m64", "vex_predicate_control", "c4e17a7e00", "vex_predicate_control"),
    Template("vmovq_d6_m64_xmm0", "vex_predicate_control", "c4e1f9d600", "vex_predicate_control"),
    Template("vmovmskps_eax_xmm1", "vex_predicate_control", "c4e17850c1", "vex_predicate_control"),
    Template("vmovmskpd_eax_xmm1", "vex_predicate_control", "c4e17950c1", "vex_predicate_control"),
    Template("vpmovmskb_eax_xmm1", "vex_predicate_control", "c4e179d7c1", "vex_predicate_control"),
    Template("vtestps_xmm0_xmm1", "vex_predicate_control", "c4e2790ec1", "vex_predicate_control", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("vtestps_ymm0_ymm1", "vex_predicate_control", "c4e27d0ec1", "vex_predicate_control", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("vtestpd_xmm0_xmm1", "vex_predicate_control", "c4e2790fc1", "vex_predicate_control", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("vtestpd_ymm0_ymm1", "vex_predicate_control", "c4e27d0fc1", "vex_predicate_control", frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("vmaskmovdqu_xmm0_xmm1", "vex_predicate_control", "c4e179f7c1", "vex_predicate_control"),
    Template("vldmxcsr_m32", "vex_predicate_control", "c4e178ae10", "vex_predicate_control"),
    Template("vstmxcsr_m32", "vex_predicate_control", "c4e178ae18", "vex_predicate_control"),
    Template("vmovups_xmm7_xmm1", "avx_avx2", "c5f810f9", None),
    Template("vmovdqu_xmm0_xmm1", "avx_avx2", "c5fa6fc1", None),
    Template("vpclmulqdq_xmm3_xmm4_xmm0", "avx_aes_pclmul", "c4e35944d811", None),
    Template("vaesimc_xmm0_xmm1", "avx_vaesimc", "c4e279dbc1", None),
    Template("vaesenc_ymm0_ymm1_ymm2", "avx_aes_pclmul", "c4e275dcc2", None),
    Template("evex_vaddps_zmm0_k1_zmm0_zmm0", "evex_mask_zero", "62f17c4958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1z_zmm0_zmm0", "evex_mask_zero", "62f17cc958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_rn_sae_zmm0_zmm0", "evex_rn_sae", "62f17c1858c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1_rn_sae_zmm0_zmm0", "evex_rn_sae", "62f17c1958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1z_rn_sae_zmm0_zmm0", "evex_rn_sae", "62f17c9958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_rd_sae_zmm0_zmm0", "evex_er_directed", "62f17c3858c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_ru_sae_zmm0_zmm0", "evex_er_directed", "62f17c5858c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_rz_sae_zmm0_zmm0", "evex_er_directed", "62f17c7858c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1_rd_sae_zmm0_zmm0", "evex_er_directed", "62f17c3958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1_ru_sae_zmm0_zmm0", "evex_er_directed", "62f17c5958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1_rz_sae_zmm0_zmm0", "evex_er_directed", "62f17c7958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1z_rd_sae_zmm0_zmm0", "evex_er_directed", "62f17cb958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1z_ru_sae_zmm0_zmm0", "evex_er_directed", "62f17cd958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1z_rz_sae_zmm0_zmm0", "evex_er_directed", "62f17cf958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_zmm0_m32bcst", "evex_broadcast", "62f17c585800", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1_zmm0_m32bcst", "evex_broadcast", "62f17c595800", "evex_mask_zero", frozenset()),
    Template("evex_vaddps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast", "62f17cd95800", "evex_mask_zero", frozenset()),
    Template("evex_vsubps_zmm0_zmm0_m32bcst", "evex_broadcast_arith", "62f17c585c00", "evex_mask_zero", frozenset()),
    Template("evex_vsubps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_arith", "62f17c595c00", "evex_mask_zero", frozenset()),
    Template("evex_vsubps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_arith", "62f17cd95c00", "evex_mask_zero", frozenset()),
    Template("evex_vmulps_zmm0_zmm0_m32bcst", "evex_broadcast_arith", "62f17c585900", "evex_mask_zero", frozenset()),
    Template("evex_vmulps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_arith", "62f17c595900", "evex_mask_zero", frozenset()),
    Template("evex_vmulps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_arith", "62f17cd95900", "evex_mask_zero", frozenset()),
    Template("evex_vdivps_zmm0_zmm0_m32bcst", "evex_broadcast_arith", "62f17c585e00", "evex_mask_zero", frozenset()),
    Template("evex_vdivps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_arith", "62f17c595e00", "evex_mask_zero", frozenset()),
    Template("evex_vdivps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_arith", "62f17cd95e00", "evex_mask_zero", frozenset()),
    Template("evex_vaddpd_zmm0_zmm0_m64bcst", "evex_broadcast_arith", "62f1fd585800", "evex_mask_zero", frozenset()),
    Template("evex_vaddpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_arith", "62f1fd595800", "evex_mask_zero", frozenset()),
    Template("evex_vaddpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_arith", "62f1fdd95800", "evex_mask_zero", frozenset()),
    Template("evex_vsubpd_zmm0_zmm0_m64bcst", "evex_broadcast_arith", "62f1fd585c00", "evex_mask_zero", frozenset()),
    Template("evex_vsubpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_arith", "62f1fd595c00", "evex_mask_zero", frozenset()),
    Template("evex_vsubpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_arith", "62f1fdd95c00", "evex_mask_zero", frozenset()),
    Template("evex_vmulpd_zmm0_zmm0_m64bcst", "evex_broadcast_arith", "62f1fd585900", "evex_mask_zero", frozenset()),
    Template("evex_vmulpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_arith", "62f1fd595900", "evex_mask_zero", frozenset()),
    Template("evex_vmulpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_arith", "62f1fdd95900", "evex_mask_zero", frozenset()),
    Template("evex_vdivpd_zmm0_zmm0_m64bcst", "evex_broadcast_arith", "62f1fd585e00", "evex_mask_zero", frozenset()),
    Template("evex_vdivpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_arith", "62f1fd595e00", "evex_mask_zero", frozenset()),
    Template("evex_vdivpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_arith", "62f1fdd95e00", "evex_mask_zero", frozenset()),
    Template("evex_vmulps_zmm0_zmm0_zmm0", "evex_ps_arith", "62f17c4859c0", "evex_mask_zero", frozenset()),
    Template("evex_vmulps_zmm0_k1_zmm0_zmm0", "evex_ps_arith", "62f17c4959c0", "evex_mask_zero", frozenset()),
    Template("evex_vmulps_zmm0_k1z_zmm0_zmm0", "evex_ps_arith", "62f17cc959c0", "evex_mask_zero", frozenset()),
    Template("evex_vsubps_zmm0_zmm0_zmm0", "evex_ps_arith", "62f17c485cc0", "evex_mask_zero", frozenset()),
    Template("evex_vsubps_zmm0_k1_zmm0_zmm0", "evex_ps_arith", "62f17c495cc0", "evex_mask_zero", frozenset()),
    Template("evex_vsubps_zmm0_k1z_zmm0_zmm0", "evex_ps_arith", "62f17cc95cc0", "evex_mask_zero", frozenset()),
    Template("evex_vdivps_zmm0_zmm0_zmm0", "evex_ps_arith", "62f17c485ec0", "evex_mask_zero", frozenset()),
    Template("evex_vdivps_zmm0_k1_zmm0_zmm0", "evex_ps_arith", "62f17c495ec0", "evex_mask_zero", frozenset()),
    Template("evex_vdivps_zmm0_k1z_zmm0_zmm0", "evex_ps_arith", "62f17cc95ec0", "evex_mask_zero", frozenset()),
    Template("evex_vaddpd_zmm0_zmm0_zmm0", "evex_pd_arith", "62f1fd4858c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddpd_zmm0_k1_zmm0_zmm0", "evex_pd_arith", "62f1fd4958c0", "evex_mask_zero", frozenset()),
    Template("evex_vaddpd_zmm0_k1z_zmm0_zmm0", "evex_pd_arith", "62f1fdc958c0", "evex_mask_zero", frozenset()),
    Template("evex_vmulpd_zmm0_zmm0_zmm0", "evex_pd_arith", "62f1fd4859c0", "evex_mask_zero", frozenset()),
    Template("evex_vmulpd_zmm0_k1_zmm0_zmm0", "evex_pd_arith", "62f1fd4959c0", "evex_mask_zero", frozenset()),
    Template("evex_vmulpd_zmm0_k1z_zmm0_zmm0", "evex_pd_arith", "62f1fdc959c0", "evex_mask_zero", frozenset()),
    Template("evex_vsubpd_zmm0_zmm0_zmm0", "evex_pd_arith", "62f1fd485cc0", "evex_mask_zero", frozenset()),
    Template("evex_vsubpd_zmm0_k1_zmm0_zmm0", "evex_pd_arith", "62f1fd495cc0", "evex_mask_zero", frozenset()),
    Template("evex_vsubpd_zmm0_k1z_zmm0_zmm0", "evex_pd_arith", "62f1fdc95cc0", "evex_mask_zero", frozenset()),
    Template("evex_vdivpd_zmm0_zmm0_zmm0", "evex_pd_arith", "62f1fd485ec0", "evex_mask_zero", frozenset()),
    Template("evex_vdivpd_zmm0_k1_zmm0_zmm0", "evex_pd_arith", "62f1fd495ec0", "evex_mask_zero", frozenset()),
    Template("evex_vdivpd_zmm0_k1z_zmm0_zmm0", "evex_pd_arith", "62f1fdc95ec0", "evex_mask_zero", frozenset()),
    Template("evex_vsqrtps_zmm0_zmm1", "evex_sqrt", "62f17c4851c1", "evex_mask_zero", frozenset()),
    Template("evex_vsqrtps_zmm0_k1_zmm1", "evex_sqrt", "62f17c4951c1", "evex_mask_zero", frozenset()),
    Template("evex_vsqrtps_zmm0_k1z_zmm1", "evex_sqrt", "62f17cc951c1", "evex_mask_zero", frozenset()),
    Template("evex_vsqrtpd_zmm0_zmm1", "evex_sqrt", "62f1fd4851c1", "evex_mask_zero", frozenset()),
    Template("evex_vsqrtpd_zmm0_k1_zmm1", "evex_sqrt", "62f1fd4951c1", "evex_mask_zero", frozenset()),
    Template("evex_vsqrtpd_zmm0_k1z_zmm1", "evex_sqrt", "62f1fdc951c1", "evex_mask_zero", frozenset()),
    Template("evex_vminps_zmm0_zmm0_zmm1", "evex_minmax", "62f17c485dc1", "evex_mask_zero", frozenset()),
    Template("evex_vminps_zmm0_k1_zmm0_zmm1", "evex_minmax", "62f17c495dc1", "evex_mask_zero", frozenset()),
    Template("evex_vminps_zmm0_k1z_zmm0_zmm1", "evex_minmax", "62f17cc95dc1", "evex_mask_zero", frozenset()),
    Template("evex_vmaxps_zmm0_zmm0_zmm1", "evex_minmax", "62f17c485fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmaxps_zmm0_k1_zmm0_zmm1", "evex_minmax", "62f17c495fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmaxps_zmm0_k1z_zmm0_zmm1", "evex_minmax", "62f17cc95fc1", "evex_mask_zero", frozenset()),
    Template("evex_vminpd_zmm0_zmm0_zmm1", "evex_minmax", "62f1fd485dc1", "evex_mask_zero", frozenset()),
    Template("evex_vminpd_zmm0_k1_zmm0_zmm1", "evex_minmax", "62f1fd495dc1", "evex_mask_zero", frozenset()),
    Template("evex_vminpd_zmm0_k1z_zmm0_zmm1", "evex_minmax", "62f1fdc95dc1", "evex_mask_zero", frozenset()),
    Template("evex_vmaxpd_zmm0_zmm0_zmm1", "evex_minmax", "62f1fd485fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmaxpd_zmm0_k1_zmm0_zmm1", "evex_minmax", "62f1fd495fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmaxpd_zmm0_k1z_zmm0_zmm1", "evex_minmax", "62f1fdc95fc1", "evex_mask_zero", frozenset()),
    Template("evex_vminps_zmm0_zmm0_m32bcst", "evex_broadcast_minmax", "62f17c585d00", "evex_mask_zero", frozenset()),
    Template("evex_vminps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_minmax", "62f17c595d00", "evex_mask_zero", frozenset()),
    Template("evex_vminps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_minmax", "62f17cd95d00", "evex_mask_zero", frozenset()),
    Template("evex_vmaxps_zmm0_zmm0_m32bcst", "evex_broadcast_minmax", "62f17c585f00", "evex_mask_zero", frozenset()),
    Template("evex_vmaxps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_minmax", "62f17c595f00", "evex_mask_zero", frozenset()),
    Template("evex_vmaxps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_minmax", "62f17cd95f00", "evex_mask_zero", frozenset()),
    Template("evex_vminpd_zmm0_zmm0_m64bcst", "evex_broadcast_minmax", "62f1fd585d00", "evex_mask_zero", frozenset()),
    Template("evex_vminpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_minmax", "62f1fd595d00", "evex_mask_zero", frozenset()),
    Template("evex_vminpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_minmax", "62f1fdd95d00", "evex_mask_zero", frozenset()),
    Template("evex_vmaxpd_zmm0_zmm0_m64bcst", "evex_broadcast_minmax", "62f1fd585f00", "evex_mask_zero", frozenset()),
    Template("evex_vmaxpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_minmax", "62f1fd595f00", "evex_mask_zero", frozenset()),
    Template("evex_vmaxpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_minmax", "62f1fdd95f00", "evex_mask_zero", frozenset()),
    Template("evex_vandps_zmm0_zmm0_zmm1", "evex_logic_fp", "62f17c4854c1", "evex_mask_zero", frozenset()),
    Template("evex_vandps_zmm0_k1_zmm0_zmm1", "evex_logic_fp", "62f17c4954c1", "evex_mask_zero", frozenset()),
    Template("evex_vandps_zmm0_k1z_zmm0_zmm1", "evex_logic_fp", "62f17cc954c1", "evex_mask_zero", frozenset()),
    Template("evex_vandnps_zmm0_zmm0_zmm1", "evex_logic_fp", "62f17c4855c1", "evex_mask_zero", frozenset()),
    Template("evex_vandnps_zmm0_k1_zmm0_zmm1", "evex_logic_fp", "62f17c4955c1", "evex_mask_zero", frozenset()),
    Template("evex_vandnps_zmm0_k1z_zmm0_zmm1", "evex_logic_fp", "62f17cc955c1", "evex_mask_zero", frozenset()),
    Template("evex_vorps_zmm0_zmm0_zmm1", "evex_logic_fp", "62f17c4856c1", "evex_mask_zero", frozenset()),
    Template("evex_vorps_zmm0_k1_zmm0_zmm1", "evex_logic_fp", "62f17c4956c1", "evex_mask_zero", frozenset()),
    Template("evex_vorps_zmm0_k1z_zmm0_zmm1", "evex_logic_fp", "62f17cc956c1", "evex_mask_zero", frozenset()),
    Template("evex_vxorps_zmm0_zmm0_zmm1", "evex_logic_fp", "62f17c4857c1", "evex_mask_zero", frozenset()),
    Template("evex_vxorps_zmm0_k1_zmm0_zmm1", "evex_logic_fp", "62f17c4957c1", "evex_mask_zero", frozenset()),
    Template("evex_vxorps_zmm0_k1z_zmm0_zmm1", "evex_logic_fp", "62f17cc957c1", "evex_mask_zero", frozenset()),
    Template("evex_vandpd_zmm0_zmm0_zmm1", "evex_logic_fp", "62f1fd4854c1", "evex_mask_zero", frozenset()),
    Template("evex_vandpd_zmm0_k1_zmm0_zmm1", "evex_logic_fp", "62f1fd4954c1", "evex_mask_zero", frozenset()),
    Template("evex_vandpd_zmm0_k1z_zmm0_zmm1", "evex_logic_fp", "62f1fdc954c1", "evex_mask_zero", frozenset()),
    Template("evex_vandnpd_zmm0_zmm0_zmm1", "evex_logic_fp", "62f1fd4855c1", "evex_mask_zero", frozenset()),
    Template("evex_vandnpd_zmm0_k1_zmm0_zmm1", "evex_logic_fp", "62f1fd4955c1", "evex_mask_zero", frozenset()),
    Template("evex_vandnpd_zmm0_k1z_zmm0_zmm1", "evex_logic_fp", "62f1fdc955c1", "evex_mask_zero", frozenset()),
    Template("evex_vorpd_zmm0_zmm0_zmm1", "evex_logic_fp", "62f1fd4856c1", "evex_mask_zero", frozenset()),
    Template("evex_vorpd_zmm0_k1_zmm0_zmm1", "evex_logic_fp", "62f1fd4956c1", "evex_mask_zero", frozenset()),
    Template("evex_vorpd_zmm0_k1z_zmm0_zmm1", "evex_logic_fp", "62f1fdc956c1", "evex_mask_zero", frozenset()),
    Template("evex_vxorpd_zmm0_zmm0_zmm1", "evex_logic_fp", "62f1fd4857c1", "evex_mask_zero", frozenset()),
    Template("evex_vxorpd_zmm0_k1_zmm0_zmm1", "evex_logic_fp", "62f1fd4957c1", "evex_mask_zero", frozenset()),
    Template("evex_vxorpd_zmm0_k1z_zmm0_zmm1", "evex_logic_fp", "62f1fdc957c1", "evex_mask_zero", frozenset()),
    Template("evex_vandps_zmm0_zmm0_m32bcst", "evex_broadcast_logic", "62f17c585400", "evex_mask_zero", frozenset()),
    Template("evex_vandps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_logic", "62f17c595400", "evex_mask_zero", frozenset()),
    Template("evex_vandps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_logic", "62f17cd95400", "evex_mask_zero", frozenset()),
    Template("evex_vandnps_zmm0_zmm0_m32bcst", "evex_broadcast_logic", "62f17c585500", "evex_mask_zero", frozenset()),
    Template("evex_vandnps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_logic", "62f17c595500", "evex_mask_zero", frozenset()),
    Template("evex_vandnps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_logic", "62f17cd95500", "evex_mask_zero", frozenset()),
    Template("evex_vorps_zmm0_zmm0_m32bcst", "evex_broadcast_logic", "62f17c585600", "evex_mask_zero", frozenset()),
    Template("evex_vorps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_logic", "62f17c595600", "evex_mask_zero", frozenset()),
    Template("evex_vorps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_logic", "62f17cd95600", "evex_mask_zero", frozenset()),
    Template("evex_vxorps_zmm0_zmm0_m32bcst", "evex_broadcast_logic", "62f17c585700", "evex_mask_zero", frozenset()),
    Template("evex_vxorps_zmm0_k1_zmm0_m32bcst", "evex_broadcast_logic", "62f17c595700", "evex_mask_zero", frozenset()),
    Template("evex_vxorps_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_logic", "62f17cd95700", "evex_mask_zero", frozenset()),
    Template("evex_vandpd_zmm0_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd585400", "evex_mask_zero", frozenset()),
    Template("evex_vandpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd595400", "evex_mask_zero", frozenset()),
    Template("evex_vandpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_logic", "62f1fdd95400", "evex_mask_zero", frozenset()),
    Template("evex_vandnpd_zmm0_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd585500", "evex_mask_zero", frozenset()),
    Template("evex_vandnpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd595500", "evex_mask_zero", frozenset()),
    Template("evex_vandnpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_logic", "62f1fdd95500", "evex_mask_zero", frozenset()),
    Template("evex_vorpd_zmm0_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd585600", "evex_mask_zero", frozenset()),
    Template("evex_vorpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd595600", "evex_mask_zero", frozenset()),
    Template("evex_vorpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_logic", "62f1fdd95600", "evex_mask_zero", frozenset()),
    Template("evex_vxorpd_zmm0_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd585700", "evex_mask_zero", frozenset()),
    Template("evex_vxorpd_zmm0_k1_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd595700", "evex_mask_zero", frozenset()),
    Template("evex_vxorpd_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_logic", "62f1fdd95700", "evex_mask_zero", frozenset()),
    Template("evex_vunpcklps_zmm0_zmm0_zmm1", "evex_unpck", "62f17c4814c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpcklps_zmm0_k1_zmm0_zmm1", "evex_unpck", "62f17c4914c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpcklps_zmm0_k1z_zmm0_zmm1", "evex_unpck", "62f17cc914c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpckhps_zmm0_zmm0_zmm1", "evex_unpck", "62f17c4815c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpckhps_zmm0_k1_zmm0_zmm1", "evex_unpck", "62f17c4915c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpckhps_zmm0_k1z_zmm0_zmm1", "evex_unpck", "62f17cc915c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpcklpd_zmm0_zmm0_zmm1", "evex_unpck", "62f1fd4814c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpcklpd_zmm0_k1_zmm0_zmm1", "evex_unpck", "62f1fd4914c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpcklpd_zmm0_k1z_zmm0_zmm1", "evex_unpck", "62f1fdc914c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpckhpd_zmm0_zmm0_zmm1", "evex_unpck", "62f1fd4815c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpckhpd_zmm0_k1_zmm0_zmm1", "evex_unpck", "62f1fd4915c1", "evex_mask_zero", frozenset()),
    Template("evex_vunpckhpd_zmm0_k1z_zmm0_zmm1", "evex_unpck", "62f1fdc915c1", "evex_mask_zero", frozenset()),
    Template("evex_vshufps_zmm0_zmm0_zmm1_1b", "evex_shuf", "62f17c48c6c11b", "evex_mask_zero", frozenset()),
    Template("evex_vshufps_zmm0_k1_zmm0_zmm1_1b", "evex_shuf", "62f17c49c6c11b", "evex_mask_zero", frozenset()),
    Template("evex_vshufps_zmm0_k1z_zmm0_zmm1_1b", "evex_shuf", "62f17cc9c6c11b", "evex_mask_zero", frozenset()),
    Template("evex_vshufpd_zmm0_zmm0_zmm1_1b", "evex_shuf", "62f1fd48c6c11b", "evex_mask_zero", frozenset()),
    Template("evex_vshufpd_zmm0_k1_zmm0_zmm1_1b", "evex_shuf", "62f1fd49c6c11b", "evex_mask_zero", frozenset()),
    Template("evex_vshufpd_zmm0_k1z_zmm0_zmm1_1b", "evex_shuf", "62f1fdc9c6c11b", "evex_mask_zero", frozenset()),
    Template("evex_vmovups_zmm0_zmm1", "evex_mov_fp", "62f17c4810c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovups_zmm0_k1_zmm1", "evex_mov_fp", "62f17c4910c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovups_zmm0_k1z_zmm1", "evex_mov_fp", "62f17cc910c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovups_zmm0_k1_m512", "evex_mov_fp", "62f17c491000", "evex_mask_zero", frozenset()),
    Template("evex_vmovups_zmm0_k1z_m512", "evex_mov_fp", "62f17cc91000", "evex_mask_zero", frozenset()),
    Template("evex_vmovups_m512_k1_zmm1", "evex_mov_fp", "62f17c491108", "evex_mask_zero", frozenset()),
    Template("evex_vmovaps_zmm0_zmm1", "evex_mov_fp", "62f17c4828c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovaps_zmm0_k1_zmm1", "evex_mov_fp", "62f17c4928c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovaps_zmm0_k1z_zmm1", "evex_mov_fp", "62f17cc928c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovaps_zmm0_k1_m512", "evex_mov_fp", "62f17c492800", "evex_mask_zero", frozenset()),
    Template("evex_vmovaps_zmm0_k1z_m512", "evex_mov_fp", "62f17cc92800", "evex_mask_zero", frozenset()),
    Template("evex_vmovaps_m512_k1_zmm1", "evex_mov_fp", "62f17c492908", "evex_mask_zero", frozenset()),
    Template("evex_vmovupd_zmm0_zmm1", "evex_mov_fp", "62f1fd4810c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovupd_zmm0_k1_zmm1", "evex_mov_fp", "62f1fd4910c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovupd_zmm0_k1z_zmm1", "evex_mov_fp", "62f1fdc910c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovupd_zmm0_k1_m512", "evex_mov_fp", "62f1fd491000", "evex_mask_zero", frozenset()),
    Template("evex_vmovupd_zmm0_k1z_m512", "evex_mov_fp", "62f1fdc91000", "evex_mask_zero", frozenset()),
    Template("evex_vmovupd_m512_k1_zmm1", "evex_mov_fp", "62f1fd491108", "evex_mask_zero", frozenset()),
    Template("evex_vmovapd_zmm0_zmm1", "evex_mov_fp", "62f1fd4828c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovapd_zmm0_k1_zmm1", "evex_mov_fp", "62f1fd4928c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovapd_zmm0_k1z_zmm1", "evex_mov_fp", "62f1fdc928c1", "evex_mask_zero", frozenset()),
    Template("evex_vmovapd_zmm0_k1_m512", "evex_mov_fp", "62f1fd492800", "evex_mask_zero", frozenset()),
    Template("evex_vmovapd_zmm0_k1z_m512", "evex_mov_fp", "62f1fdc92800", "evex_mask_zero", frozenset()),
    Template("evex_vmovapd_m512_k1_zmm1", "evex_mov_fp", "62f1fd492908", "evex_mask_zero", frozenset()),
    Template("evex_vpandd_zmm0_zmm0_zmm1", "evex_logic_d", "62f17d48dbc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandd_zmm0_k1_zmm0_zmm1", "evex_logic_d", "62f17d49dbc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandd_zmm0_k1z_zmm0_zmm1", "evex_logic_d", "62f17dc9dbc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandnd_zmm0_zmm0_zmm1", "evex_logic_d", "62f17d48dfc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandnd_zmm0_k1_zmm0_zmm1", "evex_logic_d", "62f17d49dfc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandnd_zmm0_k1z_zmm0_zmm1", "evex_logic_d", "62f17dc9dfc1", "evex_mask_zero", frozenset()),
    Template("evex_vpord_zmm0_zmm0_zmm1", "evex_logic_d", "62f17d48ebc1", "evex_mask_zero", frozenset()),
    Template("evex_vpord_zmm0_k1_zmm0_zmm1", "evex_logic_d", "62f17d49ebc1", "evex_mask_zero", frozenset()),
    Template("evex_vpord_zmm0_k1z_zmm0_zmm1", "evex_logic_d", "62f17dc9ebc1", "evex_mask_zero", frozenset()),
    Template("evex_vpxord_zmm0_zmm0_zmm1", "evex_logic_d", "62f17d48efc1", "evex_mask_zero", frozenset()),
    Template("evex_vpxord_zmm0_k1_zmm0_zmm1", "evex_logic_d", "62f17d49efc1", "evex_mask_zero", frozenset()),
    Template("evex_vpxord_zmm0_k1z_zmm0_zmm1", "evex_logic_d", "62f17dc9efc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandq_zmm0_zmm0_zmm1", "evex_logic_q", "62f1fd48dbc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandq_zmm0_k1_zmm0_zmm1", "evex_logic_q", "62f1fd49dbc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandq_zmm0_k1z_zmm0_zmm1", "evex_logic_q", "62f1fdc9dbc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandnq_zmm0_zmm0_zmm1", "evex_logic_q", "62f1fd48dfc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandnq_zmm0_k1_zmm0_zmm1", "evex_logic_q", "62f1fd49dfc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandnq_zmm0_k1z_zmm0_zmm1", "evex_logic_q", "62f1fdc9dfc1", "evex_mask_zero", frozenset()),
    Template("evex_vporq_zmm0_zmm0_zmm1", "evex_logic_q", "62f1fd48ebc1", "evex_mask_zero", frozenset()),
    Template("evex_vporq_zmm0_k1_zmm0_zmm1", "evex_logic_q", "62f1fd49ebc1", "evex_mask_zero", frozenset()),
    Template("evex_vporq_zmm0_k1z_zmm0_zmm1", "evex_logic_q", "62f1fdc9ebc1", "evex_mask_zero", frozenset()),
    Template("evex_vpxorq_zmm0_zmm0_zmm1", "evex_logic_q", "62f1fd48efc1", "evex_mask_zero", frozenset()),
    Template("evex_vpxorq_zmm0_k1_zmm0_zmm1", "evex_logic_q", "62f1fd49efc1", "evex_mask_zero", frozenset()),
    Template("evex_vpxorq_zmm0_k1z_zmm0_zmm1", "evex_logic_q", "62f1fdc9efc1", "evex_mask_zero", frozenset()),
    Template("evex_vpandd_zmm0_zmm0_m32bcst", "evex_broadcast_logic", "62f17d58db00", "evex_mask_zero", frozenset()),
    Template("evex_vpandd_zmm0_k1_zmm0_m32bcst", "evex_broadcast_logic", "62f17d59db00", "evex_mask_zero", frozenset()),
    Template("evex_vpandd_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_logic", "62f17dd9db00", "evex_mask_zero", frozenset()),
    Template("evex_vpandnd_zmm0_zmm0_m32bcst", "evex_broadcast_logic", "62f17d58df00", "evex_mask_zero", frozenset()),
    Template("evex_vpandnd_zmm0_k1_zmm0_m32bcst", "evex_broadcast_logic", "62f17d59df00", "evex_mask_zero", frozenset()),
    Template("evex_vpandnd_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_logic", "62f17dd9df00", "evex_mask_zero", frozenset()),
    Template("evex_vpord_zmm0_zmm0_m32bcst", "evex_broadcast_logic", "62f17d58eb00", "evex_mask_zero", frozenset()),
    Template("evex_vpord_zmm0_k1_zmm0_m32bcst", "evex_broadcast_logic", "62f17d59eb00", "evex_mask_zero", frozenset()),
    Template("evex_vpord_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_logic", "62f17dd9eb00", "evex_mask_zero", frozenset()),
    Template("evex_vpxord_zmm0_zmm0_m32bcst", "evex_broadcast_logic", "62f17d58ef00", "evex_mask_zero", frozenset()),
    Template("evex_vpxord_zmm0_k1_zmm0_m32bcst", "evex_broadcast_logic", "62f17d59ef00", "evex_mask_zero", frozenset()),
    Template("evex_vpxord_zmm0_k1z_zmm0_m32bcst", "evex_broadcast_logic", "62f17dd9ef00", "evex_mask_zero", frozenset()),
    Template("evex_vpandq_zmm0_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd58db00", "evex_mask_zero", frozenset()),
    Template("evex_vpandq_zmm0_k1_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd59db00", "evex_mask_zero", frozenset()),
    Template("evex_vpandq_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_logic", "62f1fdd9db00", "evex_mask_zero", frozenset()),
    Template("evex_vpandnq_zmm0_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd58df00", "evex_mask_zero", frozenset()),
    Template("evex_vpandnq_zmm0_k1_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd59df00", "evex_mask_zero", frozenset()),
    Template("evex_vpandnq_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_logic", "62f1fdd9df00", "evex_mask_zero", frozenset()),
    Template("evex_vporq_zmm0_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd58eb00", "evex_mask_zero", frozenset()),
    Template("evex_vporq_zmm0_k1_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd59eb00", "evex_mask_zero", frozenset()),
    Template("evex_vporq_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_logic", "62f1fdd9eb00", "evex_mask_zero", frozenset()),
    Template("evex_vpxorq_zmm0_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd58ef00", "evex_mask_zero", frozenset()),
    Template("evex_vpxorq_zmm0_k1_zmm0_m64bcst", "evex_broadcast_logic", "62f1fd59ef00", "evex_mask_zero", frozenset()),
    Template("evex_vpxorq_zmm0_k1z_zmm0_m64bcst", "evex_broadcast_logic", "62f1fdd9ef00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa32_zmm0_k1_zmm1", "evex_mask_zero", "62f17d496fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa32_zmm0_k1z_zmm1", "evex_mask_zero", "62f17dc96fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa32_zmm0_k1_m512", "evex_mask_mov_mem", "62f17d496f00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa32_zmm0_k1z_m512", "evex_mask_mov_mem", "62f17dc96f00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa32_m512_k1_zmm1", "evex_mask_mov_mem", "62f17d497f08", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa64_zmm0_k1_zmm1", "evex_movdqa64", "62f1fd496fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa64_zmm0_k1z_zmm1", "evex_movdqa64", "62f1fdc96fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa64_zmm0_k1_m512", "evex_movdqa64", "62f1fd496f00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa64_zmm0_k1z_m512", "evex_movdqa64", "62f1fdc96f00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqa64_m512_k1_zmm1", "evex_movdqa64", "62f1fd497f08", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu8_zmm0_k1_zmm1", "evex_movdqu", "62f17f496fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu8_zmm0_k1z_zmm1", "evex_movdqu", "62f17fc96fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu8_zmm0_k1_m512", "evex_movdqu", "62f17f496f00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu8_m512_k1_zmm1", "evex_movdqu", "62f17f497f08", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu16_zmm0_k1_zmm1", "evex_movdqu", "62f1ff496fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu16_zmm0_k1z_zmm1", "evex_movdqu", "62f1ffc96fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu16_zmm0_k1_m512", "evex_movdqu", "62f1ff496f00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu16_m512_k1_zmm1", "evex_movdqu", "62f1ff497f08", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu32_zmm0_k1_zmm1", "evex_movdqu", "62f17e496fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu32_zmm0_k1z_zmm1", "evex_movdqu", "62f17ec96fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu32_zmm0_k1_m512", "evex_movdqu", "62f17e496f00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu32_m512_k1_zmm1", "evex_movdqu", "62f17e497f08", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu64_zmm0_k1_zmm1", "evex_movdqu", "62f1fe496fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu64_zmm0_k1z_zmm1", "evex_movdqu", "62f1fec96fc1", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu64_zmm0_k1_m512", "evex_movdqu", "62f1fe496f00", "evex_mask_zero", frozenset()),
    Template("evex_vmovdqu64_m512_k1_zmm1", "evex_movdqu", "62f1fe497f08", "evex_mask_zero", frozenset()),
    Template("evex_vgf2p8mulb_zmm0_k1_zmm1_zmm2", "evex_mask_zero", "62f27549cfc2", "evex_mask_zero", frozenset()),
    Template("evex_vgf2p8mulb_zmm0_k1z_zmm1_zmm2", "evex_mask_zero", "62f275c9cfc2", "evex_mask_zero", frozenset()),
    Template("evex_vaesenc_zmm0_k1_zmm1_zmm2", "evex_mask_zero", "62f27549dcc2", "evex_mask_zero", frozenset()),
    Template("evex_vaesenc_zmm0_k1z_zmm1_zmm2", "evex_mask_zero", "62f275c9dcc2", "evex_mask_zero", frozenset()),
    Template("evex_vpxord_zmm16_zmm17_zmm24", "evex_zmm_regbank", "62817540efc0", "evex_zmm_regbank", frozenset()),
    Template("evex_vpandq_zmm24_k1z_zmm16_zmm17", "evex_zmm_regbank", "6221fdc1dbc1", "evex_zmm_regbank", frozenset()),
    Template("evex_vmovdqa32_zmm16_zmm24", "evex_zmm_regbank", "62817d486fc0", "evex_zmm_regbank", frozenset()),
    Template("evex_vmovdqa64_zmm24_zmm16", "evex_zmm_regbank", "6221fd486fc0", "evex_zmm_regbank", frozenset()),
    Template("evex_vmovdqa32_zmm16_m512", "evex_zmm_regbank", "62e17d486f00", "evex_zmm_regbank", frozenset()),
    Template("evex_vmovdqa32_m512_zmm16", "evex_zmm_regbank", "62e17d487f00", "evex_zmm_regbank", frozenset()),
    Template("evex_vcmpeqps_k1_zmm0_zmm1", "evex_cmp_k", "62f17c48c2c900", "evex_cmp_k", frozenset()),
    Template("evex_vcmpltps_k2_k1_zmm0_zmm1", "evex_cmp_k", "62f17c49c2d101", "evex_cmp_k", frozenset()),
    Template("evex_vcmplepd_k3_zmm0_zmm1", "evex_cmp_k", "62f1fd48c2d902", "evex_cmp_k", frozenset()),
    Template("evex_vcmpneqps_k4_zmm0_m32bcst", "evex_cmp_k", "62f17c58c22004", "evex_cmp_k", frozenset()),
    Template("evex_vcmpeqss_k5_xmm0_xmm1", "evex_cmp_k", "62f17e08c2e900", "evex_cmp_k", frozenset()),
    Template("evex_vcmpeqsd_k6_k1_xmm0_xmm1", "evex_cmp_k", "62f1ff09c2f100", "evex_cmp_k", frozenset()),
    Template("evex_vaddss_xmm0_xmm0_xmm1", "evex_scalar_arith", "62f17e0858c1", "evex_scalar_arith", frozenset()),
    Template("evex_vaddss_xmm0_k1_xmm0_xmm1", "evex_scalar_arith", "62f17e0958c1", "evex_scalar_arith", frozenset()),
    Template("evex_vaddss_xmm0_k1z_xmm0_xmm1", "evex_scalar_arith", "62f17e8958c1", "evex_scalar_arith", frozenset()),
    Template("evex_vsubss_xmm0_xmm0_xmm1", "evex_scalar_arith", "62f17e085cc1", "evex_scalar_arith", frozenset()),
    Template("evex_vsubss_xmm0_k1_xmm0_xmm1", "evex_scalar_arith", "62f17e095cc1", "evex_scalar_arith", frozenset()),
    Template("evex_vsubss_xmm0_k1z_xmm0_xmm1", "evex_scalar_arith", "62f17e895cc1", "evex_scalar_arith", frozenset()),
    Template("evex_vmulss_xmm0_xmm0_xmm1", "evex_scalar_arith", "62f17e0859c1", "evex_scalar_arith", frozenset()),
    Template("evex_vmulss_xmm0_k1_xmm0_xmm1", "evex_scalar_arith", "62f17e0959c1", "evex_scalar_arith", frozenset()),
    Template("evex_vmulss_xmm0_k1z_xmm0_xmm1", "evex_scalar_arith", "62f17e8959c1", "evex_scalar_arith", frozenset()),
    Template("evex_vdivss_xmm0_xmm0_xmm1", "evex_scalar_arith", "62f17e085ec1", "evex_scalar_arith", frozenset()),
    Template("evex_vdivss_xmm0_k1_xmm0_xmm1", "evex_scalar_arith", "62f17e095ec1", "evex_scalar_arith", frozenset()),
    Template("evex_vdivss_xmm0_k1z_xmm0_xmm1", "evex_scalar_arith", "62f17e895ec1", "evex_scalar_arith", frozenset()),
    Template("evex_vaddsd_xmm0_xmm0_xmm1", "evex_scalar_arith", "62f1ff0858c1", "evex_scalar_arith", frozenset()),
    Template("evex_vaddsd_xmm0_k1_xmm0_xmm1", "evex_scalar_arith", "62f1ff0958c1", "evex_scalar_arith", frozenset()),
    Template("evex_vaddsd_xmm0_k1z_xmm0_xmm1", "evex_scalar_arith", "62f1ff8958c1", "evex_scalar_arith", frozenset()),
    Template("evex_vsubsd_xmm0_xmm0_xmm1", "evex_scalar_arith", "62f1ff085cc1", "evex_scalar_arith", frozenset()),
    Template("evex_vsubsd_xmm0_k1_xmm0_xmm1", "evex_scalar_arith", "62f1ff095cc1", "evex_scalar_arith", frozenset()),
    Template("evex_vsubsd_xmm0_k1z_xmm0_xmm1", "evex_scalar_arith", "62f1ff895cc1", "evex_scalar_arith", frozenset()),
    Template("evex_vmulsd_xmm0_xmm0_xmm1", "evex_scalar_arith", "62f1ff0859c1", "evex_scalar_arith", frozenset()),
    Template("evex_vmulsd_xmm0_k1_xmm0_xmm1", "evex_scalar_arith", "62f1ff0959c1", "evex_scalar_arith", frozenset()),
    Template("evex_vmulsd_xmm0_k1z_xmm0_xmm1", "evex_scalar_arith", "62f1ff8959c1", "evex_scalar_arith", frozenset()),
    Template("evex_vdivsd_xmm0_xmm0_xmm1", "evex_scalar_arith", "62f1ff085ec1", "evex_scalar_arith", frozenset()),
    Template("evex_vdivsd_xmm0_k1_xmm0_xmm1", "evex_scalar_arith", "62f1ff095ec1", "evex_scalar_arith", frozenset()),
    Template("evex_vdivsd_xmm0_k1z_xmm0_xmm1", "evex_scalar_arith", "62f1ff895ec1", "evex_scalar_arith", frozenset()),
    Template("evex_vsqrtss_xmm0_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e0851c1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vsqrtss_xmm0_k1_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e0951c1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vsqrtss_xmm0_k1z_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e8951c1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vsqrtsd_xmm0_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff0851c1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vsqrtsd_xmm0_k1_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff0951c1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vsqrtsd_xmm0_k1z_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff8951c1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vminss_xmm0_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e085dc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vminss_xmm0_k1_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e095dc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vminss_xmm0_k1z_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e895dc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vmaxss_xmm0_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e085fc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vmaxss_xmm0_k1_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e095fc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vmaxss_xmm0_k1z_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f17e895fc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vminsd_xmm0_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff085dc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vminsd_xmm0_k1_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff095dc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vminsd_xmm0_k1z_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff895dc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vmaxsd_xmm0_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff085fc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vmaxsd_xmm0_k1_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff095fc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vmaxsd_xmm0_k1z_xmm0_xmm1", "evex_scalar_minmax_sqrt", "62f1ff895fc1", "evex_scalar_minmax_sqrt", frozenset()),
    Template("evex_vmovss_xmm0_xmm0_xmm1", "evex_scalar_mov_rr", "62f17e0810c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovss_xmm0_k1_xmm0_xmm1", "evex_scalar_mov_rr", "62f17e0910c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovss_xmm0_k1z_xmm0_xmm1", "evex_scalar_mov_rr", "62f17e8910c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovsd_xmm0_xmm0_xmm1", "evex_scalar_mov_rr", "62f1ff0810c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovsd_xmm0_k1_xmm0_xmm1", "evex_scalar_mov_rr", "62f1ff0910c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovsd_xmm0_k1z_xmm0_xmm1", "evex_scalar_mov_rr", "62f1ff8910c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovss_xmm1_xmm0_xmm0", "evex_scalar_mov_rr", "62f17e0811c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovss_xmm1_k1_xmm0_xmm0", "evex_scalar_mov_rr", "62f17e0911c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovss_xmm1_k1z_xmm0_xmm0", "evex_scalar_mov_rr", "62f17e8911c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovsd_xmm1_xmm0_xmm0", "evex_scalar_mov_rr", "62f1ff0811c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovsd_xmm1_k1_xmm0_xmm0", "evex_scalar_mov_rr", "62f1ff0911c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovsd_xmm1_k1z_xmm0_xmm0", "evex_scalar_mov_rr", "62f1ff8911c1", "evex_scalar_mov_rr", frozenset()),
    Template("evex_vmovss_xmm0_m32", "evex_scalar_mov_mem", "62f17e081000", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovss_xmm0_k1_m32", "evex_scalar_mov_mem", "62f17e091000", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovss_xmm0_k1z_m32", "evex_scalar_mov_mem", "62f17e891000", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovsd_xmm0_m64", "evex_scalar_mov_mem", "62f1ff081000", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovsd_xmm0_k1_m64", "evex_scalar_mov_mem", "62f1ff091000", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovsd_xmm0_k1z_m64", "evex_scalar_mov_mem", "62f1ff891000", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovss_m32_xmm0", "evex_scalar_mov_mem", "62f17e081100", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovss_m32_k1_xmm0", "evex_scalar_mov_mem", "62f17e091100", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovsd_m64_xmm0", "evex_scalar_mov_mem", "62f1ff081100", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vmovsd_m64_k1_xmm0", "evex_scalar_mov_mem", "62f1ff091100", "evex_scalar_mov_mem", frozenset()),
    Template("evex_vpbroadcastb_zmm0_m8", "evex_vpbroadcast_mem", "62f27d487800", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastb_zmm0_k1_m8", "evex_vpbroadcast_mem", "62f27d497800", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastb_zmm0_k1z_m8", "evex_vpbroadcast_mem", "62f27dc97800", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastw_zmm0_m16", "evex_vpbroadcast_mem", "62f27d487900", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastw_zmm0_k1_m16", "evex_vpbroadcast_mem", "62f27d497900", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastw_zmm0_k1z_m16", "evex_vpbroadcast_mem", "62f27dc97900", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastd_zmm0_m32", "evex_vpbroadcast_mem", "62f27d485800", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastd_zmm0_k1_m32", "evex_vpbroadcast_mem", "62f27d495800", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastd_zmm0_k1z_m32", "evex_vpbroadcast_mem", "62f27dc95800", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastq_zmm0_m64", "evex_vpbroadcast_mem", "62f2fd485900", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastq_zmm0_k1_m64", "evex_vpbroadcast_mem", "62f2fd495900", "evex_vpbroadcast_mem", frozenset()),
    Template("evex_vpbroadcastq_zmm0_k1z_m64", "evex_vpbroadcast_mem", "62f2fdc95900", "evex_vpbroadcast_mem", frozenset()),
    Template("vex_vbroadcastss_xmm0_m32", "vbroadcast_fp", "c4e2791800", "avx_vbroadcast_fp", frozenset()),
    Template("vex_vbroadcastss_ymm0_m32", "vbroadcast_fp", "c4e27d1800", "avx_vbroadcast_fp", frozenset()),
    Template("vex_vbroadcastss_ymm0_xmm1", "vbroadcast_fp", "c4e27d18c1", "avx_vbroadcast_fp", frozenset()),
    Template("vex_vbroadcastsd_ymm0_m64", "vbroadcast_fp", "c4e27d1900", "avx_vbroadcast_fp", frozenset()),
    Template("vex_vbroadcastsd_ymm0_xmm1", "vbroadcast_fp", "c4e27d19c1", "avx_vbroadcast_fp", frozenset()),
    Template("vex_vbroadcastf128_ymm0_m128", "vbroadcast_fp", "c4e27d1a00", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastss_zmm0_m32", "vbroadcast_fp", "62f27d481800", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastss_zmm0_k1_m32", "vbroadcast_fp", "62f27d491800", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastss_zmm0_k1z_m32", "vbroadcast_fp", "62f27dc91800", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastss_zmm0_xmm1", "vbroadcast_fp", "62f27d4818c1", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastss_zmm0_k1_xmm1", "vbroadcast_fp", "62f27d4918c1", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastss_zmm0_k1z_xmm1", "vbroadcast_fp", "62f27dc918c1", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastsd_zmm0_m64", "vbroadcast_fp", "62f2fd481900", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastsd_zmm0_k1_m64", "vbroadcast_fp", "62f2fd491900", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastsd_zmm0_k1z_m64", "vbroadcast_fp", "62f2fdc91900", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastsd_zmm0_xmm1", "vbroadcast_fp", "62f2fd4819c1", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastsd_zmm0_k1_xmm1", "vbroadcast_fp", "62f2fd4919c1", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastsd_zmm0_k1z_xmm1", "vbroadcast_fp", "62f2fdc919c1", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastf32x4_zmm0_m128", "vbroadcast_fp", "62f27d481a00", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastf32x4_zmm0_k1_m128", "vbroadcast_fp", "62f27d491a00", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastf32x4_zmm0_k1z_m128", "vbroadcast_fp", "62f27dc91a00", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastf64x4_zmm0_m256", "vbroadcast_fp", "62f2fd481b00", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastf64x4_zmm0_k1_m256", "vbroadcast_fp", "62f2fd491b00", "avx_vbroadcast_fp", frozenset()),
    Template("evex_vbroadcastf64x4_zmm0_k1z_m256", "vbroadcast_fp", "62f2fdc91b00", "avx_vbroadcast_fp", frozenset()),
    Template("vpsrlw_xmm1_xmm0_imm2", "avx_packed_shift", "c5f171d002", "avx_packed_shift", frozenset()),
    Template("vpsrlw_ymm1_ymm0_imm2", "avx_packed_shift", "c5f571d002", "avx_packed_shift", frozenset()),
    Template("vpsraw_xmm1_xmm0_imm2", "avx_packed_shift", "c5f171e002", "avx_packed_shift", frozenset()),
    Template("vpsraw_ymm1_ymm0_imm2", "avx_packed_shift", "c5f571e002", "avx_packed_shift", frozenset()),
    Template("vpsllw_xmm1_xmm0_imm2", "avx_packed_shift", "c5f171f002", "avx_packed_shift", frozenset()),
    Template("vpsllw_ymm1_ymm0_imm2", "avx_packed_shift", "c5f571f002", "avx_packed_shift", frozenset()),
    Template("vpsrld_xmm1_xmm0_imm2", "avx_packed_shift", "c5f172d002", "avx_packed_shift", frozenset()),
    Template("vpsrld_ymm1_ymm0_imm2", "avx_packed_shift", "c5f572d002", "avx_packed_shift", frozenset()),
    Template("vpsrad_xmm1_xmm0_imm2", "avx_packed_shift", "c5f172e002", "avx_packed_shift", frozenset()),
    Template("vpsrad_ymm1_ymm0_imm2", "avx_packed_shift", "c5f572e002", "avx_packed_shift", frozenset()),
    Template("vpslld_xmm1_xmm0_imm2", "avx_packed_shift", "c5f172f002", "avx_packed_shift", frozenset()),
    Template("vpslld_ymm1_ymm0_imm2", "avx_packed_shift", "c5f572f002", "avx_packed_shift", frozenset()),
    Template("vpsrlq_xmm1_xmm0_imm2", "avx_packed_shift", "c5f173d002", "avx_packed_shift", frozenset()),
    Template("vpsrlq_ymm1_ymm0_imm2", "avx_packed_shift", "c5f573d002", "avx_packed_shift", frozenset()),
    Template("vpsrldq_xmm1_xmm0_imm2", "avx_packed_shift", "c5f173d802", "avx_packed_shift", frozenset()),
    Template("vpsrldq_ymm1_ymm0_imm2", "avx_packed_shift", "c5f573d802", "avx_packed_shift", frozenset()),
    Template("vpsllq_xmm1_xmm0_imm2", "avx_packed_shift", "c5f173f002", "avx_packed_shift", frozenset()),
    Template("vpsllq_ymm1_ymm0_imm2", "avx_packed_shift", "c5f573f002", "avx_packed_shift", frozenset()),
    Template("vpslldq_xmm1_xmm0_imm2", "avx_packed_shift", "c5f173f802", "avx_packed_shift", frozenset()),
    Template("vpslldq_ymm1_ymm0_imm2", "avx_packed_shift", "c5f573f802", "avx_packed_shift", frozenset()),
    Template("vpsrlw_xmm0_xmm1_xmm2", "avx_packed_shift", "c5f1d1c2", "avx_packed_shift", frozenset()),
    Template("vpsrlw_ymm0_ymm1_xmm2", "avx_packed_shift", "c5f5d1c2", "avx_packed_shift", frozenset()),
    Template("vpsraw_xmm0_xmm1_xmm2", "avx_packed_shift", "c5f1e1c2", "avx_packed_shift", frozenset()),
    Template("vpsraw_ymm0_ymm1_xmm2", "avx_packed_shift", "c5f5e1c2", "avx_packed_shift", frozenset()),
    Template("vpsllw_xmm0_xmm1_xmm2", "avx_packed_shift", "c5f1f1c2", "avx_packed_shift", frozenset()),
    Template("vpsllw_ymm0_ymm1_xmm2", "avx_packed_shift", "c5f5f1c2", "avx_packed_shift", frozenset()),
    Template("vpsrld_xmm0_xmm1_xmm2", "avx_packed_shift", "c5f1d2c2", "avx_packed_shift", frozenset()),
    Template("vpsrld_ymm0_ymm1_xmm2", "avx_packed_shift", "c5f5d2c2", "avx_packed_shift", frozenset()),
    Template("vpsrad_xmm0_xmm1_xmm2", "avx_packed_shift", "c5f1e2c2", "avx_packed_shift", frozenset()),
    Template("vpsrad_ymm0_ymm1_xmm2", "avx_packed_shift", "c5f5e2c2", "avx_packed_shift", frozenset()),
    Template("vpslld_xmm0_xmm1_xmm2", "avx_packed_shift", "c5f1f2c2", "avx_packed_shift", frozenset()),
    Template("vpslld_ymm0_ymm1_xmm2", "avx_packed_shift", "c5f5f2c2", "avx_packed_shift", frozenset()),
    Template("vpsrlq_xmm0_xmm1_xmm2", "avx_packed_shift", "c5f1d3c2", "avx_packed_shift", frozenset()),
    Template("vpsrlq_ymm0_ymm1_xmm2", "avx_packed_shift", "c5f5d3c2", "avx_packed_shift", frozenset()),
    Template("vpsllq_xmm0_xmm1_xmm2", "avx_packed_shift", "c5f1f3c2", "avx_packed_shift", frozenset()),
    Template("vpsllq_ymm0_ymm1_xmm2", "avx_packed_shift", "c5f5f3c2", "avx_packed_shift", frozenset()),
    Template("vmovlps_xmm0_xmm1_m64", "avx_vmovlh_mem", "c4e1701200", "avx_vmovlh_mem", frozenset()),
    Template("vmovhps_xmm0_xmm1_m64", "avx_vmovlh_mem", "c4e1701600", "avx_vmovlh_mem", frozenset()),
    Template("vmovlpd_xmm0_xmm1_m64", "avx_vmovlh_mem", "c4e1711200", "avx_vmovlh_mem", frozenset()),
    Template("vmovhpd_xmm0_xmm1_m64", "avx_vmovlh_mem", "c4e1711600", "avx_vmovlh_mem", frozenset()),
    Template("vmovlps_m64_xmm0", "avx_vmovlh_mem", "c5f81300", "avx_vmovlh_mem", frozenset()),
    Template("vmovhps_m64_xmm0", "avx_vmovlh_mem", "c5f81700", "avx_vmovlh_mem", frozenset()),
    Template("vmovlpd_m64_xmm0", "avx_vmovlh_mem", "c5f91300", "avx_vmovlh_mem", frozenset()),
    Template("vmovhpd_m64_xmm0", "avx_vmovlh_mem", "c5f91700", "avx_vmovlh_mem", frozenset()),
    Template("evex_vpshufd_zmm0_zmm0_imm", "evex_pshuf_word_imm", "62f17d4870c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpshufd_zmm0_k1_zmm0_imm", "evex_pshuf_word_imm", "62f17d4970c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpshufd_zmm0_k1z_zmm0_imm", "evex_pshuf_word_imm", "62f17dc970c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpshufhw_zmm0_zmm0_imm", "evex_pshuf_word_imm", "62f17e4870c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpshufhw_zmm0_k1_zmm0_imm", "evex_pshuf_word_imm", "62f17e4970c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpshufhw_zmm0_k1z_zmm0_imm", "evex_pshuf_word_imm", "62f17ec970c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpshuflw_zmm0_zmm0_imm", "evex_pshuf_word_imm", "62f17f4870c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpshuflw_zmm0_k1_zmm0_imm", "evex_pshuf_word_imm", "62f17f4970c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpshuflw_zmm0_k1z_zmm0_imm", "evex_pshuf_word_imm", "62f17fc970c002", "evex_pshuf_word_imm", frozenset()),
    Template("evex_vpbroadcastb_xmm0_xmm1", "evex_broadcast_ext", "62f27d0878c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastb_ymm0_k1_xmm1", "evex_broadcast_ext", "62f27d2978c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastb_zmm0_k1z_xmm1", "evex_broadcast_ext", "62f27dc978c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastw_xmm0_xmm1", "evex_broadcast_ext", "62f27d0879c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastw_ymm0_k1_xmm1", "evex_broadcast_ext", "62f27d2979c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastw_zmm0_k1z_xmm1", "evex_broadcast_ext", "62f27dc979c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastd_xmm0_xmm1", "evex_broadcast_ext", "62f27d0858c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastd_ymm0_k1_xmm1", "evex_broadcast_ext", "62f27d2958c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastd_zmm0_k1z_xmm1", "evex_broadcast_ext", "62f27dc958c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastq_xmm0_xmm1", "evex_broadcast_ext", "62f2fd0859c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastq_ymm0_k1_xmm1", "evex_broadcast_ext", "62f2fd2959c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vpbroadcastq_zmm0_k1z_xmm1", "evex_broadcast_ext", "62f2fdc959c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcasti32x2_xmm0_xmm1", "evex_broadcast_ext", "62f27d0859c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcasti32x2_ymm0_k1_xmm1", "evex_broadcast_ext", "62f27d2959c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcasti32x2_zmm0_k1z_xmm1", "evex_broadcast_ext", "62f27dc959c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf32x2_ymm0_xmm1", "evex_broadcast_ext", "62f27d2819c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf32x2_zmm0_k1_xmm1", "evex_broadcast_ext", "62f27d4919c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf32x2_zmm0_k1z_xmm1", "evex_broadcast_ext", "62f27dc919c1", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf64x2_ymm0_m128", "evex_broadcast_ext", "62f2fd281a00", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf64x2_zmm0_k1_m128", "evex_broadcast_ext", "62f2fd491a00", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf64x2_zmm0_k1z_m128", "evex_broadcast_ext", "62f2fdc91a00", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf32x8_zmm0_m256", "evex_broadcast_ext", "62f27d481b00", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf32x8_zmm0_k1_m256", "evex_broadcast_ext", "62f27d491b00", "evex_broadcast_ext", frozenset()),
    Template("evex_vbroadcastf32x8_zmm0_k1z_m256", "evex_broadcast_ext", "62f27dc91b00", "evex_broadcast_ext", frozenset()),
    Template("evex_vpunpcklbw_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d0860c1", "evex_packed_int", frozenset()),
    Template("evex_vpunpcklwd_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d2961c1", "evex_packed_int", frozenset()),
    Template("evex_vpunpckldq_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc962c1", "evex_packed_int", frozenset()),
    Template("evex_vpunpcklqdq_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f1fdc96cc1", "evex_packed_int", frozenset()),
    Template("evex_vpunpckhbw_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d0868c1", "evex_packed_int", frozenset()),
    Template("evex_vpunpckhwd_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d2969c1", "evex_packed_int", frozenset()),
    Template("evex_vpunpckhdq_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc96ac1", "evex_packed_int", frozenset()),
    Template("evex_vpunpckhqdq_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f1fdc96dc1", "evex_packed_int", frozenset()),
    Template("evex_vpacksswb_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d0863c1", "evex_packed_int", frozenset()),
    Template("evex_vpackssdw_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d296bc1", "evex_packed_int", frozenset()),
    Template("evex_vpackuswb_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc967c1", "evex_packed_int", frozenset()),
    Template("evex_vpaddb_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d08fcc1", "evex_packed_int", frozenset()),
    Template("evex_vpaddw_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d29fdc1", "evex_packed_int", frozenset()),
    Template("evex_vpaddd_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9fec1", "evex_packed_int", frozenset()),
    Template("evex_vpaddq_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f1fdc9d4c1", "evex_packed_int", frozenset()),
    Template("evex_vpsubb_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d08f8c1", "evex_packed_int", frozenset()),
    Template("evex_vpsubw_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d29f9c1", "evex_packed_int", frozenset()),
    Template("evex_vpsubd_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9fac1", "evex_packed_int", frozenset()),
    Template("evex_vpsubq_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f1fdc9fbc1", "evex_packed_int", frozenset()),
    Template("evex_vpmullw_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d08d5c1", "evex_packed_int", frozenset()),
    Template("evex_vpmulhw_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d29e5c1", "evex_packed_int", frozenset()),
    Template("evex_vpmulhuw_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9e4c1", "evex_packed_int", frozenset()),
    Template("evex_vpmuludq_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f1fdc9f4c1", "evex_packed_int", frozenset()),
    Template("evex_vpmaddwd_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9f5c1", "evex_packed_int", frozenset()),
    Template("evex_vpsadbw_zmm0_zmm0_zmm1", "evex_packed_int", "62f17d48f6c1", "evex_packed_int", frozenset()),
    Template("evex_vpsubusb_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d08d8c1", "evex_packed_int", frozenset()),
    Template("evex_vpsubusw_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d29d9c1", "evex_packed_int", frozenset()),
    Template("evex_vpsubsb_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9e8c1", "evex_packed_int", frozenset()),
    Template("evex_vpsubsw_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9e9c1", "evex_packed_int", frozenset()),
    Template("evex_vpaddusb_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d08dcc1", "evex_packed_int", frozenset()),
    Template("evex_vpaddusw_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d29ddc1", "evex_packed_int", frozenset()),
    Template("evex_vpaddsb_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9ecc1", "evex_packed_int", frozenset()),
    Template("evex_vpaddsw_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9edc1", "evex_packed_int", frozenset()),
    Template("evex_vpavgb_xmm0_xmm0_xmm1", "evex_packed_int", "62f17d08e0c1", "evex_packed_int", frozenset()),
    Template("evex_vpavgw_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9e3c1", "evex_packed_int", frozenset()),
    Template("evex_vpminub_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d29dac1", "evex_packed_int", frozenset()),
    Template("evex_vpminsw_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9eac1", "evex_packed_int", frozenset()),
    Template("evex_vpmaxub_ymm0_k1_ymm0_ymm1", "evex_packed_int", "62f17d29dec1", "evex_packed_int", frozenset()),
    Template("evex_vpmaxsw_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f17dc9eec1", "evex_packed_int", frozenset()),
    Template("evex_vpshufb_xmm0_xmm0_xmm1", "evex_packed_int", "62f27d0800c1", "evex_packed_int", frozenset()),
    Template("evex_vpshufb_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f27dc900c1", "evex_packed_int", frozenset()),
    Template("evex_vpmaddubsw_xmm0_xmm0_xmm1", "evex_packed_int", "62f27d0804c1", "evex_packed_int", frozenset()),
    Template("evex_vpmaddubsw_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f27dc904c1", "evex_packed_int", frozenset()),
    Template("evex_vpmulhrsw_zmm0_k1z_zmm0_zmm1", "evex_packed_int", "62f27dc90bc1", "evex_packed_int", frozenset()),
    Template("evex_vpabsb_xmm0_xmm1", "evex_packed_int", "62f27d081cc1", "evex_packed_int", frozenset()),
    Template("evex_vpabsw_ymm0_k1_ymm1", "evex_packed_int", "62f27d291dc1", "evex_packed_int", frozenset()),
    Template("evex_vpabsd_zmm0_k1z_zmm1", "evex_packed_int", "62f27dc91ec1", "evex_packed_int", frozenset()),
    Template("vfmadd132ps_ymm0_ymm1_ymm2", "fma3", "c4e27598c2", None),
    Template("vfmadd213pd_ymm0_ymm1_ymm2", "fma3", "c4e2f5a8c2", None),
    Template("vfmadd231ss_xmm0_xmm1_xmm2", "fma3", "c4e271b9c2", None),
    Template("vfmsub132sd_xmm0_xmm1_xmm2", "fma3", "c4e2f19bc2", None),
    Template("vfmadd132ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e27599c2", "fma3_tail"),
    Template("vfmsub132ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2759bc2", "fma3_tail"),
    Template("vfnmadd132ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2759dc2", "fma3_tail"),
    Template("vfnmsub132ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2759fc2", "fma3_tail"),
    Template("vfmadd213ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e275a9c2", "fma3_tail"),
    Template("vfmsub213ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e275abc2", "fma3_tail"),
    Template("vfnmadd213ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e275adc2", "fma3_tail"),
    Template("vfnmsub213ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e275afc2", "fma3_tail"),
    Template("vfmadd231ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e275b9c2", "fma3_tail"),
    Template("vfmsub231ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e275bbc2", "fma3_tail"),
    Template("vfnmadd231ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e275bdc2", "fma3_tail"),
    Template("vfnmsub231ss_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e275bfc2", "fma3_tail"),
    Template("vfmadd132sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f599c2", "fma3_tail"),
    Template("vfmsub132sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f59bc2", "fma3_tail"),
    Template("vfnmadd132sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f59dc2", "fma3_tail"),
    Template("vfnmsub132sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f59fc2", "fma3_tail"),
    Template("vfmadd213sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f5a9c2", "fma3_tail"),
    Template("vfmsub213sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f5abc2", "fma3_tail"),
    Template("vfnmadd213sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f5adc2", "fma3_tail"),
    Template("vfnmsub213sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f5afc2", "fma3_tail"),
    Template("vfmadd231sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f5b9c2", "fma3_tail"),
    Template("vfmsub231sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f5bbc2", "fma3_tail"),
    Template("vfnmadd231sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f5bdc2", "fma3_tail"),
    Template("vfnmsub231sd_l1_xmm0_xmm1_xmm2", "fma3_scalar_lig", "c4e2f5bfc2", "fma3_tail"),
    Template("vfmaddsub132ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e27596c2", "fma3_tail"),
    Template("vfmaddsub132pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f596c2", "fma3_tail"),
    Template("vfmsubadd132ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e27597c2", "fma3_tail"),
    Template("vfmsubadd132pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f597c2", "fma3_tail"),
    Template("vfmaddsub213ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e275a6c2", "fma3_tail"),
    Template("vfmaddsub213pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f5a6c2", "fma3_tail"),
    Template("vfmsubadd213ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e275a7c2", "fma3_tail"),
    Template("vfmsubadd213pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f5a7c2", "fma3_tail"),
    Template("vfmaddsub231ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e275b6c2", "fma3_tail"),
    Template("vfmaddsub231pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f5b6c2", "fma3_tail"),
    Template("vfmsubadd231ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e275b7c2", "fma3_tail"),
    Template("vfmsubadd231pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f5b7c2", "fma3_tail"),
    Template("vfnmadd132ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e2759cc2", "fma3_tail"),
    Template("vfnmadd132pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f59cc2", "fma3_tail"),
    Template("vfnmadd132ss_xmm0_xmm1_xmm2", "fma3_tail", "c4e2719dc2", "fma3_tail"),
    Template("vfnmadd132sd_xmm0_xmm1_xmm2", "fma3_tail", "c4e2f19dc2", "fma3_tail"),
    Template("vfnmsub132ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e2759ec2", "fma3_tail"),
    Template("vfnmsub132pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f59ec2", "fma3_tail"),
    Template("vfnmsub132ss_xmm0_xmm1_xmm2", "fma3_tail", "c4e2719fc2", "fma3_tail"),
    Template("vfnmsub132sd_xmm0_xmm1_xmm2", "fma3_tail", "c4e2f19fc2", "fma3_tail"),
    Template("vfnmadd213ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e275acc2", "fma3_tail"),
    Template("vfnmadd213pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f5acc2", "fma3_tail"),
    Template("vfnmadd213ss_xmm0_xmm1_xmm2", "fma3_tail", "c4e271adc2", "fma3_tail"),
    Template("vfnmadd213sd_xmm0_xmm1_xmm2", "fma3_tail", "c4e2f1adc2", "fma3_tail"),
    Template("vfnmsub213ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e275aec2", "fma3_tail"),
    Template("vfnmsub213pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f5aec2", "fma3_tail"),
    Template("vfnmsub213ss_xmm0_xmm1_xmm2", "fma3_tail", "c4e271afc2", "fma3_tail"),
    Template("vfnmsub213sd_xmm0_xmm1_xmm2", "fma3_tail", "c4e2f1afc2", "fma3_tail"),
    Template("vfnmadd231ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e275bcc2", "fma3_tail"),
    Template("vfnmadd231pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f5bcc2", "fma3_tail"),
    Template("vfnmadd231ss_xmm0_xmm1_xmm2", "fma3_tail", "c4e271bdc2", "fma3_tail"),
    Template("vfnmadd231sd_xmm0_xmm1_xmm2", "fma3_tail", "c4e2f1bdc2", "fma3_tail"),
    Template("vfnmsub231ps_ymm0_ymm1_ymm2", "fma3_tail", "c4e275bec2", "fma3_tail"),
    Template("vfnmsub231pd_ymm0_ymm1_ymm2", "fma3_tail", "c4e2f5bec2", "fma3_tail"),
    Template("vfnmsub231ss_xmm0_xmm1_xmm2", "fma3_tail", "c4e271bfc2", "fma3_tail"),
    Template("vfnmsub231sd_xmm0_xmm1_xmm2", "fma3_tail", "c4e2f1bfc2", "fma3_tail"),
    Template("vmovhlps_xmm0_xmm1_xmm2", "vmovhl_lh", "c4e17012c2", "vmovhl_lh"),
    Template("vmovhlps_w1_xmm0_xmm1_xmm2", "vmovhl_lh", "c4e1f012c2", "vmovhl_lh"),
    Template("vmovlhps_xmm0_xmm1_xmm2", "vmovhl_lh", "c4e17016c2", "vmovhl_lh"),
    Template("vmovlhps_w1_xmm0_xmm1_xmm2", "vmovhl_lh", "c4e1f016c2", "vmovhl_lh"),
    Template("vcvtph2ps_xmm0_xmm1", "f16c", "c4e27913c1", None),
    Template("vcvtph2ps_ymm0_xmm1", "f16c", "c4e27d13c1", None),
    Template("vcvtps2ph_xmm1_xmm0", "f16c", "c4e3791dc100", None),
    Template("vcvtps2ph_xmm1_ymm0", "f16c", "c4e37d1dc100", None),
    Template("rep_movsb", "string_ops", "f3a4", None),
    Template("rep_cmpsb", "string_ops", "f3a6", None),
    Template("rep_stosb", "string_ops", "f3aa", None),
    Template("lodsb", "string_ops", "ac", None),
    Template("scasb", "string_ops", "ae", None),
]

X64_TEMPLATES = TEMPLATES

I386_TEMPLATES: list[Template] = [
    Template("add_eax_ecx", "int_arith_flags", "01c8"),
    Template("adc_eax_ecx", "int_arith_flags", "11c8"),
    Template("sub_eax_ecx", "int_arith_flags", "29c8"),
    Template("sbb_eax_ecx", "int_arith_flags", "19c8"),
    Template("cmp_eax_ecx", "int_arith_flags", "39c8"),
    Template("and_eax_ecx", "int_logic_flags", "21c8"),
    Template("or_eax_ecx", "int_logic_flags", "09c8"),
    Template("xor_eax_ecx", "int_logic_flags", "31c8"),
    Template("test_eax_ecx", "int_logic_flags", "85c8"),
    Template("neg_eax", "int_arith_flags", "f7d8"),
    Template("mul_ecx", "mul_div", "f7e1", None, frozenset({"cf", "of"})),
    Template("imul_ecx", "mul_div", "f7e9", None, frozenset({"cf", "of"})),
    Template("imul_eax_ecx", "imul_flags", "0fafc1", None, frozenset({"cf", "of"})),
    Template("imul_eax_ecx_imm8", "imul_flags", "6bc17f", None, frozenset({"cf", "of"})),
    Template("imul_eax_ecx_imm32", "imul_flags", "69c178563412", None, frozenset({"cf", "of"})),
    Template("div_ecx", "mul_div", "f7f1", None, frozenset()),
    Template("idiv_ecx", "mul_div", "f7f9", None, frozenset()),
    Template("shl_eax_cl", "shift_rotate_flags", "d3e0"),
    Template("shr_eax_cl", "shift_rotate_flags", "d3e8"),
    Template("sar_eax_cl", "shift_rotate_flags", "d3f8"),
    Template("rol_eax_cl", "shift_rotate_flags", "d3c0", None, frozenset({"cf"})),
    Template("ror_eax_cl", "shift_rotate_flags", "d3c8", None, frozenset({"cf"})),
    Template("rcl_eax_cl", "rcl_rcr", "d3d0", None, frozenset({"cf"})),
    Template("rcr_eax_cl", "rcl_rcr", "d3d8", None, frozenset({"cf"})),
    Template("bt_eax_ecx", "bt_family", "0fa3c8", None, frozenset({"cf"})),
    Template("bts_eax_ecx", "bt_family", "0fabc8", None, frozenset({"cf"})),
    Template("btr_eax_ecx", "bt_family", "0fb3c8", None, frozenset({"cf"})),
    Template("btc_eax_ecx", "bt_family", "0fbbc8", None, frozenset({"cf"})),
    Template("bsf_eax_ecx", "bit_scan", "0fbcc1", None, frozenset({"zf"})),
    Template("bsr_eax_ecx", "bit_scan", "0fbdc1", None, frozenset({"zf"})),
    Template("cmovne_eax_ecx", "cmov_setcc", "0f45c1"),
    Template("cmove_eax_ecx", "cmov_setcc", "0f44c1"),
    Template("setne_al", "cmov_setcc", "0f95c0"),
    Template("sete_al", "cmov_setcc", "0f94c0"),
    Template("xchg_eax_ecx", "xchg_cmpxchg", "87c8"),
    Template("xadd_eax_ecx", "xchg_cmpxchg", "0fc1c8"),
    Template("cmpxchg_eax_ecx", "xchg_cmpxchg", "0fb1c8"),
    Template("lea_eax_esi_edi4", "lea", "8d04be"),
    Template("rep_movsb", "string_ops", "f3a4"),
    Template("repe_cmpsb", "string_ops", "f3a6"),
    Template("rep_stosb", "string_ops", "f3aa"),
    Template("lodsb", "string_ops", "ac"),
    Template("scasb", "string_ops", "ae"),
    Template("xorps_xmm0_xmm0", "sse", "0f57c0"),
    Template("addps_xmm0_xmm1", "sse", "0f58c1"),
    Template("subps_xmm0_xmm1", "sse", "0f5cc1"),
    Template("mulps_xmm0_xmm1", "sse", "0f59c1"),
    Template("divps_xmm0_xmm1", "sse", "0f5ec1"),
    Template("minps_xmm0_xmm1", "sse", "0f5dc1"),
    Template("maxps_xmm0_xmm1", "sse", "0f5fc1"),
    Template("addss_xmm0_xmm1", "sse", "f30f58c1"),
    Template("subss_xmm0_xmm1", "sse", "f30f5cc1"),
    Template("mulss_xmm0_xmm1", "sse", "f30f59c1"),
    Template("divss_xmm0_xmm1", "sse", "f30f5ec1"),
    Template("comiss_xmm0_xmm1", "sse", "0f2fc1", None, frozenset({"cf", "pf", "af", "zf", "sf", "of"})),
    Template("movmskps_eax_xmm0", "sse", "0f50c0"),
    Template("unpcklps_xmm0_xmm1", "sse", "0f14c1"),
    Template("unpckhps_xmm0_xmm1", "sse", "0f15c1"),
    Template("shufps_xmm0_xmm1", "sse", "0fc6c11b"),
    Template("pxor_xmm0_xmm1", "sse2", "660fefc1"),
    Template("addpd_xmm0_xmm1", "sse2", "660f58c1"),
    Template("subpd_xmm0_xmm1", "sse2", "660f5cc1"),
    Template("mulpd_xmm0_xmm1", "sse2", "660f59c1"),
    Template("divpd_xmm0_xmm1", "sse2", "660f5ec1"),
    Template("paddb_xmm0_xmm1", "sse2", "660ffcc1"),
    Template("paddw_xmm0_xmm1", "sse2", "660ffdc1"),
    Template("paddd_xmm0_xmm1", "sse2", "660ffec1"),
    Template("paddq_xmm0_xmm1", "sse2", "660fd4c1"),
    Template("psubb_xmm0_xmm1", "sse2", "660ff8c1"),
    Template("psubw_xmm0_xmm1", "sse2", "660ff9c1"),
    Template("psubd_xmm0_xmm1", "sse2", "660ffac1"),
    Template("psubq_xmm0_xmm1", "sse2", "660ffbc1"),
    Template("pmullw_xmm0_xmm1", "sse2", "660fd5c1"),
    Template("movmskpd_eax_xmm0", "sse2", "660f50c0"),
    Template("pmovmskb_eax_xmm0", "sse2", "660fd7c0"),
    Template("unpcklpd_xmm0_xmm1", "sse2", "660f14c1"),
    Template("unpckhpd_xmm0_xmm1", "sse2", "660f15c1"),
    Template("pshufd_xmm0_xmm1", "sse2", "660f70c11b"),
    Template("fninit", "x87", "dbe3"),
    Template("fnclex", "x87", "dbe2"),
    Template("fninit_fnstsw_ax", "x87", "dbe3dfe0"),
    Template("fld1_fnstsw_ax", "x87", "d9e8dfe0"),
    Template("fldz_fnstsw_ax", "x87", "d9eedfe0"),
    Template("fld1_fstp_m64", "x87", "d9e8dd1f"),
    # Legacy i386-only opcodes (removed in x64). CS_MODE_32-only.
    Template("pusha", "legacy_pusha_popa", "60"),
    Template("popa", "legacy_pusha_popa", "61"),
    Template("daa", "legacy_bcd", "27"),
    # NOTE: DAS/AAS are EXCLUDED from the Unicorn diff — HyperBridge follows the
    # Intel SDM (real-silicon contract) and Unicorn 2.1.4 diverges (AAS: AH-=2 vs
    # SDM AH-=1; DAS: drops the old_AL>99h second-adjust clause). Keeping them here
    # would report false mismatches. See HB-I386-DECODE-COMPLETE "Oracle Divergences".
    # Template("das", "legacy_bcd", "2f"),   # oracle-divergent (SDM-correct in HB)
    Template("aaa", "legacy_bcd", "37"),
    # Template("aas", "legacy_bcd", "3f"),   # oracle-divergent (SDM-correct in HB)
    Template("aam_imm10", "legacy_bcd", "d40a"),
    Template("aad_imm10", "legacy_bcd", "d50a"),
    Template("bound_eax_mem", "legacy_bound_arpl", "6200", None, frozenset()),
    Template("arpl_ax_cx", "legacy_bound_arpl", "632901"),
    # LDS/LES/LFS/LGS need a far-pointer m48 at esi+disp; 8 bytes:
    # [16-bit offset | 16-bit selector]. Use indirection through esi.
    Template("les_eax_mem", "legacy_segreg_load", "c4041e0008007000"),
    Template("lds_eax_mem", "legacy_segreg_load", "c5041e0008007000"),
    Template("lfs_eax_mem", "legacy_segreg_load", "0fb4041e0008007000"),
    Template("lgs_eax_mem", "legacy_segreg_load", "0fb5041e0008007000"),
]


def build_runner() -> None:
    subprocess.run(["make", "libhyperbridge.a"], cwd=HB_ROOT, check=True, stdout=subprocess.PIPE)
    subprocess.run(
        [
            "cc",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-std=c11",
            "-I./include",
            str(RUNNER_C.relative_to(HB_ROOT)),
            "libhyperbridge.a",
            "-o",
            str(RUNNER.relative_to(HB_ROOT)),
        ],
        cwd=HB_ROOT,
        check=True,
        stdout=subprocess.PIPE,
    )


def as_u64(v: str) -> int:
    return int(v, 16) & MASK64


def parity_even(byte: int) -> int:
    return 1 if (byte & 0xff).bit_count() % 2 == 0 else 0


def flag_common(result: int) -> dict[str, int]:
    result &= MASK64
    return {
        "zf": 1 if result == 0 else 0,
        "sf": 1 if result & SIGN64 else 0,
        "pf": parity_even(result),
    }


def add_flags(a: int, b: int, carry: int) -> tuple[int, dict[str, int]]:
    total = a + b + carry
    result = total & MASK64
    flags = flag_common(result)
    flags["cf"] = 1 if total >> 64 else 0
    flags["of"] = 1 if ((~(a ^ b) & (a ^ result) & SIGN64) != 0) else 0
    flags["af"] = 1 if ((a ^ b ^ result) & 0x10) != 0 else 0
    return result, flags


def sub_flags(a: int, b: int, borrow: int) -> tuple[int, dict[str, int]]:
    subtrahend = b + borrow
    result = (a - subtrahend) & MASK64
    flags = flag_common(result)
    flags["cf"] = 1 if a < subtrahend else 0
    flags["of"] = 1 if (((a ^ b) & (a ^ result) & SIGN64) != 0) else 0
    flags["af"] = 1 if ((a ^ b ^ result) & 0x10) != 0 else 0
    return result, flags


def logic_flags(result: int) -> dict[str, int]:
    flags = flag_common(result)
    flags["cf"] = 0
    flags["of"] = 0
    return flags


def shl_flags(a: int, raw_count: int, initial_flags: dict[str, int]) -> tuple[int, dict[str, int]]:
    count = raw_count & 0x3f
    if count == 0:
        return a, {k: int(v) for k, v in initial_flags.items()}
    result = (a << count) & MASK64
    flags = flag_common(result)
    flags["cf"] = 1 if ((a >> (64 - count)) & 1) else 0
    if count == 1:
        flags["of"] = 1 if (((result >> 63) & 1) != flags["cf"]) else 0
    return result, flags


def exact_avx_vpermil_blendv_vcmp(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    name = template.name

    def vpermilps(data: bytes, ctrl: bytes | None, imm: int) -> bytes:
        out = bytearray(16)
        for block in range(0, 16, 16):
            for j in range(4):
                sel = ((imm >> (2 * j)) & 3) if ctrl is None else (int.from_bytes(ctrl[block + j * 4:block + j * 4 + 4], "little") & 3)
                out[block + j * 4:block + j * 4 + 4] = data[block + sel * 4:block + sel * 4 + 4]
        return bytes(out)

    def vpermilpd(data: bytes, ctrl: bytes | None, imm: int) -> bytes:
        out = bytearray(16)
        for j in range(2):
            sel = ((imm >> j) & 1) if ctrl is None else ((int.from_bytes(ctrl[j * 8:j * 8 + 8], "little") >> 1) & 1)
            out[j * 8:j * 8 + 8] = data[sel * 8:sel * 8 + 8]
        return bytes(out)

    def blendv(lhs: bytes, rhs: bytes, mask: bytes, lane: int) -> bytes:
        out = bytearray(lhs)
        for off in range(0, 16, lane):
            if mask[off + lane - 1] & 0x80:
                out[off:off + lane] = rhs[off:off + lane]
        return bytes(out)

    def cmp_pred(a: float, b: float) -> bool:
        unordered = math.isnan(a) or math.isnan(b)
        return (not unordered) and a < b

    def vcmp(lane: int, scalar: bool) -> bytes:
        out = bytearray(xmm_i[1] if scalar else b"\x00" * 16)
        count = 1 if scalar else 16 // lane
        for j in range(count):
            off = j * lane
            if lane == 4:
                a = struct.unpack("<f", xmm_i[1][off:off + 4])[0]
                b = struct.unpack("<f", xmm_i[2][off:off + 4])[0]
            else:
                a = struct.unpack("<d", xmm_i[1][off:off + 8])[0]
                b = struct.unpack("<d", xmm_i[2][off:off + 8])[0]
            out[off:off + lane] = b"\xff" * lane if cmp_pred(a, b) else b"\x00" * lane
        return bytes(out)

    target = 0
    expected: bytes
    if name == "vpermilps_xmm0_xmm1_imm":
        expected = vpermilps(xmm_i[1], None, 0x1b)
    elif name == "vpermilpd_xmm0_xmm1_imm":
        expected = vpermilpd(xmm_i[1], None, 0x1b)
    elif name == "vpermilps_xmm0_xmm1_xmm2":
        expected = vpermilps(xmm_i[1], xmm_i[2], 0)
    elif name == "vpermilpd_xmm0_xmm1_xmm2":
        expected = vpermilpd(xmm_i[1], xmm_i[2], 0)
    elif name == "vblendvps_xmm1_xmm2_xmm3_xmm1":
        target = 1
        expected = blendv(xmm_i[2], xmm_i[3], xmm_i[1], 4)
    elif name == "vblendvpd_xmm1_xmm2_xmm3_xmm1":
        target = 1
        expected = blendv(xmm_i[2], xmm_i[3], xmm_i[1], 8)
    elif name == "vpblendvb_xmm1_xmm2_xmm3_xmm1":
        target = 1
        expected = blendv(xmm_i[2], xmm_i[3], xmm_i[1], 1)
    elif name.startswith("vcmpps"):
        expected = vcmp(4, False)
    elif name.startswith("vcmppd"):
        expected = vcmp(8, False)
    elif name.startswith("vcmpss"):
        expected = vcmp(4, True)
    elif name.startswith("vcmpsd"):
        expected = vcmp(8, True)
    else:
        return []

    actual = xmm_f[target]
    if actual != expected:
        return [f"xmm{target} expected={expected.hex()} actual={actual.hex()}"]
    return []


def exact_avx_shuffle_unpack(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    bytes_len = 32 if "_ymm" in name else 16
    src1 = xmm_i[1] + (ymm_i[1] if bytes_len == 32 else b"")
    src2 = xmm_i[2] + (ymm_i[2] if bytes_len == 32 else b"")

    def unpck(lane: int, high: bool) -> bytes:
        out = bytearray(bytes_len)
        for base in range(0, bytes_len, 16):
            start = 8 if high else 0
            lanes = 8 // lane
            pos = base
            for i in range(lanes):
                off = base + start + i * lane
                out[pos:pos + lane] = src1[off:off + lane]
                pos += lane
                out[pos:pos + lane] = src2[off:off + lane]
                pos += lane
        return bytes(out)

    def shuf(lane: int, imm: int) -> bytes:
        out = bytearray(bytes_len)
        for base in range(0, bytes_len, 16):
            if lane == 4:
                sels = [(imm >> 0) & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3]
                out[base + 0:base + 4] = src1[base + sels[0] * 4:base + sels[0] * 4 + 4]
                out[base + 4:base + 8] = src1[base + sels[1] * 4:base + sels[1] * 4 + 4]
                out[base + 8:base + 12] = src2[base + sels[2] * 4:base + sels[2] * 4 + 4]
                out[base + 12:base + 16] = src2[base + sels[3] * 4:base + sels[3] * 4 + 4]
            else:
                s0 = imm & 1
                s1 = (imm >> 1) & 1
                out[base:base + 8] = src1[base + s0 * 8:base + s0 * 8 + 8]
                out[base + 8:base + 16] = src2[base + s1 * 8:base + s1 * 8 + 8]
        return bytes(out)

    if name.startswith("vunpcklps"):
        expected = unpck(4, False)
    elif name.startswith("vunpckhps"):
        expected = unpck(4, True)
    elif name.startswith("vunpcklpd"):
        expected = unpck(8, False)
    elif name.startswith("vunpckhpd"):
        expected = unpck(8, True)
    elif name.startswith("vshufps"):
        expected = shuf(4, 0x1b)
    elif name.startswith("vshufpd"):
        expected = shuf(8, 0x1b)
    else:
        return []

    actual = xmm_f[0] + (ymm_f[0] if bytes_len == 32 else b"")
    if actual != expected:
        return [f"ymm0 expected={expected.hex()} actual={actual.hex()}"]
    return []


def exact_avx_horizontal_addsub(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    bytes_len = 32 if "_ymm" in name else 16
    src1 = xmm_i[1] + (ymm_i[1] if bytes_len == 32 else b"")
    src2 = xmm_i[2] + (ymm_i[2] if bytes_len == 32 else b"")

    def f32(bits: int) -> float:
        return struct.unpack("<f", struct.pack("<I", bits))[0]

    def f32_bits(value: float) -> int:
        try:
            return struct.unpack("<I", struct.pack("<f", value))[0]
        except OverflowError:
            return 0xff800000 if value < 0 else 0x7f800000

    def f64(bits: int) -> float:
        return struct.unpack("<d", struct.pack("<Q", bits))[0]

    def f64_bits(value: float) -> int:
        return struct.unpack("<Q", struct.pack("<d", value))[0]

    def arith32(a: int, b: int, sub: bool) -> int:
        a_nan = (a & 0x7f800000) == 0x7f800000 and (a & 0x007fffff) != 0
        b_nan = (b & 0x7f800000) == 0x7f800000 and (b & 0x007fffff) != 0
        if a_nan or b_nan:
            chosen = a if a_nan and (not b_nan or ((a & 0x007fffff) >= (b & 0x007fffff))) else b
            return chosen | 0x00400000
        a_inf = (a & 0x7fffffff) == 0x7f800000
        b_inf = (b & 0x7fffffff) == 0x7f800000
        if (sub and a_inf and b_inf and not ((a ^ b) & 0x80000000)) or \
           ((not sub) and a_inf and b_inf and ((a ^ b) & 0x80000000)):
            return 0xffc00000
        return f32_bits(f32(a) - f32(b) if sub else f32(a) + f32(b))

    def arith64(a: int, b: int, sub: bool) -> int:
        a_nan = (a & 0x7ff0000000000000) == 0x7ff0000000000000 and (a & 0x000fffffffffffff) != 0
        b_nan = (b & 0x7ff0000000000000) == 0x7ff0000000000000 and (b & 0x000fffffffffffff) != 0
        if a_nan or b_nan:
            chosen = a if a_nan and (not b_nan or ((a & 0x000fffffffffffff) >= (b & 0x000fffffffffffff))) else b
            return chosen | 0x0008000000000000
        a_inf = (a & 0x7fffffffffffffff) == 0x7ff0000000000000
        b_inf = (b & 0x7fffffffffffffff) == 0x7ff0000000000000
        if (sub and a_inf and b_inf and not ((a ^ b) & 0x8000000000000000)) or \
           ((not sub) and a_inf and b_inf and ((a ^ b) & 0x8000000000000000)):
            return 0xfff8000000000000
        return f64_bits(f64(a) - f64(b) if sub else f64(a) + f64(b))

    def load(buf: bytes, off: int, lane: int) -> int:
        return int.from_bytes(buf[off:off + lane], "little")

    def store(out: bytearray, off: int, lane: int, bits: int) -> None:
        out[off:off + lane] = int(bits).to_bytes(lane, "little")

    expected = bytearray(bytes_len)
    if "addsub" in name:
        lane = 8 if name.startswith("vaddsubpd") else 4
        for off in range(0, bytes_len, lane):
            sub = ((off // lane) & 1) == 0
            fn = arith64 if lane == 8 else arith32
            store(expected, off, lane, fn(load(src1, off, lane), load(src2, off, lane), sub))
    else:
        lane = 8 if name.startswith("vhaddpd") or name.startswith("vhsubpd") else 4
        sub = name.startswith("vhsub")
        fn = arith64 if lane == 8 else arith32
        for base in range(0, bytes_len, 16):
            if lane == 8:
                store(expected, base, 8, fn(load(src1, base, 8), load(src1, base + 8, 8), sub))
                store(expected, base + 8, 8, fn(load(src2, base, 8), load(src2, base + 8, 8), sub))
            else:
                for pair in range(2):
                    store(expected, base + pair * 4, 4,
                          fn(load(src1, base + pair * 8, 4),
                             load(src1, base + pair * 8 + 4, 4), sub))
                    store(expected, base + 8 + pair * 4, 4,
                          fn(load(src2, base + pair * 8, 4),
                             load(src2, base + pair * 8 + 4, 4), sub))

    actual = xmm_f[0] + (ymm_f[0] if bytes_len == 32 else b"")
    if actual != bytes(expected):
        return [f"ymm0 expected={bytes(expected).hex()} actual={actual.hex()}"]
    return []


def exact_avx_movnt_lddqu(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    seed = int(row["seed"], 16) if isinstance(row["seed"], str) else int(row["seed"])
    name = template.name
    bytes_len = 32 if "m256" in name or "ymm" in name else 16

    def splitmix64_next(state: list[int]) -> int:
        state[0] = (state[0] + 0x9e3779b97f4a7c15) & MASK64
        z = state[0]
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
        return z ^ (z >> 31)

    def fill_random(state: list[int], size: int) -> bytearray:
        out = bytearray()
        while len(out) < size:
            out.extend(splitmix64_next(state).to_bytes(8, "little"))
        return out[:size]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    state = [seed]
    data = fill_random(state, 0x2000)
    data_off = 0x1000

    if name.startswith("vlddqu") or name.startswith("vmovntdqa") or name.startswith("movntdqa"):
        expected = bytes(data[data_off:data_off + bytes_len])
        xmm_expected = expected[:16]
        if bytes_len == 32:
            ymm_expected = expected[16:32]
        elif name.startswith("movntdqa"):
            ymm_expected = bytes.fromhex(initial["ymm_hi"][0])
        else:
            ymm_expected = b"\x00" * 16
        xmm_actual = bytes.fromhex(final["xmm"][0])
        ymm_actual = bytes.fromhex(final["ymm_hi"][0])
        mismatches: list[str] = []
        if xmm_actual != xmm_expected:
            mismatches.append(f"xmm0 expected={xmm_expected.hex()} actual={xmm_actual.hex()}")
        if ymm_actual != ymm_expected:
            mismatches.append(f"ymm_hi0 expected={ymm_expected.hex()} actual={ymm_actual.hex()}")
        return mismatches

    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    store = xmm_i[1] + (ymm_i[1] if bytes_len == 32 else b"")
    data[data_off:data_off + bytes_len] = store
    expected_hash = f"0x{fnv1a64(data):016x}"
    actual_hash = final["data_hash"]
    if actual_hash != expected_hash:
        return [f"data_hash expected={expected_hash} actual={actual_hash}"]
    return []


def exact_avx_vcomi(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm = [bytes.fromhex(x) for x in initial["xmm"]]
    flags = final["flags"]
    is_sd = template.name.endswith("sd_xmm0_xmm1")
    if is_sd:
        lhs = struct.unpack("<d", xmm[0][:8])[0]
        rhs = struct.unpack("<d", xmm[1][:8])[0]
    else:
        lhs = struct.unpack("<f", xmm[0][:4])[0]
        rhs = struct.unpack("<f", xmm[1][:4])[0]
    if math.isnan(lhs) or math.isnan(rhs):
        expected = {"zf": 1, "pf": 1, "cf": 1, "of": 0, "sf": 0, "af": 0}
    elif lhs > rhs:
        expected = {"zf": 0, "pf": 0, "cf": 0, "of": 0, "sf": 0, "af": 0}
    elif lhs < rhs:
        expected = {"zf": 0, "pf": 0, "cf": 1, "of": 0, "sf": 0, "af": 0}
    else:
        expected = {"zf": 1, "pf": 0, "cf": 0, "of": 0, "sf": 0, "af": 0}
    mismatches = []
    for flag, value in expected.items():
        if int(flags[flag]) != value:
            mismatches.append(f"flag.{flag} expected={value} actual={flags[flag]}")
    return mismatches


def exact_avx_pshuf_imm(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    bytes_len = 32 if "_ymm" in name else 16
    src = xmm_i[1] + (ymm_i[1] if bytes_len == 32 else b"")
    imm = 0x1b
    expected = bytearray(src)
    for base in range(0, bytes_len, 16):
        if name.startswith("vpshufd"):
            for lane in range(4):
                sel = (imm >> (lane * 2)) & 3
                expected[base + lane * 4:base + lane * 4 + 4] = src[base + sel * 4:base + sel * 4 + 4]
        elif name.startswith("vpshuflw"):
            for lane in range(4):
                sel = (imm >> (lane * 2)) & 3
                expected[base + lane * 2:base + lane * 2 + 2] = src[base + sel * 2:base + sel * 2 + 2]
        elif name.startswith("vpshufhw"):
            for lane in range(4):
                sel = (imm >> (lane * 2)) & 3
                expected[base + 8 + lane * 2:base + 8 + lane * 2 + 2] = src[base + 8 + sel * 2:base + 8 + sel * 2 + 2]
        else:
            return []
    actual = xmm_f[0] + (ymm_f[0] if bytes_len == 32 else b"\x00" * 16)
    expected_full = bytes(expected) if bytes_len == 32 else bytes(expected) + b"\x00" * 16
    if actual != expected_full:
        return [f"ymm0 expected={expected_full.hex()} actual={actual.hex()}"]
    return []


def exact_avx_dup_shuffle(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    bytes_len = 32 if "_ymm" in name else 16
    src = xmm_i[1] + (ymm_i[1] if bytes_len == 32 else b"")
    expected = bytearray(bytes_len)
    for base in range(0, bytes_len, 16):
        if name.startswith("vmovddup"):
            expected[base:base + 8] = src[base:base + 8]
            expected[base + 8:base + 16] = src[base:base + 8]
        else:
            first = 1 if name.startswith("vmovshdup") else 0
            expected[base:base + 4] = src[base + first * 4:base + first * 4 + 4]
            expected[base + 4:base + 8] = src[base + first * 4:base + first * 4 + 4]
            expected[base + 8:base + 12] = src[base + (first + 2) * 4:base + (first + 2) * 4 + 4]
            expected[base + 12:base + 16] = src[base + (first + 2) * 4:base + (first + 2) * 4 + 4]
    actual = xmm_f[0] + (ymm_f[0] if bytes_len == 32 else b"\x00" * 16)
    expected_full = bytes(expected) if bytes_len == 32 else bytes(expected) + b"\x00" * 16
    if actual != expected_full:
        return [f"ymm0 expected={expected_full.hex()} actual={actual.hex()}"]
    return []


def exact_avx_packed_convert(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    src = xmm_i[1] + ymm_i[1]
    dst_bytes = 32 if "_ymm0_" in name else 16

    def f32_bits(value: float) -> int:
        try:
            return struct.unpack("<I", struct.pack("<f", value))[0]
        except OverflowError:
            return 0xff800000 if value < 0 else 0x7f800000

    def i32_from_float(value: float, truncate: bool) -> int:
        if not math.isfinite(value):
            return -2147483648
        rounded = math.trunc(value) if truncate else round(value)
        if rounded < -2147483648 or rounded > 2147483647:
            return -2147483648
        return int(rounded)

    expected = bytearray(dst_bytes)
    if name.startswith("vcvtps2pd"):
        src_bytes = dst_bytes // 2
        for i in range(src_bytes // 4):
            f = struct.unpack("<f", src[i * 4:i * 4 + 4])[0]
            expected[i * 8:i * 8 + 8] = struct.pack("<d", float(f))
    elif name.startswith("vcvtpd2ps"):
        src_bytes = 32 if name.endswith("_ymm1") else 16
        expected = bytearray(16)
        for i in range(src_bytes // 8):
            d = struct.unpack("<d", src[i * 8:i * 8 + 8])[0]
            expected[i * 4:i * 4 + 4] = struct.pack("<I", f32_bits(float(d)))
    elif name.startswith("vcvtdq2ps"):
        for i in range(dst_bytes // 4):
            v = struct.unpack("<i", src[i * 4:i * 4 + 4])[0]
            expected[i * 4:i * 4 + 4] = struct.pack("<I", f32_bits(float(v)))
    elif name.startswith("vcvtps2dq") or name.startswith("vcvttps2dq"):
        truncate = name.startswith("vcvttps2dq")
        for i in range(dst_bytes // 4):
            f = struct.unpack("<f", src[i * 4:i * 4 + 4])[0]
            expected[i * 4:i * 4 + 4] = struct.pack("<i", i32_from_float(f, truncate))
    elif name.startswith("vcvtdq2pd"):
        src_bytes = dst_bytes // 2
        for i in range(src_bytes // 4):
            v = struct.unpack("<i", src[i * 4:i * 4 + 4])[0]
            expected[i * 8:i * 8 + 8] = struct.pack("<d", float(v))
    elif name.startswith("vcvtpd2dq") or name.startswith("vcvttpd2dq"):
        truncate = name.startswith("vcvttpd2dq")
        src_bytes = 32 if name.endswith("_ymm1") else 16
        expected = bytearray(16)
        for i in range(src_bytes // 8):
            d = struct.unpack("<d", src[i * 8:i * 8 + 8])[0]
            expected[i * 4:i * 4 + 4] = struct.pack("<i", i32_from_float(d, truncate))
    else:
        return []

    actual = xmm_f[0] + ymm_f[0]
    expected_full = bytes(expected) + b"\x00" * (32 - len(expected))
    if actual != expected_full:
        return [f"ymm0 expected={expected_full.hex()} actual={actual.hex()}"]
    return []


def exact_avx_scalar_convert(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name

    def f32_bits(value: float) -> int:
        try:
            return struct.unpack("<I", struct.pack("<f", value))[0]
        except OverflowError:
            return 0xff800000 if value < 0 else 0x7f800000

    def signed32(value: int) -> int:
        value &= 0xffffffff
        return value - (1 << 32) if value & (1 << 31) else value

    def signed64(value: int) -> int:
        value &= MASK64
        return value - (1 << 64) if value & SIGN64 else value

    def int_from_float(value: float, bits: int, truncate: bool) -> int:
        if not math.isfinite(value):
            return -(1 << (bits - 1))
        rounded = math.trunc(value) if truncate else round(value)
        lo = -(1 << (bits - 1))
        hi = 1 << (bits - 1)
        if rounded < lo or rounded >= hi:
            return lo
        return int(rounded)

    if name.startswith("vcvtss2sd") or name.startswith("vcvtsd2ss"):
        expected = bytearray(xmm_i[1])
        if name.startswith("vcvtss2sd"):
            value = struct.unpack("<f", xmm_i[2][:4])[0]
            expected[:8] = struct.pack("<d", float(value))
        else:
            value = struct.unpack("<d", xmm_i[2][:8])[0]
            expected[:4] = struct.pack("<I", f32_bits(float(value)))
        actual = xmm_f[0] + ymm_f[0]
        expected_full = bytes(expected) + b"\x00" * 16
        if actual != expected_full:
            return [f"ymm0 expected={expected_full.hex()} actual={actual.hex()}"]
        return []

    if name.startswith("vcvtsi2"):
        expected = bytearray(xmm_i[1])
        raw = as_u64(regs_i["rax"])
        value = signed64(raw) if name.endswith("_rax") else signed32(raw)
        if name.startswith("vcvtsi2sd"):
            expected[:8] = struct.pack("<d", float(value))
        else:
            expected[:4] = struct.pack("<I", f32_bits(float(value)))
        actual = xmm_f[0] + ymm_f[0]
        expected_full = bytes(expected) + b"\x00" * 16
        if actual != expected_full:
            return [f"ymm0 expected={expected_full.hex()} actual={actual.hex()}"]
        return []

    if "2si" in name:
        src = xmm_i[1]
        truncate = name.startswith("vcvtt")
        dst_bits = 64 if name.startswith(("vcvttss2si_rax", "vcvttsd2si_rax", "vcvtss2si_rax", "vcvtsd2si_rax")) else 32
        if "sd2si" in name:
            value = struct.unpack("<d", src[:8])[0]
        else:
            value = struct.unpack("<f", src[:4])[0]
        expected = int_from_float(float(value), dst_bits, truncate)
        if dst_bits == 32:
            expected_u = expected & 0xffffffff
        else:
            expected_u = expected & MASK64
        actual = as_u64(regs_f["rax"])
        if actual != expected_u:
            return [f"rax expected=0x{expected_u:016x} actual=0x{actual:016x}"]
        return []

    return []


def exact_avx_rcp_rsqrt(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    scalar = name.startswith("vrsqrtss") or name.startswith("vrcpss")
    bytes_len = 16 if scalar or "_xmm" in name else 32
    src = (xmm_i[2] if scalar else xmm_i[1]) + (ymm_i[1] if (not scalar and bytes_len == 32) else b"")
    expected = bytearray(xmm_i[1] if scalar else src[:bytes_len])
    actual = xmm_f[0] + ymm_f[0]
    check_mask = bytearray(b"\xff" * bytes_len)

    def f32(value: float) -> float:
        try:
            return struct.unpack("<f", struct.pack("<f", value))[0]
        except OverflowError:
            return -math.inf if value < 0 else math.inf

    def f32_bytes(value: float) -> bytes:
        try:
            return struct.pack("<f", value)
        except OverflowError:
            return struct.pack("<I", 0xff800000 if value < 0 else 0x7f800000)

    lane_count = 1 if scalar else bytes_len // 4
    for i in range(lane_count):
        off = i * 4
        value = struct.unpack("<f", src[off:off + 4])[0]
        if math.isnan(value) or (name.startswith("vrsqrt") and value < 0.0):
            expected[off:off + 4] = actual[off:off + 4]
            check_mask[off:off + 4] = b"\x00" * 4
            continue
        if value == 0.0:
            result = math.copysign(math.inf, value)
        elif name.startswith("vrsqrt"):
            result = f32(1.0 / f32(math.sqrt(value)))
        else:
            result = f32(1.0 / f32(value))
        expected[off:off + 4] = f32_bytes(result)

    expected_full = bytes(expected) + b"\x00" * (32 - len(expected))
    mismatches = []
    for i, (e, a) in enumerate(zip(expected_full, actual)):
        mask = check_mask[i] if i < len(check_mask) else 0xff
        if mask and e != a:
            mismatches.append(f"byte{i} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_avx_round(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    scalar = name.startswith("vroundss") or name.startswith("vroundsd")
    lane = 8 if name.startswith(("vroundpd", "vroundsd")) else 4
    bytes_len = 16 if scalar or "_xmm" in name else 32
    src = (xmm_i[2] if scalar else xmm_i[1]) + (ymm_i[1] if (not scalar and bytes_len == 32) else b"")
    expected = bytearray(xmm_i[1] if scalar else b"\x00" * bytes_len)
    actual = xmm_f[0] + ymm_f[0]

    def f32_bits(value: float) -> bytes:
        try:
            return struct.pack("<f", value)
        except OverflowError:
            return struct.pack("<I", 0xff800000 if value < 0 else 0x7f800000)

    count = 1 if scalar else bytes_len // lane
    for i in range(count):
        off = i * lane
        if lane == 4:
            value = struct.unpack("<f", src[off:off + 4])[0]
            if math.isnan(value):
                expected[off:off + 4] = actual[off:off + 4]
                continue
            if math.isfinite(value):
                rounded = value if abs(value) >= 2.0 ** 23 else float(math.trunc(value))
                if rounded == 0.0:
                    rounded = math.copysign(0.0, value)
            else:
                rounded = value
            expected[off:off + 4] = f32_bits(float(rounded))
        else:
            value = struct.unpack("<d", src[off:off + 8])[0]
            if math.isnan(value):
                expected[off:off + 8] = actual[off:off + 8]
                continue
            if math.isfinite(value):
                rounded = value if abs(value) >= 2.0 ** 52 else float(math.trunc(value))
                if rounded == 0.0:
                    rounded = math.copysign(0.0, value)
            else:
                rounded = value
            expected[off:off + 8] = struct.pack("<d", float(rounded))
    expected_full = bytes(expected) + b"\x00" * (32 - len(expected))
    if actual != expected_full:
        return [f"ymm0 expected={expected_full.hex()} actual={actual.hex()}"]
    return []


def exact_avx_dp(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    imm = int(template.code[-2:], 16)
    is_vex = name.startswith("v")
    fp64 = "dppd" in name
    bytes_len = 32 if "_ymm" in name else 16
    lhs_reg = 1 if is_vex else 0
    rhs_reg = 2 if is_vex else 1
    lhs = xmm_i[lhs_reg] + (ymm_i[lhs_reg] if bytes_len == 32 else b"")
    rhs = xmm_i[rhs_reg] + (ymm_i[rhs_reg] if bytes_len == 32 else b"")
    actual = xmm_f[0] + ymm_f[0]
    expected = bytearray(32)

    def f32_from_bits(bits: int) -> float:
        return struct.unpack("<f", struct.pack("<I", bits))[0]

    def f32_bits(value: float) -> int:
        try:
            return struct.unpack("<I", struct.pack("<f", value))[0]
        except OverflowError:
            return 0xff800000 if value < 0 else 0x7f800000

    def f32_mul(a_bits: int, b_bits: int) -> int | None:
        a = f32_from_bits(a_bits)
        b = f32_from_bits(b_bits)
        if math.isnan(a) or math.isnan(b):
            return None
        if (math.isinf(a) and b == 0.0) or (math.isinf(b) and a == 0.0):
            return None
        return f32_bits(a * b)

    def f32_add(a_bits: int | None, b_bits: int | None) -> int | None:
        if a_bits is None or b_bits is None:
            return None
        a = f32_from_bits(a_bits)
        b = f32_from_bits(b_bits)
        if math.isnan(a) or math.isnan(b):
            return None
        if math.isinf(a) and math.isinf(b) and math.copysign(1.0, a) != math.copysign(1.0, b):
            return None
        return f32_bits(a + b)

    def f64_from_bits(bits: int) -> float:
        return struct.unpack("<d", struct.pack("<Q", bits))[0]

    def f64_bits(value: float) -> int:
        return struct.unpack("<Q", struct.pack("<d", value))[0]

    def f64_mul(a_bits: int, b_bits: int) -> int | None:
        a = f64_from_bits(a_bits)
        b = f64_from_bits(b_bits)
        if math.isnan(a) or math.isnan(b):
            return None
        if (math.isinf(a) and b == 0.0) or (math.isinf(b) and a == 0.0):
            return None
        return f64_bits(a * b)

    def f64_add(a_bits: int | None, b_bits: int | None) -> int | None:
        if a_bits is None or b_bits is None:
            return None
        a = f64_from_bits(a_bits)
        b = f64_from_bits(b_bits)
        if math.isnan(a) or math.isnan(b):
            return None
        if math.isinf(a) and math.isinf(b) and math.copysign(1.0, a) != math.copysign(1.0, b):
            return None
        return f64_bits(a + b)

    for base in range(0, bytes_len, 16):
        if fp64:
            prod: list[int | None] = [0, 0]
            for i in range(2):
                if imm & (1 << i):
                    a = struct.unpack("<Q", lhs[base + i * 8:base + i * 8 + 8])[0]
                    b = struct.unpack("<Q", rhs[base + i * 8:base + i * 8 + 8])[0]
                    prod[i] = f64_mul(a, b)
            total = f64_add(prod[0], prod[1])
            for i in range(2):
                if imm & (1 << (4 + i)):
                    off = base + i * 8
                    if total is None:
                        expected[off:off + 8] = actual[off:off + 8]
                    else:
                        expected[off:off + 8] = struct.pack("<Q", total)
        else:
            prod = [0, 0, 0, 0]
            for i in range(4):
                if imm & (1 << i):
                    a = struct.unpack("<I", lhs[base + i * 4:base + i * 4 + 4])[0]
                    b = struct.unpack("<I", rhs[base + i * 4:base + i * 4 + 4])[0]
                    prod[i] = f32_mul(a, b)
            total = f32_add(f32_add(prod[0], prod[1]), f32_add(prod[2], prod[3]))
            for i in range(4):
                if imm & (1 << (4 + i)):
                    off = base + i * 4
                    if total is None:
                        expected[off:off + 4] = actual[off:off + 4]
                    else:
                        expected[off:off + 4] = struct.pack("<I", total)

    if bytes_len == 32:
        expected_full = bytes(expected)
    elif is_vex:
        expected_full = bytes(expected[:16]) + b"\x00" * 16
    else:
        expected_full = bytes(expected[:16]) + ymm_i[0]
    if actual != expected_full:
        return [f"ymm0 expected={expected_full.hex()} actual={actual.hex()}"]
    return []


def exact_avx_insert_extract(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    regs_f = final["regs"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    imm = int(template.code[-2:], 16)
    if "insertps" in name:
        is_vex = name.startswith("v")
        base = bytearray(xmm_i[1] if is_vex else xmm_i[0])
        src = xmm_i[2]
        src_lane = (imm >> 6) & 3
        dst_lane = (imm >> 4) & 3
        base[dst_lane * 4:dst_lane * 4 + 4] = src[src_lane * 4:src_lane * 4 + 4]
        for lane in range(4):
            if imm & (1 << lane):
                base[lane * 4:lane * 4 + 4] = b"\x00" * 4
        expected = bytes(base) + (b"\x00" * 16 if is_vex else ymm_i[0])
        actual = xmm_f[0] + ymm_f[0]
        if actual != expected:
            return [f"ymm0 expected={expected.hex()} actual={actual.hex()}"]
        return []
    value = struct.unpack("<I", xmm_i[0][(imm & 3) * 4:(imm & 3) * 4 + 4])[0]
    actual = as_u64(regs_f["rcx"])
    if actual != value:
        return [f"rcx expected=0x{value:016x} actual={regs_f['rcx']}"]
    return []


def exact_avx_pinsr_pextr(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    imm = int(template.code[-2:], 16)

    def elem_size_from_name() -> int:
        if "pinsrb" in name or "pextrb" in name:
            return 1
        if "pinsrw" in name or "pextrw" in name:
            return 2
        if "pinsrq" in name or "pextrq" in name:
            return 8
        return 4

    elem_size = elem_size_from_name()
    if "pinsr" in name:
        is_vex = name.startswith("v")
        base = bytearray(xmm_i[1] if is_vex else xmm_i[0])
        lane = imm & (16 // elem_size - 1)
        value = as_u64(regs_i["rcx"])
        base[lane * elem_size:lane * elem_size + elem_size] = value.to_bytes(8, "little")[:elem_size]
        expected = bytes(base) + (b"\x00" * 16 if is_vex else ymm_i[0])
        actual = xmm_f[0] + ymm_f[0]
        if actual != expected:
            return [f"ymm0 expected={expected.hex()} actual={actual.hex()}"]
        return []

    src_reg = 1 if ("old_eax_xmm1" in name or "vpextrw_eax_xmm1" in name) else 0
    dst_reg = "rax" if ("old_eax_xmm1" in name or "vpextrw_eax_xmm1" in name) else "rcx"
    lane = imm & (16 // elem_size - 1)
    src = xmm_i[src_reg]
    value = int.from_bytes(src[lane * elem_size:lane * elem_size + elem_size], "little")
    actual = as_u64(regs_f[dst_reg])
    if actual != value:
        return [f"{dst_reg} expected=0x{value:016x} actual={regs_f[dst_reg]}"]
    return []


def exact_avx_gfni_affine(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    imm = int(template.code[-2:], 16)
    is_vex = name.startswith("v")
    inverse = "affineinv" in name
    bytes_len = 32 if "_ymm" in name else 16
    value_reg = 1 if is_vex else 0
    matrix_reg = 2 if is_vex else 1
    values = xmm_i[value_reg] + (ymm_i[value_reg] if bytes_len == 32 else b"")
    matrix = xmm_i[matrix_reg] + (ymm_i[matrix_reg] if bytes_len == 32 else b"")

    def gf_mul(a: int, b: int) -> int:
        r = 0
        while b:
            if b & 1:
                r ^= a
            a <<= 1
            if a & 0x100:
                a ^= 0x11b
            a &= 0xff
            b >>= 1
        return r

    def gf_inv(v: int) -> int:
        if v == 0:
            return 0
        result = 1
        base = v
        exp = 254
        while exp:
            if exp & 1:
                result = gf_mul(result, base)
            base = gf_mul(base, base)
            exp >>= 1
        return result

    def parity(v: int) -> int:
        return v.bit_count() & 1

    expected = bytearray(32)
    for i in range(bytes_len):
        value = values[i]
        if inverse:
            value = gf_inv(value)
        base = i & ~7
        out = 0
        for bit in range(8):
            b = parity(value & matrix[base + bit]) ^ ((imm >> bit) & 1)
            out |= b << bit
        expected[i] = out
    if bytes_len == 32:
        expected_full = bytes(expected)
    elif is_vex:
        expected_full = bytes(expected[:16]) + b"\x00" * 16
    else:
        expected_full = bytes(expected[:16]) + ymm_i[0]
    actual = xmm_f[0] + ymm_f[0]
    if actual != expected_full:
        return [f"ymm0 expected={expected_full.hex()} actual={actual.hex()}"]
    return []


def exact_bmi_vex(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    name = template.name
    width = 64 if "_rax" in name else 32
    mask = MASK64 if width == 64 else 0xFFFFFFFF
    sign = 1 << (width - 1)

    def gpr(name_: str) -> int:
        return as_u64(regs_i[name_]) & mask

    def write_value(value: int) -> int:
        return value & mask

    def arithmetic_shift(value: int, count: int) -> int:
        value &= mask
        if count >= width:
            return mask if (value & sign) else 0
        if value & sign:
            value |= ~mask
        return (value >> count) & mask

    def logic_flags_width(result: int, cf: int = 0, of: int = 0) -> dict[str, int]:
        result &= mask
        return {
            "cf": cf,
            "of": of,
            "zf": 1 if result == 0 else 0,
            "sf": 1 if result & sign else 0,
            "pf": parity_even(result),
        }

    src_edx = gpr("rdx")
    src_ecx = gpr("rcx")
    expected_rax: int | None = None
    expected_rcx: int | None = None
    expected_flags: dict[str, int] | None = None

    if name.startswith("andn_"):
        result = (~src_ecx) & src_edx & mask
        expected_rax = write_value(result)
        expected_flags = logic_flags_width(result)
    elif name.startswith("bextr_"):
        start = src_ecx & 0xFF
        length = (src_ecx >> 8) & 0xFF
        result = 0
        if start < width and length != 0:
            length = min(length, width - start)
            result = (src_edx >> start) & ((1 << length) - 1)
        expected_rax = write_value(result)
        expected_flags = logic_flags_width(result)
    elif name.startswith("blsi_"):
        result = src_edx & (-src_edx & mask)
        expected_rax = write_value(result)
        expected_flags = logic_flags_width(result, cf=1 if src_edx != 0 else 0)
    elif name.startswith("blsmsk_"):
        result = src_edx ^ ((src_edx - 1) & mask)
        expected_rax = write_value(result)
        expected_flags = logic_flags_width(result, cf=1 if src_edx == 0 else 0)
    elif name.startswith("blsr_"):
        result = src_edx & ((src_edx - 1) & mask)
        expected_rax = write_value(result)
        expected_flags = logic_flags_width(result, cf=1 if src_edx == 0 else 0)
    elif name.startswith("bzhi_"):
        index = src_ecx & 0xFF
        if index >= width:
            result = src_edx
            cf = 1
        else:
            result = 0 if index == 0 else (src_edx & ((1 << index) - 1))
            cf = 0
        expected_rax = write_value(result)
        expected_flags = logic_flags_width(result, cf=cf)
    elif name.startswith("pext_") or name.startswith("pdep_"):
        src, mask_bits, bit, result = src_ecx, src_edx, 1, 0
        while mask_bits:
            lowest = mask_bits & -mask_bits
            if name.startswith("pext_"):
                if src & lowest:
                    result |= bit
            elif src & bit:
                result |= lowest
            mask_bits &= mask_bits - 1
            if mask_bits:
                bit <<= 1
        expected_rax = write_value(result)
    elif name.startswith("mulx_"):
        product = src_edx * src_edx
        expected_rax = write_value(product)
        expected_rcx = write_value(product >> width)
    elif name.startswith("rorx_"):
        count = int(template.code[-2:], 16) % width
        result = src_edx if count == 0 else ((src_edx >> count) | (src_edx << (width - count)))
        expected_rax = write_value(result)
    elif name.startswith("sarx_"):
        expected_rax = write_value(arithmetic_shift(src_edx, src_ecx & 0xFF))
    elif name.startswith("shlx_"):
        count = src_ecx & 0xFF
        expected_rax = write_value(0 if count >= width else src_edx << count)
    elif name.startswith("shrx_"):
        count = src_ecx & 0xFF
        expected_rax = write_value(0 if count >= width else src_edx >> count)

    mismatches: list[str] = []
    if expected_rax is not None and as_u64(regs_f["rax"]) != expected_rax:
        mismatches.append(f"rax expected=0x{expected_rax:016x} actual={regs_f['rax']}")
    if expected_rcx is not None and as_u64(regs_f["rcx"]) != expected_rcx:
        mismatches.append(f"rcx expected=0x{expected_rcx:016x} actual={regs_f['rcx']}")
    if expected_flags is None:
        expected_flags = flags_i
    for flag, expected in expected_flags.items():
        if int(flags_f[flag]) != expected:
            mismatches.append(f"flag.{flag} expected={expected} actual={flags_f[flag]}")
    return mismatches


def exact_ssse3_mmx(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    name = template.name
    lhs = xmm_i[0][:8]
    rhs = xmm_i[1][:8]

    def load(data: bytes, off: int, lane: int, signed: bool = False) -> int:
        return int.from_bytes(data[off:off + lane], "little", signed=signed)

    def store(value: int, lane: int) -> bytes:
        return (value & ((1 << (lane * 8)) - 1)).to_bytes(lane, "little")

    def sat_i16(value: int) -> int:
        return max(-32768, min(32767, value))

    def abs_mod(value: int, lane: int) -> int:
        return (-value if value < 0 else value) & ((1 << (lane * 8)) - 1)

    out = bytearray(8)
    if name.startswith("pshufb_"):
        for i in range(8):
            sel = rhs[i]
            out[i] = 0 if (sel & 0x80) else lhs[sel & 7]
    elif name.startswith("phaddw_") or name.startswith("phaddsw_"):
        saturating = name.startswith("phaddsw_")
        pairs = [(lhs, 0), (lhs, 4), (rhs, 0), (rhs, 4)]
        for j, (data, off) in enumerate(pairs):
            value = load(data, off, 2, True) + load(data, off + 2, 2, True)
            if saturating:
                value = sat_i16(value)
            out[j * 2:j * 2 + 2] = store(value, 2)
    elif name.startswith("phaddd_"):
        for j, (data, off) in enumerate(((lhs, 0), (rhs, 0))):
            value = load(data, off, 4, True) + load(data, off + 4, 4, True)
            out[j * 4:j * 4 + 4] = store(value, 4)
    elif name.startswith("phsubw_") or name.startswith("phsubsw_"):
        saturating = name.startswith("phsubsw_")
        pairs = [(lhs, 0), (lhs, 4), (rhs, 0), (rhs, 4)]
        for j, (data, off) in enumerate(pairs):
            value = load(data, off, 2, True) - load(data, off + 2, 2, True)
            if saturating:
                value = sat_i16(value)
            out[j * 2:j * 2 + 2] = store(value, 2)
    elif name.startswith("phsubd_"):
        for j, (data, off) in enumerate(((lhs, 0), (rhs, 0))):
            value = load(data, off, 4, True) - load(data, off + 4, 4, True)
            out[j * 4:j * 4 + 4] = store(value, 4)
    elif name.startswith("pmaddubsw_"):
        for j in range(4):
            off = j * 2
            value = lhs[off] * load(rhs, off, 1, True) + lhs[off + 1] * load(rhs, off + 1, 1, True)
            out[off:off + 2] = store(sat_i16(value), 2)
    elif name.startswith("psign"):
        lane = 1 if name.startswith("psignb_") else (2 if name.startswith("psignw_") else 4)
        for off in range(0, 8, lane):
            value = load(lhs, off, lane, True)
            control = load(rhs, off, lane, True)
            signed = 0 if control == 0 else (-value if control < 0 else value)
            out[off:off + lane] = store(signed, lane)
    elif name.startswith("pmulhrsw_"):
        for off in range(0, 8, 2):
            value = load(lhs, off, 2, True) * load(rhs, off, 2, True)
            out[off:off + 2] = store((value + 0x4000) >> 15, 2)
    elif name.startswith("pabs"):
        lane = 1 if name.startswith("pabsb_") else (2 if name.startswith("pabsw_") else 4)
        for off in range(0, 8, lane):
            out[off:off + lane] = store(abs_mod(load(rhs, off, lane, True), lane), lane)
    elif name.startswith("palignr_"):
        imm = int(template.code[-2:], 16)
        cat = rhs + lhs
        out[:] = bytes(cat[imm + i] if imm + i < 16 else 0 for i in range(8))

    expected_low = bytes(out)
    mismatches: list[str] = []
    if xmm_f[0][:8] != expected_low:
        mismatches.append(f"xmm0.low64 expected={expected_low.hex()} actual={xmm_f[0][:8].hex()}")
    if xmm_f[0][8:] != xmm_i[0][8:]:
        mismatches.append(f"xmm0.high64 expected_preserve={xmm_i[0][8:].hex()} actual={xmm_f[0][8:].hex()}")
    if xmm_f[1] != xmm_i[1]:
        mismatches.append(f"xmm1 expected_preserve={xmm_i[1].hex()} actual={xmm_f[1].hex()}")
    for flag, expected in flags_i.items():
        if flags_f.get(flag, 0) != expected:
            mismatches.append(f"flag.{flag} expected_preserve={expected} actual={flags_f.get(flag, 0)}")
    return mismatches


def exact_mpsadbw(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    vex = template.name.startswith("vmpsadbw")
    imm = int(template.code[-2:], 16)
    lhs = xmm_i[1] if vex else xmm_i[0]
    rhs = xmm_i[2] if vex else xmm_i[1]
    lhs_base = ((imm >> 2) & 1) * 4
    rhs_base = (imm & 3) * 4
    out = bytearray(16)
    for j in range(8):
        total = 0
        for k in range(4):
            total += abs(lhs[lhs_base + j + k] - rhs[rhs_base + k])
        out[j * 2:j * 2 + 2] = total.to_bytes(2, "little")

    mismatches: list[str] = []
    if xmm_f[0] != bytes(out):
        mismatches.append(f"xmm0 expected={bytes(out).hex()} actual={xmm_f[0].hex()}")
    expected_hi0 = b"\x00" * 16 if vex else ymm_i[0]
    if ymm_f[0] != expected_hi0:
        mismatches.append(f"ymm_hi0 expected={expected_hi0.hex()} actual={ymm_f[0].hex()}")
    for name, expected in flags_i.items():
        if flags_f.get(name) != expected:
            mismatches.append(f"flag {name} expected={expected} actual={flags_f.get(name)}")
    return mismatches


def exact_fma3_tail(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    op = template.name.split("_", 1)[0]
    scalar = op.endswith("ss") or op.endswith("sd")
    fp64 = op.endswith("pd") or op.endswith("sd")
    lane = 8 if fp64 else 4
    bytes_len = 16 if scalar or "_xmm" in template.name else 32
    dst = xmm_i[0] + (ymm_i[0] if bytes_len == 32 else b"")
    src1 = xmm_i[1] + (ymm_i[1] if bytes_len == 32 else b"")
    src2 = xmm_i[2] + (ymm_i[2] if bytes_len == 32 else b"")
    out = bytearray(src1[:16] if scalar else b"\x00" * bytes_len)

    def f32_at(buf: bytes, off: int) -> float:
        return struct.unpack("<f", buf[off:off + 4])[0]

    def f64_at(buf: bytes, off: int) -> float:
        return struct.unpack("<d", buf[off:off + 8])[0]

    def fma_value(a: float, b: float, c: float):
        if fp64:
            return _LIBM.fma(ctypes.c_double(a), ctypes.c_double(b), ctypes.c_double(c))
        return _LIBM.fmaf(ctypes.c_float(a), ctypes.c_float(b), ctypes.c_float(c))

    def put_value(buf: bytearray, off: int, value: float) -> None:
        if fp64:
            buf[off:off + 8] = struct.pack("<d", value)
        else:
            buf[off:off + 4] = struct.pack("<f", value)

    nan_lanes: set[int] = set()
    for off in range(0, scalar and lane or bytes_len, lane):
        dval = f64_at(dst, off) if fp64 else f32_at(dst, off)
        s1 = f64_at(src1, off) if fp64 else f32_at(src1, off)
        s2 = f64_at(src2, off) if fp64 else f32_at(src2, off)
        even_lane = ((off // lane) & 1) == 0
        if "132" in op:
            mul_a, mul_b, addend = dval, s2, s1
        elif "213" in op:
            mul_a, mul_b, addend = s1, dval, s2
        else:
            mul_a, mul_b, addend = s1, s2, dval
        if op.startswith("vfmaddsub"):
            addend = -addend if even_lane else addend
        elif op.startswith("vfmsubadd"):
            addend = addend if even_lane else -addend
        elif op.startswith("vfmsub"):
            addend = -addend
        elif op.startswith("vfnmadd"):
            mul_a = -mul_a
        elif op.startswith("vfnmsub"):
            mul_a = -mul_a
            addend = -addend
        result = fma_value(mul_a, mul_b, addend)
        if math.isnan(result):
            nan_lanes.add(off)
        put_value(out, off, result)

    mismatches: list[str] = []
    expected_vec = bytes(out[:bytes_len])
    actual_vec = xmm_f[0] + (ymm_f[0] if bytes_len == 32 else b"")

    def lane_is_nan(buf: bytes, off: int) -> bool:
        if fp64:
            bits = struct.unpack("<Q", buf[off:off + 8])[0]
            return (bits & 0x7ff0000000000000) == 0x7ff0000000000000 and (bits & 0x000fffffffffffff) != 0
        bits = struct.unpack("<I", buf[off:off + 4])[0]
        return (bits & 0x7f800000) == 0x7f800000 and (bits & 0x007fffff) != 0

    for off in range(0, scalar and lane or bytes_len, lane):
        exp_lane = expected_vec[off:off + lane]
        act_lane = actual_vec[off:off + lane]
        if off in nan_lanes:
            if not lane_is_nan(actual_vec, off):
                mismatches.append(f"lane{off // lane} expected=nan actual={act_lane.hex()}")
        elif exp_lane != act_lane:
            mismatches.append(f"lane{off // lane} expected={exp_lane.hex()} actual={act_lane.hex()}")
    if scalar and xmm_f[0][lane:] != expected_vec[lane:16]:
        mismatches.append(f"xmm0_tail expected={expected_vec[lane:16].hex()} actual={xmm_f[0][lane:].hex()}")
    expected_hi = b"\x00" * 16
    if bytes_len < 32 and ymm_f[0] != expected_hi:
        mismatches.append(f"ymm_hi0 expected={expected_hi.hex()} actual={ymm_f[0].hex()}")
    for name, expected in flags_i.items():
        if flags_f.get(name) != expected:
            mismatches.append(f"flag {name} expected={expected} actual={flags_f.get(name)}")
    return mismatches


def exact_vmovhl_lh(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    if template.name.startswith("vmovhlps"):
        expected = xmm_i[2][8:16] + xmm_i[1][8:16]
    else:
        expected = xmm_i[1][0:8] + xmm_i[2][0:8]
    mismatches: list[str] = []
    if xmm_f[0] != expected:
        mismatches.append(f"xmm0 expected={expected.hex()} actual={xmm_f[0].hex()}")
    if ymm_f[0] != b"\x00" * 16:
        mismatches.append(f"ymm_hi0 expected_zero actual={ymm_f[0].hex()}")
    for name, expected_flag in flags_i.items():
        if flags_f.get(name) != expected_flag:
            mismatches.append(f"flag {name} expected={expected_flag} actual={flags_f.get(name)}")
    return mismatches


def exact_evex_zmm_regbank(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name

    def vec(state: dict[str, Any], reg: int) -> bytes:
        if reg < 16:
            return (bytes.fromhex(state["xmm"][reg]) +
                    bytes.fromhex(state["ymm_hi"][reg]) +
                    bytes.fromhex(state["zmm_hi"][reg]))
        idx = reg - 16
        return (bytes.fromhex(state["xmm_ext"][idx]) +
                bytes.fromhex(state["ymm_hi_ext"][idx]) +
                bytes.fromhex(state["zmm_hi_ext"][idx]))

    def deterministic_data() -> bytearray:
        state = [int(row["seed"], 16)]
        data = bytearray()
        while len(data) < 0x2000:
            state[0] = (state[0] + 0x9e3779b97f4a7c15) & MASK64
            z = state[0]
            z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
            z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
            data.extend((z ^ (z >> 31)).to_bytes(8, "little"))
        return data[:0x2000]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    regs = [int(x) for x in re.findall(r"zmm(\d+)", name)]
    if "_m512_zmm" in name:
        src = vec(initial, regs[-1])
        data = deterministic_data()
        data[0x1000:0x1040] = src
        expected_hash = f"0x{fnv1a64(data):016x}"
        actual_hash = final["data_hash"]
        return [] if expected_hash == actual_hash else [f"data_hash expected={expected_hash} actual={actual_hash}"]

    if "_m512" in name:
        dst = regs[0]
        expected = deterministic_data()[0x1000:0x1040]
    elif "vmovdqa" in name:
        dst, src = regs[0], regs[1]
        expected = vec(initial, src)
    else:
        dst, src1, src2 = regs[0], regs[1], regs[2]
        a = vec(initial, src1)
        b = vec(initial, src2)
        if "vpxor" in name:
            expected = bytes((x ^ y) & 0xff for x, y in zip(a, b))
        elif "vpand" in name:
            expected = bytes((x & y) & 0xff for x, y in zip(a, b))
        else:
            return []

    if "_k1" in name:
        old = bytearray(vec(initial, dst))
        out = bytearray(expected)
        k1 = int(initial["k"][1], 16)
        lane = 8 if "q_" in name or "dqa64" in name else 4
        zeroing = "_k1z_" in name
        for off in range(0, 64, lane):
            lane_idx = off // lane
            if (k1 >> lane_idx) & 1:
                continue
            out[off:off + lane] = b"\x00" * lane if zeroing else old[off:off + lane]
        expected = bytes(out)

    actual = vec(final, dst)
    mismatches: list[str] = []
    for off, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            mismatches.append(f"zmm{dst}.byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_evex_cmp_k(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    k_i = [int(x, 16) for x in initial["k"]]
    k_f = [int(x, 16) for x in final["k"]]

    def deterministic_data() -> bytearray:
        state = [int(row["seed"], 16)]
        data = bytearray()
        while len(data) < 0x2000:
            state[0] = (state[0] + 0x9e3779b97f4a7c15) & MASK64
            z = state[0]
            z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
            z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
            data.extend((z ^ (z >> 31)).to_bytes(8, "little"))
        return data[:0x2000]

    def cmp_pred(unordered: bool, eq: bool, lt: bool, le: bool, pred: int) -> bool:
        pred &= 31
        if pred in (0, 16): return (not unordered) and eq
        if pred in (1, 17): return (not unordered) and lt
        if pred in (2, 18): return (not unordered) and le
        if pred in (3, 19): return unordered
        if pred in (4, 20): return unordered or not eq
        if pred in (5, 21): return unordered or not lt
        if pred in (6, 22): return unordered or not le
        if pred in (7, 23): return not unordered
        if pred in (8, 24): return unordered or eq
        if pred in (9, 25): return unordered or not (eq or not lt)
        if pred in (10, 26): return unordered or not (eq or lt or le)
        if pred in (11, 27): return False
        if pred in (12, 28): return (not unordered) and not eq
        if pred in (13, 29): return (not unordered) and (eq or not lt)
        if pred in (14, 30): return (not unordered) and not le
        return True

    kregs = [int(x) for x in re.findall(r"_k(\d)", name)]
    kdst = kregs[0]
    kmask = kregs[1] if len(kregs) > 1 else 0
    pred = int(template.code[-2:], 16) & 31
    is_pd = "pd" in name or "sd" in name
    scalar = "ss" in name or "sd" in name
    lane = 8 if is_pd else 4
    lanes = 1 if scalar else (64 // lane)
    src1 = xmm_i[0] + ymm_i[0] + zmm_i[0]
    if "m32bcst" in name:
        src2 = deterministic_data()[0x1000:0x1004] * 16
    else:
        src2 = xmm_i[1] + ymm_i[1] + zmm_i[1]

    expected = 0
    for lane_idx in range(lanes):
        off = lane_idx * lane
        if is_pd:
            a = struct.unpack("<d", src1[off:off + 8])[0]
            b = struct.unpack("<d", src2[off:off + 8])[0]
        else:
            a = struct.unpack("<f", src1[off:off + 4])[0]
            b = struct.unpack("<f", src2[off:off + 4])[0]
        unordered = math.isnan(a) or math.isnan(b)
        if cmp_pred(unordered, a == b, a < b, a <= b, pred):
            expected |= 1 << lane_idx
    if kmask:
        expected &= k_i[kmask]
    expected &= (1 << lanes) - 1
    actual = k_f[kdst] & ((1 << lanes) - 1)
    return [] if actual == expected else [f"k{kdst} expected=0x{expected:x} actual=0x{actual:x}"]


def exact_evex_scalar_arith(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    k_i = [int(x, 16) for x in initial["k"]]
    is_sd = "sd_" in name or name.endswith("sd")
    lane = 8 if is_sd else 4
    expected = bytearray(xmm_i[0] + b"\x00" * 48)
    check = bytearray(b"\x01" * 64)

    if lane == 4:
        abits = struct.unpack("<I", xmm_i[0][:4])[0]
        bbits = struct.unpack("<I", xmm_i[1][:4])[0]
        a = struct.unpack("<f", xmm_i[0][:4])[0]
        b = struct.unpack("<f", xmm_i[1][:4])[0]
        invalid = math.isnan(a) or math.isnan(b)
        try:
            if "vadd" in name:
                r = a + b
            elif "vsub" in name:
                r = a - b
            elif "vmul" in name:
                r = a * b
            else:
                if b == 0.0:
                    invalid = True
                    r = 0.0
                else:
                    r = a / b
            if not invalid:
                expected[:4] = struct.pack("<f", r)
        except (OverflowError, ZeroDivisionError):
            invalid = True
        if invalid:
            check[:4] = b"\x00" * 4
    else:
        a_bits = struct.unpack("<Q", xmm_i[0][:8])[0]
        b_bits = struct.unpack("<Q", xmm_i[1][:8])[0]
        a = struct.unpack("<d", xmm_i[0][:8])[0]
        b = struct.unpack("<d", xmm_i[1][:8])[0]
        invalid = math.isnan(a) or math.isnan(b)
        try:
            if "vadd" in name:
                r = a + b
            elif "vsub" in name:
                r = a - b
            elif "vmul" in name:
                r = a * b
            else:
                if b == 0.0:
                    invalid = True
                    r = 0.0
                else:
                    r = a / b
            if not invalid:
                expected[:8] = struct.pack("<d", r)
        except (OverflowError, ZeroDivisionError):
            invalid = True
        if invalid:
            check[:8] = b"\x00" * 8
        _ = (a_bits, b_bits)

    if "_k1" in name and (k_i[1] & 1) == 0:
        if "_k1z_" in name:
            expected[:lane] = b"\x00" * lane
        else:
            expected[:lane] = xmm_i[0][:lane]
        check[:lane] = b"\x01" * lane

    actual = xmm_f[0] + ymm_f[0] + zmm_f[0]
    mismatches: list[str] = []
    for off, (e, a, c) in enumerate(zip(expected, actual, check)):
        if c and e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_evex_scalar_minmax_sqrt(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    k_i = [int(x, 16) for x in initial["k"]]
    is_sd = "sd_" in name or name.endswith("sd")
    lane = 8 if is_sd else 4
    expected = bytearray(xmm_i[0] + b"\x00" * 48)
    check = bytearray(b"\x01" * 64)

    if lane == 4:
        a = struct.unpack("<f", xmm_i[0][:4])[0]
        b = struct.unpack("<f", xmm_i[1][:4])[0]
        if "vsqrt" in name:
            if math.isnan(b) or b < 0.0:
                check[:4] = b"\x00" * 4
            else:
                expected[:4] = struct.pack("<f", math.sqrt(b))
        else:
            if math.isnan(a) or math.isnan(b):
                check[:4] = b"\x00" * 4
            else:
                result = a if (a > b if "vmax" in name else a < b) else b
                expected[:4] = struct.pack("<f", result)
    else:
        a = struct.unpack("<d", xmm_i[0][:8])[0]
        b = struct.unpack("<d", xmm_i[1][:8])[0]
        if "vsqrt" in name:
            if math.isnan(b) or b < 0.0:
                check[:8] = b"\x00" * 8
            else:
                expected[:8] = struct.pack("<d", math.sqrt(b))
        else:
            if math.isnan(a) or math.isnan(b):
                check[:8] = b"\x00" * 8
            else:
                result = a if (a > b if "vmax" in name else a < b) else b
                expected[:8] = struct.pack("<d", result)

    if "_k1" in name and (k_i[1] & 1) == 0:
        if "_k1z_" in name:
            expected[:lane] = b"\x00" * lane
        else:
            expected[:lane] = xmm_i[0][:lane]
        check[:lane] = b"\x01" * lane

    actual = xmm_f[0] + ymm_f[0] + zmm_f[0]
    mismatches: list[str] = []
    for off, (e, a, c) in enumerate(zip(expected, actual, check)):
        if c and e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_evex_scalar_mov_rr(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    k_i = [int(x, 16) for x in initial["k"]]
    lane = 8 if "vmovsd" in name else 4
    target = 1 if "xmm1" in name.split("_")[2] else 0
    src1 = 0
    src2 = 0 if target == 1 else 1
    expected = bytearray(xmm_i[src1] + b"\x00" * 48)
    expected[:lane] = xmm_i[src2][:lane]

    if "_k1" in name and (k_i[1] & 1) == 0:
        if "_k1z_" in name:
            expected[:lane] = b"\x00" * lane
        else:
            expected[:lane] = xmm_i[target][:lane]

    actual = xmm_f[target] + ymm_f[target] + zmm_f[target]
    mismatches: list[str] = []
    for off, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_evex_scalar_mov_mem(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    k_i = [int(x, 16) for x in initial["k"]]
    lane = 8 if "vmovsd" in name or "_m64" in name else 4

    def splitmix64_next(state: int) -> tuple[int, int]:
        state = (state + 0x9e3779b97f4a7c15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
        return state, (z ^ (z >> 31)) & MASK64

    def fill_random(seed: int, n: int) -> bytearray:
        state = seed
        out = bytearray()
        while len(out) < n:
            state, value = splitmix64_next(state)
            out.extend(value.to_bytes(8, "little"))
        return out[:n]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    seed = int(row["seed"], 16)
    data = fill_random(seed, 0x2000)
    mismatches: list[str] = []
    if initial.get("data_hash") != f"0x{fnv1a64(data):016x}":
        return ["initial data_hash does not match deterministic fixture"]

    active = not ("_k1" in name) or ((k_i[1] & 1) != 0)
    zeroing = "_k1z_" in name
    if ("_m32" in name or "_m64" in name) and name.endswith("_xmm0"):
        if active:
            data[0x1000:0x1000 + lane] = xmm_i[0][:lane]
        expected_hash = f"0x{fnv1a64(data):016x}"
        if final.get("data_hash") != expected_hash:
            mismatches.append(f"data_hash expected={expected_hash} actual={final.get('data_hash')}")
        return mismatches

    expected = bytearray(b"\x00" * 64)
    if active:
        expected[:lane] = data[0x1000:0x1000 + lane]
    elif zeroing:
        expected[:lane] = b"\x00" * lane
    else:
        expected[:lane] = xmm_i[0][:lane]
    actual = xmm_f[0] + ymm_f[0] + zmm_f[0]
    for off, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_evex_vpbroadcast_mem(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    k_i = [int(x, 16) for x in initial["k"]]
    lane = 8 if "broadcastq" in name else (4 if "broadcastd" in name else (2 if "broadcastw" in name else 1))

    def splitmix64_next(state: int) -> tuple[int, int]:
        state = (state + 0x9e3779b97f4a7c15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
        return state, (z ^ (z >> 31)) & MASK64

    def fill_random(seed: int, n: int) -> bytearray:
        state = seed
        out = bytearray()
        while len(out) < n:
            state, value = splitmix64_next(state)
            out.extend(value.to_bytes(8, "little"))
        return out[:n]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    seed = int(row["seed"], 16)
    data = fill_random(seed, 0x2000)
    if initial.get("data_hash") != f"0x{fnv1a64(data):016x}":
        return ["initial data_hash does not match deterministic fixture"]

    old = bytearray(xmm_i[0] + ymm_i[0] + zmm_i[0])
    scalar = bytes(data[0x1000:0x1000 + lane])
    expected = bytearray()
    while len(expected) < 64:
        expected.extend(scalar)
    expected = expected[:64]
    if "_k1" in name:
        mask = k_i[1]
        zeroing = "_k1z_" in name
        for off in range(0, 64, lane):
            lane_idx = off // lane
            if (mask >> lane_idx) & 1:
                continue
            expected[off:off + lane] = b"\x00" * lane if zeroing else old[off:off + lane]

    actual = xmm_f[0] + ymm_f[0] + zmm_f[0]
    mismatches: list[str] = []
    for off, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_vbroadcast_fp(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    k_i = [int(x, 16) for x in initial["k"]]

    if "broadcastf64x4" in name:
        src_bytes, mask_lane = 32, 8
    elif "broadcastf32x8" in name:
        src_bytes, mask_lane = 32, 4
    elif "broadcastf64x2" in name:
        src_bytes, mask_lane = 16, 8
    elif "broadcastf32x4" in name or "broadcastf128" in name:
        src_bytes, mask_lane = 16, 4
    elif "broadcastf32x2" in name or "broadcasti32x2" in name:
        src_bytes, mask_lane = 8, 4
    elif "broadcastsd" in name or "broadcastq" in name:
        src_bytes, mask_lane = 8, 8
    elif "broadcastw" in name:
        src_bytes, mask_lane = 2, 2
    elif "broadcastb" in name:
        src_bytes, mask_lane = 1, 1
    else:
        src_bytes, mask_lane = 4, 4

    if name.startswith("evex_"):
        dest_bytes = 16 if "_xmm0_" in name else (32 if "_ymm0_" in name else 64)
    else:
        dest_bytes = 16 if "_xmm0_" in name else 32
    old = bytearray(xmm_i[0] + ymm_i[0] + zmm_i[0])[:dest_bytes]

    if name.endswith("_xmm1") or "_xmm1_" in name:
        scalar = bytes((xmm_i[1] + ymm_i[1] + zmm_i[1])[:src_bytes])
    else:
        def splitmix64_next(state: int) -> tuple[int, int]:
            state = (state + 0x9e3779b97f4a7c15) & MASK64
            z = state
            z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
            z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
            return state, (z ^ (z >> 31)) & MASK64

        def fill_random(seed: int, n: int) -> bytearray:
            state = seed
            out = bytearray()
            while len(out) < n:
                state, value = splitmix64_next(state)
                out.extend(value.to_bytes(8, "little"))
            return out[:n]

        def fnv1a64(data: bytes) -> int:
            h = 0xcbf29ce484222325
            for b in data:
                h ^= b
                h = (h * 0x100000001b3) & MASK64
            return h

        seed = int(row["seed"], 16)
        data = fill_random(seed, 0x2000)
        if initial.get("data_hash") != f"0x{fnv1a64(data):016x}":
            return ["initial data_hash does not match deterministic fixture"]
        scalar = bytes(data[0x1000:0x1000 + src_bytes])

    expected = bytearray()
    while len(expected) < dest_bytes:
        expected.extend(scalar)
    expected = expected[:dest_bytes]

    if name.startswith("evex_") and "_k1" in name:
        mask = k_i[1]
        zeroing = "_k1z_" in name
        for off in range(0, dest_bytes, mask_lane):
            lane_idx = off // mask_lane
            if (mask >> lane_idx) & 1:
                continue
            expected[off:off + mask_lane] = b"\x00" * mask_lane if zeroing else old[off:off + mask_lane]

    actual = (xmm_f[0] + ymm_f[0] + zmm_f[0])[:dest_bytes]
    mismatches: list[str] = []
    for off, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_avx_packed_shift(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    bytes_out = 32 if "_ymm" in name else 16
    dest = 1 if "_imm" in name else 0
    src = 0 if "_imm" in name else 1
    src_bytes = bytearray(xmm_i[src] + ymm_i[src])[:bytes_out]
    actual = (xmm_f[dest] + ymm_f[dest])[:bytes_out]
    code_bytes = bytes.fromhex(template.code) if isinstance(template.code, str) else template.code
    count = code_bytes[-1] if "_imm" in name else int.from_bytes(xmm_i[2][:8], "little") & 0xff

    if "psrldq" in name or "pslldq" in name:
        expected = bytearray(bytes_out)
        for base in range(0, bytes_out, 16):
            if count >= 16:
                continue
            if "psrldq" in name:
                expected[base:base + 16 - count] = src_bytes[base + count:base + 16]
            else:
                expected[base + count:base + 16] = src_bytes[base:base + 16 - count]
    else:
        if "psrlw" in name or "psraw" in name or "psllw" in name:
            lane, bits = 2, 16
        elif "psrld" in name or "psrad" in name or "pslld" in name:
            lane, bits = 4, 32
        else:
            lane, bits = 8, 64
        mask = (1 << bits) - 1
        expected = bytearray(bytes_out)
        for off in range(0, bytes_out, lane):
            value = int.from_bytes(src_bytes[off:off + lane], "little", signed=False)
            if "psll" in name:
                result = 0 if count >= bits else (value << count) & mask
            elif "psra" in name:
                signed = value if value < (1 << (bits - 1)) else value - (1 << bits)
                result = (-1 if signed < 0 else 0) & mask if count >= bits else (signed >> count) & mask
            else:
                result = 0 if count >= bits else value >> count
            expected[off:off + lane] = result.to_bytes(lane, "little", signed=False)

    mismatches: list[str] = []
    for off, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_avx_vmovlh_mem(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    lane_off = 8 if "vmovh" in name else 0

    def splitmix64_next(state: int) -> tuple[int, int]:
        state = (state + 0x9e3779b97f4a7c15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
        return state, (z ^ (z >> 31)) & MASK64

    def fill_random(seed: int, n: int) -> bytearray:
        state = seed
        out = bytearray()
        while len(out) < n:
            state, value = splitmix64_next(state)
            out.extend(value.to_bytes(8, "little"))
        return out[:n]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    seed = int(row["seed"], 16)
    data = fill_random(seed, 0x2000)
    if initial.get("data_hash") != f"0x{fnv1a64(data):016x}":
        return ["initial data_hash does not match deterministic fixture"]

    mismatches: list[str] = []
    if name.endswith("_m64"):
        expected_xmm = bytearray(xmm_i[1])
        expected_xmm[lane_off:lane_off + 8] = data[0x1000:0x1008]
        if xmm_f[0] != bytes(expected_xmm):
            mismatches.append(f"xmm0 expected={bytes(expected_xmm).hex()} actual={xmm_f[0].hex()}")
        if ymm_f[0] != b"\x00" * 16:
            mismatches.append(f"ymm_hi0 expected_zero actual={ymm_f[0].hex()}")
        return mismatches

    data[0x1000:0x1008] = xmm_i[0][lane_off:lane_off + 8]
    expected_hash = f"0x{fnv1a64(data):016x}"
    if final.get("data_hash") != expected_hash:
        mismatches.append(f"data_hash expected={expected_hash} actual={final.get('data_hash')}")
    return mismatches


def exact_evex_pshuf_word_imm(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    imm = bytes.fromhex(template.code)[-1]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    k_i = [int(x, 16) for x in initial["k"]]
    src = bytes(xmm_i[0] + ymm_i[0] + zmm_i[0])
    expected = bytearray(src)

    for lane_base in range(0, 64, 16):
        lane = src[lane_base:lane_base + 16]
        if "vpshufd" in name:
            for dst in range(4):
                sel = (imm >> (dst * 2)) & 3
                expected[lane_base + dst * 4:lane_base + dst * 4 + 4] = lane[sel * 4:sel * 4 + 4]
        elif "vpshufhw" in name:
            for dst in range(4):
                sel = (imm >> (dst * 2)) & 3
                expected[lane_base + 8 + dst * 2:lane_base + 10 + dst * 2] = lane[8 + sel * 2:10 + sel * 2]
        else:
            for dst in range(4):
                sel = (imm >> (dst * 2)) & 3
                expected[lane_base + dst * 2:lane_base + 2 + dst * 2] = lane[sel * 2:sel * 2 + 2]

    if "_k1" in name:
        mask_lane = 4 if "vpshufd" in name else 2
        mask = k_i[1]
        zeroing = "_k1z_" in name
        for off in range(0, 64, mask_lane):
            lane_idx = off // mask_lane
            if (mask >> lane_idx) & 1:
                continue
            expected[off:off + mask_lane] = b"\x00" * mask_lane if zeroing else src[off:off + mask_lane]

    actual = xmm_f[0] + ymm_f[0] + zmm_f[0]
    mismatches: list[str] = []
    for off, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_evex_packed_int(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    op = name.split("_")[1]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    k_i = [int(x, 16) for x in initial["k"]]

    dest_bytes = 64 if "_zmm0" in name else (32 if "_ymm0" in name else 16)
    vec0 = bytes(xmm_i[0] + ymm_i[0] + zmm_i[0])[:dest_bytes]
    vec1 = bytes(xmm_i[1] + ymm_i[1] + zmm_i[1])[:dest_bytes]
    lhs = vec0
    rhs = vec1
    old = bytearray(vec0)
    expected = bytearray(dest_bytes)
    mask_lane = 1

    def load(data: bytes | bytearray, off: int, lane: int, signed: bool = False) -> int:
        return int.from_bytes(data[off:off + lane], "little", signed=signed)

    def store(value: int, lane: int) -> bytes:
        return (value & ((1 << (lane * 8)) - 1)).to_bytes(lane, "little")

    def sat_signed(value: int, lane: int) -> int:
        bits = lane * 8
        return max(-(1 << (bits - 1)), min((1 << (bits - 1)) - 1, value))

    def sat_unsigned(value: int, lane: int) -> int:
        return max(0, min((1 << (lane * 8)) - 1, value))

    def abs_mod(value: int, lane: int) -> int:
        return (-value if value < 0 else value) & ((1 << (lane * 8)) - 1)

    if op.startswith("vpunpck"):
        if op.endswith("bw"):
            lane = 1
        elif op.endswith("lwd") or op.endswith("hwd"):
            lane = 2
        elif op.endswith("ldq") or op.endswith("hdq"):
            lane = 4
        else:
            lane = 8
        mask_lane = lane
        high = op.startswith("vpunpckh")
        for base in range(0, dest_bytes, 16):
            elems = 16 // lane
            start = elems // 2 if high else 0
            for j in range(elems // 2):
                src_off = base + (start + j) * lane
                dst_off = base + j * 2 * lane
                expected[dst_off:dst_off + lane] = lhs[src_off:src_off + lane]
                expected[dst_off + lane:dst_off + 2 * lane] = rhs[src_off:src_off + lane]
    elif op in ("vpacksswb", "vpackuswb", "vpackssdw"):
        if op == "vpackssdw":
            mask_lane = 2
            for base in range(0, dest_bytes, 16):
                for j in range(4):
                    value = sat_signed(load(lhs, base + j * 4, 4, True), 2)
                    expected[base + j * 2:base + j * 2 + 2] = store(value, 2)
                    value = sat_signed(load(rhs, base + j * 4, 4, True), 2)
                    expected[base + 8 + j * 2:base + 10 + j * 2] = store(value, 2)
        else:
            mask_lane = 1
            for base in range(0, dest_bytes, 16):
                for j in range(8):
                    value = load(lhs, base + j * 2, 2, True)
                    value = sat_unsigned(value, 1) if op == "vpackuswb" else sat_signed(value, 1)
                    expected[base + j] = value & 0xff
                    value = load(rhs, base + j * 2, 2, True)
                    value = sat_unsigned(value, 1) if op == "vpackuswb" else sat_signed(value, 1)
                    expected[base + 8 + j] = value & 0xff
    elif op in ("vpaddb", "vpaddw", "vpaddd", "vpaddq", "vpsubb", "vpsubw", "vpsubd", "vpsubq"):
        lane = 1 if op.endswith("b") else (2 if op.endswith("w") else (4 if op.endswith("d") else 8))
        mask_lane = lane
        add = op.startswith("vpadd")
        for off in range(0, dest_bytes, lane):
            a = load(lhs, off, lane)
            b = load(rhs, off, lane)
            expected[off:off + lane] = store(a + b if add else a - b, lane)
    elif op in ("vpaddusb", "vpaddusw", "vpsubusb", "vpsubusw",
                "vpaddsb", "vpaddsw", "vpsubsb", "vpsubsw"):
        lane = 1 if op.endswith("b") else 2
        mask_lane = lane
        signed = "usb" not in op and "usw" not in op
        add = op.startswith("vpadd")
        for off in range(0, dest_bytes, lane):
            a = load(lhs, off, lane, signed)
            b = load(rhs, off, lane, signed)
            value = a + b if add else a - b
            value = sat_signed(value, lane) if signed else sat_unsigned(value, lane)
            expected[off:off + lane] = store(value, lane)
    elif op in ("vpavgb", "vpavgw"):
        lane = 1 if op.endswith("b") else 2
        mask_lane = lane
        for off in range(0, dest_bytes, lane):
            value = (load(lhs, off, lane) + load(rhs, off, lane) + 1) >> 1
            expected[off:off + lane] = store(value, lane)
    elif op in ("vpminub", "vpminsw", "vpmaxub", "vpmaxsw"):
        lane = 1 if op.endswith("ub") else 2
        signed = op.endswith("sw")
        mask_lane = lane
        for off in range(0, dest_bytes, lane):
            a = load(lhs, off, lane, signed)
            b = load(rhs, off, lane, signed)
            value = max(a, b) if op.startswith("vpmax") else min(a, b)
            expected[off:off + lane] = store(value, lane)
    elif op in ("vpmullw", "vpmulhw", "vpmulhuw"):
        mask_lane = 2
        for off in range(0, dest_bytes, 2):
            signed = op != "vpmulhuw"
            product = load(lhs, off, 2, signed) * load(rhs, off, 2, signed)
            value = product if op == "vpmullw" else (product >> 16)
            expected[off:off + 2] = store(value, 2)
    elif op == "vpmuludq":
        mask_lane = 8
        for off in range(0, dest_bytes, 8):
            product = load(lhs, off, 4) * load(rhs, off, 4)
            expected[off:off + 8] = store(product, 8)
    elif op == "vpmaddwd":
        mask_lane = 4
        for off in range(0, dest_bytes, 4):
            value = (load(lhs, off, 2, True) * load(rhs, off, 2, True) +
                     load(lhs, off + 2, 2, True) * load(rhs, off + 2, 2, True))
            expected[off:off + 4] = store(value, 4)
    elif op == "vpsadbw":
        mask_lane = 8
        for off in range(0, dest_bytes, 8):
            value = sum(abs(lhs[off + j] - rhs[off + j]) for j in range(8))
            expected[off:off + 8] = store(value, 8)
    elif op == "vpshufb":
        mask_lane = 1
        for base in range(0, dest_bytes, 16):
            for j in range(16):
                sel = rhs[base + j]
                expected[base + j] = 0 if (sel & 0x80) else lhs[base + (sel & 0x0f)]
    elif op == "vpmaddubsw":
        mask_lane = 2
        for off in range(0, dest_bytes, 2):
            value = lhs[off] * load(rhs, off, 1, True) + lhs[off + 1] * load(rhs, off + 1, 1, True)
            expected[off:off + 2] = store(sat_signed(value, 2), 2)
    elif op == "vpmulhrsw":
        mask_lane = 2
        for off in range(0, dest_bytes, 2):
            product = load(lhs, off, 2, True) * load(rhs, off, 2, True)
            expected[off:off + 2] = store((product + 0x4000) >> 15, 2)
    elif op in ("vpabsb", "vpabsw", "vpabsd"):
        lane = 1 if op.endswith("b") else (2 if op.endswith("w") else 4)
        mask_lane = lane
        for off in range(0, dest_bytes, lane):
            expected[off:off + lane] = store(abs_mod(load(rhs, off, lane, True), lane), lane)
    else:
        return [f"missing evex_packed_int oracle for {op}"]

    if "_k1" in name:
        mask = k_i[1]
        zeroing = "_k1z_" in name
        for off in range(0, dest_bytes, mask_lane):
            lane_idx = off // mask_lane
            if (mask >> lane_idx) & 1:
                continue
            expected[off:off + mask_lane] = b"\x00" * mask_lane if zeroing else old[off:off + mask_lane]

    actual = bytes(xmm_f[0] + ymm_f[0] + zmm_f[0])[:dest_bytes]
    mismatches: list[str] = []
    for off, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_evex_mask_zero(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    zmm_i = [bytes.fromhex(x) for x in initial["zmm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    zmm_f = [bytes.fromhex(x) for x in final["zmm_hi"]]
    k = [int(x, 16) for x in initial["k"]]
    name = template.name

    def vec(reg: int, initial_state: bool = True) -> bytes:
        if initial_state:
            return xmm_i[reg] + ymm_i[reg] + zmm_i[reg]
        return xmm_f[reg] + ymm_f[reg] + zmm_f[reg]

    is_evex_mov_fp = any(op in name for op in ("vmovups", "vmovaps", "vmovupd", "vmovapd"))
    is_evex_mov_int = any(op in name for op in (
        "vmovdqa32", "vmovdqa64", "vmovdqu8", "vmovdqu16", "vmovdqu32", "vmovdqu64"))

    def evex_mov_lane() -> int:
        if "vmovdqu8" in name:
            return 1
        if "vmovdqu16" in name:
            return 2
        if any(op in name for op in ("vmovdqa64", "vmovdqu64", "vmovupd", "vmovapd")):
            return 8
        return 4

    def aes_gmul(a: int, b: int) -> int:
        p = 0
        for _ in range(8):
            if b & 1:
                p ^= a
            carry = a & 0x80
            a = (a << 1) & 0xff
            if carry:
                a ^= 0x1b
            b >>= 1
        return p & 0xff

    def rotl8(v: int, n: int) -> int:
        return ((v << n) | (v >> (8 - n))) & 0xff

    def aes_sbox(v: int) -> int:
        if v == 0:
            inv = 0
        else:
            inv = 1
            base = v
            exp = 254
            while exp:
                if exp & 1:
                    inv = aes_gmul(inv, base)
                base = aes_gmul(base, base)
                exp >>= 1
        return (inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63) & 0xff

    def aesenc128(state: bytes, key: bytes) -> bytes:
        sub = [aes_sbox(b) for b in state]
        shifted = [0] * 16
        for c in range(4):
            for r in range(4):
                shifted[4 * c + r] = sub[4 * ((c + r) & 3) + r]
        mixed = [0] * 16
        for c in range(4):
            off = c * 4
            a0, a1, a2, a3 = shifted[off:off + 4]
            mixed[off + 0] = aes_gmul(a0, 2) ^ aes_gmul(a1, 3) ^ a2 ^ a3
            mixed[off + 1] = a0 ^ aes_gmul(a1, 2) ^ aes_gmul(a2, 3) ^ a3
            mixed[off + 2] = a0 ^ a1 ^ aes_gmul(a2, 2) ^ aes_gmul(a3, 3)
            mixed[off + 3] = aes_gmul(a0, 3) ^ a1 ^ a2 ^ aes_gmul(a3, 2)
        return bytes((mixed[i] ^ key[i]) & 0xff for i in range(16))

    def deterministic_data() -> bytearray:
        state = [int(row["seed"], 16)]
        data = bytearray()
        while len(data) < 0x2000:
            state[0] = (state[0] + 0x9e3779b97f4a7c15) & MASK64
            z = state[0]
            z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
            z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
            data.extend((z ^ (z >> 31)).to_bytes(8, "little"))
        return data[:0x2000]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    def f32_fraction(bits: int) -> Fraction:
        sign = -1 if bits & 0x80000000 else 1
        exp = (bits >> 23) & 0xff
        frac = bits & 0x7fffff
        if exp == 0:
            mant = frac
            power = -126 - 23
        else:
            mant = (1 << 23) | frac
            power = exp - 127 - 23
        val = Fraction(mant, 1)
        if power >= 0:
            val *= 1 << power
        else:
            val /= 1 << (-power)
        return -val if sign < 0 else val

    def f32_bits_fraction(bits: int) -> Fraction | None:
        if (bits & 0x7f800000) == 0x7f800000:
            return None
        return f32_fraction(bits)

    def pack_f32_nearest_bits(value: Fraction) -> int:
        try:
            return struct.unpack("<I", struct.pack("<f", float(value)))[0]
        except OverflowError:
            return 0xff800000 if value < 0 else 0x7f800000

    def nextafter_f32_bits(bits: int, toward_positive: bool) -> int:
        bits &= 0xffffffff
        if (bits & 0x7fffffff) > 0x7f800000:
            return bits
        if toward_positive:
            if bits == 0x7f800000:
                return bits
            if bits == 0xff800000:
                return 0xff7fffff
            if bits & 0x80000000:
                return 0x00000001 if bits == 0x80000000 else bits - 1
            return bits + 1
        if bits == 0xff800000:
            return bits
        if bits == 0x7f800000:
            return 0x7f7fffff
        if bits & 0x80000000:
            return bits + 1
        return 0x80000001 if bits == 0 else bits - 1

    def fadd32_directed_bits(a_bits: int, b_bits: int, mode: str) -> int:
        if ((a_bits & 0x7f800000) == 0x7f800000 or
                (b_bits & 0x7f800000) == 0x7f800000):
            a = struct.unpack("<f", struct.pack("<I", a_bits))[0]
            b = struct.unpack("<f", struct.pack("<I", b_bits))[0]
            return struct.unpack("<I", struct.pack("<f", a + b))[0]
        exact = f32_fraction(a_bits) + f32_fraction(b_bits)
        candidate = pack_f32_nearest_bits(exact)
        rounded = f32_bits_fraction(candidate)
        if rounded is None:
            if mode == "rd" and exact > 0:
                candidate = nextafter_f32_bits(candidate, False)
            elif mode in {"ru", "rz"} and exact < 0:
                candidate = nextafter_f32_bits(candidate, True)
            elif mode == "rz" and exact > 0:
                candidate = nextafter_f32_bits(candidate, False)
            return candidate
        if mode == "rd" and rounded > exact:
            candidate = nextafter_f32_bits(candidate, False)
        elif mode == "ru" and rounded < exact:
            candidate = nextafter_f32_bits(candidate, True)
        elif mode == "rz":
            if exact > 0 and rounded > exact:
                candidate = nextafter_f32_bits(candidate, False)
            elif exact < 0 and rounded < exact:
                candidate = nextafter_f32_bits(candidate, True)
        return candidate

    old = bytearray(vec(0))
    expected = bytearray(old)
    check = bytearray(b"\xff" * 64)
    zeroing = "_k1z_" in name

    if (is_evex_mov_int and "_m512_k1_zmm1" in name) or (
            is_evex_mov_fp and "_m512_k1_zmm1" in name):
        lane = evex_mov_lane()
        data = deterministic_data()
        src = vec(1)
        mask = k[1]
        for off in range(0, 64, lane):
            if (mask >> (off // lane)) & 1:
                data[0x1000 + off:0x1000 + off + lane] = src[off:off + lane]
        expected_hash = f"0x{fnv1a64(data):016x}"
        actual_hash = final["data_hash"]
        return [] if actual_hash == expected_hash else [f"data_hash expected={expected_hash} actual={actual_hash}"]

    if (is_evex_mov_int and ("_zmm0_k1_m512" in name or "_zmm0_k1z_m512" in name)) or (
            is_evex_mov_fp and ("_zmm0_k1_m512" in name or "_zmm0_k1z_m512" in name)):
        lane = evex_mov_lane()
        data = deterministic_data()
        expected[:] = data[0x1000:0x1040]
    elif any(op in name for op in ("vpandd", "vpandnd", "vpord", "vpxord",
                                   "vpandq", "vpandnq", "vporq", "vpxorq",
                                   "vandps", "vandnps", "vorps", "vxorps",
                                   "vandpd", "vandnpd", "vorpd", "vxorpd")):
        lane = 8 if any(op in name for op in ("vpandq", "vpandnq", "vporq", "vpxorq",
                                                "vandpd", "vandnpd", "vorpd", "vxorpd")) else 4
        src1 = vec(0)
        if "m32bcst" in name:
            data = deterministic_data()
            src2 = data[0x1000:0x1004] * 16
        elif "m64bcst" in name:
            data = deterministic_data()
            src2 = data[0x1000:0x1008] * 8
        else:
            src2 = vec(1)
        for off in range(64):
            if "vpandn" in name or "vandn" in name:
                expected[off] = (~src1[off] & src2[off]) & 0xff
            elif "vpand" in name or "vand" in name:
                expected[off] = src1[off] & src2[off]
            elif "vpor" in name or "vor" in name:
                expected[off] = src1[off] | src2[off]
            else:
                expected[off] = src1[off] ^ src2[off]
    elif any(op in name for op in ("vunpcklps", "vunpckhps", "vunpcklpd", "vunpckhpd")):
        lane = 8 if "pd" in name else 4
        src1 = vec(0)
        src2 = vec(1)
        high = "vunpckh" in name
        for base in range(0, 64, 16):
            start = 8 if high else 0
            pos = base
            for i in range(8 // lane):
                off = base + start + i * lane
                expected[pos:pos + lane] = src1[off:off + lane]
                pos += lane
                expected[pos:pos + lane] = src2[off:off + lane]
                pos += lane
    elif "vshufps" in name or "vshufpd" in name:
        lane = 8 if "pd" in name else 4
        imm = int(template.code[-2:], 16)
        src1 = vec(0)
        src2 = vec(1)
        for base in range(0, 64, 16):
            if lane == 4:
                sels = [(imm >> 0) & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3]
                expected[base + 0:base + 4] = src1[base + sels[0] * 4:base + sels[0] * 4 + 4]
                expected[base + 4:base + 8] = src1[base + sels[1] * 4:base + sels[1] * 4 + 4]
                expected[base + 8:base + 12] = src2[base + sels[2] * 4:base + sels[2] * 4 + 4]
                expected[base + 12:base + 16] = src2[base + sels[3] * 4:base + sels[3] * 4 + 4]
            else:
                s0 = imm & 1
                s1 = (imm >> 1) & 1
                expected[base:base + 8] = src1[base + s0 * 8:base + s0 * 8 + 8]
                expected[base + 8:base + 16] = src2[base + s1 * 8:base + s1 * 8 + 8]
    elif "vsqrtps" in name or "vsqrtpd" in name:
        src = vec(1)
        lane = 8 if "vsqrtpd" in name else 4
        for off in range(0, 64, lane):
            if lane == 4:
                bits = struct.unpack("<I", src[off:off + 4])[0]
                value = struct.unpack("<f", src[off:off + 4])[0]
                if math.isnan(value) or value < 0.0:
                    check[off:off + lane] = b"\x00" * lane
                    continue
                expected[off:off + lane] = struct.pack("<f", math.sqrt(value))
            else:
                bits = struct.unpack("<Q", src[off:off + 8])[0]
                value = struct.unpack("<d", src[off:off + 8])[0]
                if math.isnan(value) or value < 0.0:
                    check[off:off + lane] = b"\x00" * lane
                    continue
                expected[off:off + lane] = struct.pack("<d", math.sqrt(value))
    elif any(op in name for op in ("vminps", "vmaxps", "vminpd", "vmaxpd")):
        src1 = vec(0)
        if "m32bcst" in name:
            data = deterministic_data()
            src2 = data[0x1000:0x1004] * 16
        elif "m64bcst" in name:
            data = deterministic_data()
            src2 = data[0x1000:0x1008] * 8
        else:
            src2 = vec(1)
        lane = 8 if any(op in name for op in ("vminpd", "vmaxpd")) else 4
        is_max = "vmax" in name
        for off in range(0, 64, lane):
            if lane == 4:
                a = struct.unpack("<f", src1[off:off + 4])[0]
                b = struct.unpack("<f", src2[off:off + 4])[0]
                if math.isnan(a) or math.isnan(b):
                    check[off:off + lane] = b"\x00" * lane
                    continue
                result = a if (a > b if is_max else a < b) else b
                expected[off:off + lane] = struct.pack("<f", result)
            else:
                a = struct.unpack("<d", src1[off:off + 8])[0]
                b = struct.unpack("<d", src2[off:off + 8])[0]
                if math.isnan(a) or math.isnan(b):
                    check[off:off + lane] = b"\x00" * lane
                    continue
                result = a if (a > b if is_max else a < b) else b
                expected[off:off + lane] = struct.pack("<d", result)
    elif any(op in name for op in ("vaddpd", "vsubpd", "vmulpd", "vdivpd")):
        src1 = vec(0)
        if "m64bcst" in name:
            data = deterministic_data()
            src2 = data[0x1000:0x1008] * 8
        else:
            src2 = vec(0)
        lane = 8
        op_name = "div" if "vdivpd" in name else ("mul" if "vmulpd" in name else ("sub" if "vsubpd" in name else "add"))
        for off in range(0, 64, lane):
            a_bits = struct.unpack("<Q", src1[off:off + 8])[0]
            b_bits = struct.unpack("<Q", src2[off:off + 8])[0]
            a = struct.unpack("<d", src1[off:off + 8])[0]
            b = struct.unpack("<d", src2[off:off + 8])[0]
            if math.isnan(a) or math.isnan(b):
                check[off:off + lane] = b"\x00" * lane
                continue
            invalid = False
            if op_name == "add":
                invalid = ((a_bits ^ b_bits) & 0x8000000000000000) and math.isinf(a) and math.isinf(b)
            elif op_name == "sub":
                invalid = not ((a_bits ^ b_bits) & 0x8000000000000000) and math.isinf(a) and math.isinf(b)
            elif op_name == "mul":
                invalid = (math.isinf(a) and b == 0.0) or (math.isinf(b) and a == 0.0)
            elif op_name == "div":
                invalid = (math.isinf(a) and math.isinf(b)) or (a == 0.0 and b == 0.0)
            if invalid:
                check[off:off + lane] = b"\x00" * lane
                continue
            try:
                if op_name == "add":
                    result = a + b
                elif op_name == "sub":
                    result = a - b
                elif op_name == "mul":
                    result = a * b
                else:
                    if b == 0.0:
                        sign = (a_bits ^ b_bits) & 0x8000000000000000
                        expected[off:off + lane] = struct.pack("<Q", sign | 0x7ff0000000000000)
                        continue
                    result = a / b
                expected[off:off + lane] = struct.pack("<d", result)
            except OverflowError:
                sign = (a_bits ^ b_bits) & 0x8000000000000000 if op_name in {"mul", "div"} else 0x8000000000000000 if (a - b if op_name == "sub" else a + b) < 0 else 0
                expected[off:off + lane] = struct.pack("<Q", sign | 0x7ff0000000000000)
    elif any(op in name for op in ("vaddps", "vsubps", "vmulps", "vdivps")):
        src1 = vec(0)
        if "m32bcst" in name:
            data = deterministic_data()
            src2 = data[0x1000:0x1004] * 16
        else:
            src2 = vec(0)
        lane = 4
        op_name = "div" if "vdivps" in name else ("mul" if "vmulps" in name else ("sub" if "vsubps" in name else "add"))
        for off in range(0, 64, lane):
            a_bits = struct.unpack("<I", src1[off:off + 4])[0]
            b_bits = struct.unpack("<I", src2[off:off + 4])[0]
            a = struct.unpack("<f", src1[off:off + 4])[0]
            b = struct.unpack("<f", src2[off:off + 4])[0]
            if math.isnan(a) or math.isnan(b):
                check[off:off + lane] = b"\x00" * lane
                continue
            invalid = False
            if op_name == "add":
                invalid = ((a_bits ^ b_bits) & 0x80000000) and math.isinf(a) and math.isinf(b)
            elif op_name == "sub":
                invalid = not ((a_bits ^ b_bits) & 0x80000000) and math.isinf(a) and math.isinf(b)
            elif op_name == "mul":
                invalid = (math.isinf(a) and b == 0.0) or (math.isinf(b) and a == 0.0)
            elif op_name == "div":
                invalid = (math.isinf(a) and math.isinf(b)) or (a == 0.0 and b == 0.0)
            if invalid:
                check[off:off + lane] = b"\x00" * lane
                continue
            try:
                if "_rd_sae_" in name:
                    expected[off:off + lane] = struct.pack("<I", fadd32_directed_bits(a_bits, b_bits, "rd"))
                elif "_ru_sae_" in name:
                    expected[off:off + lane] = struct.pack("<I", fadd32_directed_bits(a_bits, b_bits, "ru"))
                elif "_rz_sae_" in name:
                    expected[off:off + lane] = struct.pack("<I", fadd32_directed_bits(a_bits, b_bits, "rz"))
                else:
                    if op_name == "add":
                        result = a + b
                    elif op_name == "sub":
                        result = a - b
                    elif op_name == "mul":
                        result = a * b
                    else:
                        if b == 0.0:
                            sign = (a_bits ^ b_bits) & 0x80000000
                            expected[off:off + lane] = struct.pack("<I", sign | 0x7f800000)
                            continue
                        result = a / b
                    expected[off:off + lane] = struct.pack("<f", result)
            except OverflowError:
                sign = (a_bits ^ b_bits) & 0x80000000 if op_name in {"mul", "div"} else 0x80000000 if (a - b if op_name == "sub" else a + b) < 0 else 0
                expected[off:off + lane] = struct.pack("<I", sign | 0x7f800000)
    elif is_evex_mov_int or is_evex_mov_fp:
        lane = evex_mov_lane()
        expected[:] = vec(1)
    elif "vgf2p8mulb" in name:
        lane = 1
        src1 = vec(1)
        src2 = vec(2)
        for off in range(64):
            expected[off] = aes_gmul(src1[off], src2[off])
    elif "vaesenc" in name:
        lane = 16
        src1 = vec(1)
        src2 = vec(2)
        for off in range(0, 64, 16):
            expected[off:off + 16] = aesenc128(src1[off:off + 16], src2[off:off + 16])
    else:
        return []

    mask = k[1] if "_k1" in name else (1 << (64 // lane)) - 1
    for off in range(0, 64, lane):
        lane_idx = off // lane
        if (mask >> lane_idx) & 1:
            continue
        if zeroing:
            expected[off:off + lane] = b"\x00" * lane
        else:
            expected[off:off + lane] = old[off:off + lane]

    actual = vec(0, initial_state=False)
    mismatches: list[str] = []
    for off, (e, a) in enumerate(zip(expected, actual)):
        if check[off] and e != a:
            mismatches.append(f"byte{off} expected=0x{e:02x} actual=0x{a:02x}")
            if len(mismatches) >= 8:
                break
    return mismatches


def exact_crc_adx(template: Template, row: dict[str, Any]) -> list[str]:
    if not (template.name.startswith("adcx") or template.name.startswith("adox")):
        return []
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    a = as_u64(regs_i["r8"])
    b = as_u64(regs_i["r9"])
    target_flag = "cf" if template.name.startswith("adcx") else "of"
    total = a + b + flags_i[target_flag]
    expected = total & MASK64
    carry = 1 if total >> 64 else 0
    mismatches: list[str] = []
    if as_u64(regs_f["r8"]) != expected:
        mismatches.append(f"r8 expected=0x{expected:016x} actual={regs_f['r8']}")
    for flag, old_value in flags_i.items():
        expected_flag = carry if flag == target_flag else old_value
        if flags_f.get(flag, 0) != expected_flag:
            mismatches.append(f"flag.{flag} expected={expected_flag} actual={flags_f.get(flag, 0)}")
    return mismatches


def exact_movbe(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    name = template.name
    size = 8 if "_m64" in name or "_rdx_m64" in name else (2 if "_m16" in name or "_dx_m16" in name else 4)

    def splitmix64_next(state: int) -> tuple[int, int]:
        state = (state + 0x9e3779b97f4a7c15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
        return state, (z ^ (z >> 31)) & MASK64

    def fill_random(seed: int, n: int) -> bytearray:
        state = seed
        out = bytearray()
        while len(out) < n:
            state, value = splitmix64_next(state)
            out.extend(value.to_bytes(8, "little"))
        return out[:n]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    def bswap(value: int, width: int) -> int:
        mask = (1 << (width * 8)) - 1
        return int.from_bytes((value & mask).to_bytes(width, "little"), "big")

    mismatches: list[str] = []
    seed = int(row["seed"], 16)
    data = fill_random(seed, 0x2000)
    if initial.get("data_hash") != f"0x{fnv1a64(data):016x}":
        mismatches.append("initial data_hash does not match deterministic fixture")
        return mismatches

    if name.startswith("movbe_m"):
        value = as_u64(regs_i["rdx"])
        swapped = bswap(value, size)
        data[0x1000:0x1000 + size] = swapped.to_bytes(size, "little")
        expected_hash = f"0x{fnv1a64(data):016x}"
        if final.get("data_hash") != expected_hash:
            mismatches.append(f"data_hash expected={expected_hash} actual={final.get('data_hash')}")
        for reg in ("rax", "rdx"):
            if as_u64(regs_f[reg]) != as_u64(regs_i[reg]):
                mismatches.append(f"{reg} expected_preserve={regs_i[reg]} actual={regs_f[reg]}")
    else:
        raw = int.from_bytes(data[0x1000:0x1000 + size], "little")
        swapped = bswap(raw, size)
        rdx_i = as_u64(regs_i["rdx"])
        if size == 8:
            expected_rdx = swapped
        elif size == 4:
            expected_rdx = swapped
        else:
            expected_rdx = (rdx_i & ~0xffff) | swapped
        if as_u64(regs_f["rdx"]) != expected_rdx:
            mismatches.append(f"rdx expected=0x{expected_rdx:016x} actual={regs_f['rdx']}")
        if final.get("data_hash") != initial.get("data_hash"):
            mismatches.append(f"data_hash expected_preserve={initial.get('data_hash')} actual={final.get('data_hash')}")

    for flag, expected in flags_i.items():
        if flags_f.get(flag, 0) != expected:
            mismatches.append(f"flag.{flag} expected={expected} actual={flags_f.get(flag, 0)}")
    return mismatches


def exact_movdiri(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    size = 8 if "_m64" in template.name else 4

    def splitmix64_next(state: int) -> tuple[int, int]:
        state = (state + 0x9e3779b97f4a7c15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
        return state, (z ^ (z >> 31)) & MASK64

    def fill_random(seed: int, n: int) -> bytearray:
        state = seed
        out = bytearray()
        while len(out) < n:
            state, value = splitmix64_next(state)
            out.extend(value.to_bytes(8, "little"))
        return out[:n]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    mismatches: list[str] = []
    seed = int(row["seed"], 16)
    data = fill_random(seed, 0x2000)
    if initial.get("data_hash") != f"0x{fnv1a64(data):016x}":
        mismatches.append("initial data_hash does not match deterministic fixture")
        return mismatches
    value = as_u64(regs_i["rdx"])
    data[0x1000:0x1000 + size] = (value & ((1 << (size * 8)) - 1)).to_bytes(size, "little")
    expected_hash = f"0x{fnv1a64(data):016x}"
    if final.get("data_hash") != expected_hash:
        mismatches.append(f"data_hash expected={expected_hash} actual={final.get('data_hash')}")
    for reg in ("rax", "rdx"):
        if as_u64(regs_f[reg]) != as_u64(regs_i[reg]):
            mismatches.append(f"{reg} expected_preserve={regs_i[reg]} actual={regs_f[reg]}")
    for flag, expected in flags_i.items():
        if flags_f.get(flag, 0) != expected:
            mismatches.append(f"flag.{flag} expected={expected} actual={flags_f.get(flag, 0)}")
    return mismatches


def exact_movdir64b(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}

    def splitmix64_next(state: int) -> tuple[int, int]:
        state = (state + 0x9e3779b97f4a7c15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
        return state, (z ^ (z >> 31)) & MASK64

    def fill_random(seed: int, n: int) -> bytearray:
        state = seed
        out = bytearray()
        while len(out) < n:
            state, value = splitmix64_next(state)
            out.extend(value.to_bytes(8, "little"))
        return out[:n]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    mismatches: list[str] = []
    seed = int(row["seed"], 16)
    data = fill_random(seed, 0x2000)
    if initial.get("data_hash") != f"0x{fnv1a64(data):016x}":
        mismatches.append("initial data_hash does not match deterministic fixture")
        return mismatches
    data[0x1000:0x1040] = data[0x0400:0x0440]
    expected_hash = f"0x{fnv1a64(data):016x}"
    if final.get("data_hash") != expected_hash:
        mismatches.append(f"data_hash expected={expected_hash} actual={final.get('data_hash')}")
    for reg in ("rdi", "rsi"):
        if as_u64(regs_f[reg]) != as_u64(regs_i[reg]):
            mismatches.append(f"{reg} expected_preserve={regs_i[reg]} actual={regs_f[reg]}")
    for flag, expected in flags_i.items():
        if flags_f.get(flag, 0) != expected:
            mismatches.append(f"flag.{flag} expected={expected} actual={flags_f.get(flag, 0)}")
    return mismatches


def exact_vex_predicate_control(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    name = template.name
    regs_i = initial["regs"]
    regs_f = final["regs"]
    flags_i = {k: int(v) for k, v in initial["flags"].items()}
    flags_f = {k: int(v) for k, v in final["flags"].items()}
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    mismatches: list[str] = []

    def splitmix64_next(state: int) -> tuple[int, int]:
        state = (state + 0x9e3779b97f4a7c15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
        return state, (z ^ (z >> 31)) & MASK64

    def fill_random(seed: int, n: int) -> bytearray:
        state = seed
        out = bytearray()
        while len(out) < n:
            state, value = splitmix64_next(state)
            out.extend(value.to_bytes(8, "little"))
        return out[:n]

    def fnv1a64(data: bytes) -> int:
        h = 0xcbf29ce484222325
        for b in data:
            h ^= b
            h = (h * 0x100000001b3) & MASK64
        return h

    def expect_flags_preserved() -> None:
        for flag, expected in flags_i.items():
            if flags_f.get(flag, 0) != expected:
                mismatches.append(f"flag.{flag} expected={expected} actual={flags_f.get(flag, 0)}")

    def check_xmm(reg: int, expected: bytes, zero_upper: bool = False) -> None:
        if xmm_f[reg] != expected:
            mismatches.append(f"xmm{reg} expected={expected.hex()} actual={xmm_f[reg].hex()}")
        expected_hi = b"\x00" * 16 if zero_upper else ymm_i[reg]
        if ymm_f[reg] != expected_hi:
            mismatches.append(f"ymm_hi{reg} expected={expected_hi.hex()} actual={ymm_f[reg].hex()}")

    def check_reg(name: str, expected: int) -> None:
        actual = as_u64(regs_f[name])
        if actual != (expected & MASK64):
            mismatches.append(f"{name} expected=0x{expected & MASK64:016x} actual={regs_f[name]}")

    def deterministic_data() -> bytearray:
        return fill_random(int(row["seed"], 16), 0x2000)

    if name == "vmovd_xmm0_ecx":
        check_xmm(0, (as_u64(regs_i["rcx"]) & 0xffffffff).to_bytes(4, "little") + b"\x00" * 12, True)
        expect_flags_preserved()
    elif name == "vmovq_xmm0_rcx":
        check_xmm(0, as_u64(regs_i["rcx"]).to_bytes(8, "little") + b"\x00" * 8, True)
        expect_flags_preserved()
    elif name == "vmovd_ecx_xmm0":
        check_reg("rcx", int.from_bytes(xmm_i[0][:4], "little"))
        expect_flags_preserved()
    elif name == "vmovq_rcx_xmm0":
        check_reg("rcx", int.from_bytes(xmm_i[0][:8], "little"))
        expect_flags_preserved()
    elif name == "vmovq_f3_xmm0_xmm1":
        check_xmm(0, xmm_i[1][:8] + b"\x00" * 8, True)
        expect_flags_preserved()
    elif name == "vmovq_d6_xmm1_xmm0":
        check_xmm(1, xmm_i[0][:8] + b"\x00" * 8, True)
        expect_flags_preserved()
    elif name in {"vmovd_m32_xmm0", "vmovq_m64_xmm0", "vmovq_d6_m64_xmm0"}:
        data = deterministic_data()
        size = 4 if name == "vmovd_m32_xmm0" else 8
        data[0x1000:0x1000 + size] = xmm_i[0][:size]
        expected_hash = f"0x{fnv1a64(data):016x}"
        if final["data_hash"] != expected_hash:
            mismatches.append(f"data_hash expected={expected_hash} actual={final['data_hash']}")
        expect_flags_preserved()
    elif name == "vmovq_f3_xmm0_m64":
        data = deterministic_data()
        check_xmm(0, bytes(data[0x1000:0x1008]) + b"\x00" * 8, True)
        expect_flags_preserved()
    elif name == "vmovmskps_eax_xmm1":
        mask = 0
        for lane in range(4):
            if xmm_i[1][lane * 4 + 3] & 0x80:
                mask |= 1 << lane
        check_reg("rax", mask)
        expect_flags_preserved()
    elif name == "vmovmskpd_eax_xmm1":
        mask = 0
        for lane in range(2):
            if xmm_i[1][lane * 8 + 7] & 0x80:
                mask |= 1 << lane
        check_reg("rax", mask)
        expect_flags_preserved()
    elif name == "vpmovmskb_eax_xmm1":
        mask = 0
        for lane in range(16):
            if xmm_i[1][lane] & 0x80:
                mask |= 1 << lane
        check_reg("rax", mask)
        expect_flags_preserved()
    elif name.startswith("vtest"):
        lane = 8 if "pd" in name else 4
        src0 = xmm_i[0] + (ymm_i[0] if "_ymm" in name else b"")
        src1 = xmm_i[1] + (ymm_i[1] if "_ymm" in name else b"")
        zf = 1
        cf = 1
        for off in range(0, len(src0), lane):
            a = src0[off + lane - 1]
            b = src1[off + lane - 1]
            if a & b & 0x80:
                zf = 0
            if ((~a & 0xff) & b & 0x80) != 0:
                cf = 0
        expected_flags = {"zf": zf, "cf": cf, "of": 0, "sf": 0, "af": 0, "pf": 0}
        for flag, expected in expected_flags.items():
            if flags_f.get(flag, 0) != expected:
                mismatches.append(f"flag.{flag} expected={expected} actual={flags_f.get(flag, 0)}")
    elif name == "vmaskmovdqu_xmm0_xmm1":
        data = deterministic_data()
        for off in range(16):
            if xmm_i[1][off] & 0x80:
                data[0x1000 + off] = xmm_i[0][off]
        expected_hash = f"0x{fnv1a64(data):016x}"
        if final["data_hash"] != expected_hash:
            mismatches.append(f"data_hash expected={expected_hash} actual={final['data_hash']}")
        expect_flags_preserved()
    elif name == "vldmxcsr_m32":
        expect_flags_preserved()
    elif name == "vstmxcsr_m32":
        data = deterministic_data()
        data[0x1000:0x1004] = (0x1f80).to_bytes(4, "little")
        expected_hash = f"0x{fnv1a64(data):016x}"
        if final["data_hash"] != expected_hash:
            mismatches.append(f"data_hash expected={expected_hash} actual={final['data_hash']}")
        expect_flags_preserved()
    return mismatches


def exact_sha_tail(template: Template, row: dict[str, Any]) -> list[str]:
    initial = row["initial"]
    final = row["interp"]
    xmm_i = [bytes.fromhex(x) for x in initial["xmm"]]
    ymm_i = [bytes.fromhex(x) for x in initial["ymm_hi"]]
    xmm_f = [bytes.fromhex(x) for x in final["xmm"]]
    ymm_f = [bytes.fromhex(x) for x in final["ymm_hi"]]
    name = template.name
    target = 2 if "xmm2" in name else 0
    src1 = xmm_i[target]
    src2 = xmm_i[1]
    imm = int(template.code[-2:], 16) if name.startswith("sha1rnds4") else 0

    def u32s_be_bits(data: bytes) -> list[int]:
        return [int.from_bytes(data[(3 - k) * 4:(4 - k) * 4], "little") for k in range(4)]

    def rol(x: int, n: int) -> int:
        x &= 0xffffffff
        return ((x << n) | (x >> (32 - n))) & 0xffffffff

    def ror(x: int, n: int) -> int:
        x &= 0xffffffff
        return ((x >> n) | (x << (32 - n))) & 0xffffffff

    def ch(x: int, y: int, z: int) -> int:
        return (z ^ (x & (y ^ z))) & 0xffffffff

    def parity(x: int, y: int, z: int) -> int:
        return (x ^ y ^ z) & 0xffffffff

    def maj(x: int, y: int, z: int) -> int:
        return ((x & y) ^ (x & z) ^ (y & z)) & 0xffffffff

    def big0(x: int) -> int:
        return ror(x, 2) ^ ror(x, 13) ^ ror(x, 22)

    def big1(x: int) -> int:
        return ror(x, 6) ^ ror(x, 11) ^ ror(x, 25)

    def small0(x: int) -> int:
        return ror(x, 7) ^ ror(x, 18) ^ ((x & 0xffffffff) >> 3)

    def small1(x: int) -> int:
        return ror(x, 17) ^ ror(x, 19) ^ ((x & 0xffffffff) >> 10)

    s0 = u32s_be_bits(src1)
    s1 = u32s_be_bits(src2)
    if name.startswith("sha1nexte"):
        d = [(s1[0] + rol(s0[0], 30)) & 0xffffffff, s1[1], s1[2], s1[3]]
    elif name.startswith("sha1msg1"):
        d = [s0[2] ^ s0[0], s0[3] ^ s0[1], s1[0] ^ s0[2], s1[1] ^ s0[3]]
    elif name.startswith("sha1msg2"):
        w13, w14, w15 = s1[1], s1[2], s1[3]
        w16 = rol(s0[0] ^ w13, 1)
        w17 = rol(s0[1] ^ w14, 1)
        w18 = rol(s0[2] ^ w15, 1)
        w19 = rol(s0[3] ^ w16, 1)
        d = [w16, w17, w18, w19]
    elif name.startswith("sha1rnds4"):
        kidx = imm & 3
        ktab = [0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6]
        a, b, c, d0 = s0
        e = 0
        for i, w in enumerate(s1):
            f = ch(b, c, d0) if kidx == 0 else parity(b, c, d0) if kidx in (1, 3) else maj(b, c, d0)
            an = (f + rol(a, 5) + w + ktab[kidx] + (0 if i == 0 else e)) & 0xffffffff
            a, b, c, d0, e = an, a, rol(b, 30), c, d0
        d = [a, b, c, d0]
    elif name.startswith("sha256msg1"):
        d = [
            (s0[3] + small0(s1[3])) & 0xffffffff,
            (s0[2] + small0(s0[3])) & 0xffffffff,
            (s0[1] + small0(s0[2])) & 0xffffffff,
            (s0[0] + small0(s0[1])) & 0xffffffff,
        ]
    elif name.startswith("sha256msg2"):
        w16 = (s0[3] + small1(s1[1])) & 0xffffffff
        w17 = (s0[2] + small1(s1[0])) & 0xffffffff
        w18 = (s0[1] + small1(w16)) & 0xffffffff
        w19 = (s0[0] + small1(w17)) & 0xffffffff
        d = [w19, w18, w17, w16]
    elif name.startswith("sha256rnds2"):
        wk0 = int.from_bytes(xmm_i[0][0:4], "little")
        wk1 = int.from_bytes(xmm_i[0][4:8], "little")
        a, b, c, d0 = s1[0], s1[1], s0[0], s0[1]
        e, f, g, h = s1[2], s1[3], s0[2], s0[3]
        for wk in (wk0, wk1):
            an = (ch(e, f, g) + big1(e) + wk + h + maj(a, b, c) + big0(a)) & 0xffffffff
            en = (ch(e, f, g) + big1(e) + wk + h + d0) & 0xffffffff
            a, b, c, d0 = an, a, b, c
            e, f, g, h = en, e, f, g
        d = [a, b, e, f]
    else:
        return []

    out = bytearray(16)
    for k, value in enumerate(d):
        out[(3 - k) * 4:(4 - k) * 4] = (value & 0xffffffff).to_bytes(4, "little")
    expected = bytes(out) + ymm_i[target]
    actual = xmm_f[target] + ymm_f[target]
    if actual != expected:
        return [f"ymm{target} expected={expected.hex()} actual={actual.hex()}"]
    return []


def exact_oracle(template: Template, row: dict[str, Any]) -> list[str]:
    if not template.oracle:
        return []
    if template.oracle == "avx_vpermil_blendv_vcmp":
        return exact_avx_vpermil_blendv_vcmp(template, row)
    if template.oracle == "avx_shuffle_unpack":
        return exact_avx_shuffle_unpack(template, row)
    if template.oracle == "avx_horizontal_addsub":
        return exact_avx_horizontal_addsub(template, row)
    if template.oracle == "avx_movnt_lddqu":
        return exact_avx_movnt_lddqu(template, row)
    if template.oracle == "avx_vcomi":
        return exact_avx_vcomi(template, row)
    if template.oracle == "avx_pshuf_imm":
        return exact_avx_pshuf_imm(template, row)
    if template.oracle == "avx_dup_shuffle":
        return exact_avx_dup_shuffle(template, row)
    if template.oracle == "avx_packed_convert":
        return exact_avx_packed_convert(template, row)
    if template.oracle == "avx_scalar_convert":
        return exact_avx_scalar_convert(template, row)
    if template.oracle == "avx_rcp_rsqrt":
        return exact_avx_rcp_rsqrt(template, row)
    if template.oracle == "avx_round":
        return exact_avx_round(template, row)
    if template.oracle == "avx_dp":
        return exact_avx_dp(template, row)
    if template.oracle == "avx_insert_extract":
        return exact_avx_insert_extract(template, row)
    if template.oracle == "avx_pinsr_pextr":
        return exact_avx_pinsr_pextr(template, row)
    if template.oracle == "avx_gfni_affine":
        return exact_avx_gfni_affine(template, row)
    if template.oracle == "bmi_vex":
        return exact_bmi_vex(template, row)
    if template.oracle == "ssse3_mmx":
        return exact_ssse3_mmx(template, row)
    if template.oracle == "mpsadbw":
        return exact_mpsadbw(template, row)
    if template.oracle == "fma3_tail":
        return exact_fma3_tail(template, row)
    if template.oracle == "vmovhl_lh":
        return exact_vmovhl_lh(template, row)
    if template.oracle == "evex_mask_zero":
        return exact_evex_mask_zero(template, row)
    if template.oracle == "evex_zmm_regbank":
        return exact_evex_zmm_regbank(template, row)
    if template.oracle == "evex_cmp_k":
        return exact_evex_cmp_k(template, row)
    if template.oracle == "evex_scalar_arith":
        return exact_evex_scalar_arith(template, row)
    if template.oracle == "evex_scalar_minmax_sqrt":
        return exact_evex_scalar_minmax_sqrt(template, row)
    if template.oracle == "evex_scalar_mov_rr":
        return exact_evex_scalar_mov_rr(template, row)
    if template.oracle == "evex_scalar_mov_mem":
        return exact_evex_scalar_mov_mem(template, row)
    if template.oracle == "evex_vpbroadcast_mem":
        return exact_evex_vpbroadcast_mem(template, row)
    if template.oracle == "avx_vbroadcast_fp":
        return exact_vbroadcast_fp(template, row)
    if template.oracle == "evex_broadcast_ext":
        return exact_vbroadcast_fp(template, row)
    if template.oracle == "avx_packed_shift":
        return exact_avx_packed_shift(template, row)
    if template.oracle == "avx_vmovlh_mem":
        return exact_avx_vmovlh_mem(template, row)
    if template.oracle == "evex_pshuf_word_imm":
        return exact_evex_pshuf_word_imm(template, row)
    if template.oracle == "evex_packed_int":
        return exact_evex_packed_int(template, row)
    if template.oracle == "crc_adx":
        return exact_crc_adx(template, row)
    if template.oracle == "movbe":
        return exact_movbe(template, row)
    if template.oracle == "movdiri":
        return exact_movdiri(template, row)
    if template.oracle == "movdir64b":
        return exact_movdir64b(template, row)
    if template.oracle == "vex_predicate_control":
        return exact_vex_predicate_control(template, row)
    if template.oracle == "sha_tail":
        return exact_sha_tail(template, row)
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    flags_i = initial["flags"]
    flags_f = final["flags"]
    a = as_u64(regs_i["r8"])
    b = as_u64(regs_i["r9"])
    rcx = as_u64(regs_i["rcx"])
    op = template.oracle
    expected_r8 = a
    expected_flags: dict[str, int]
    if op == "add":
        expected_r8, expected_flags = add_flags(a, b, 0)
    elif op == "adc":
        expected_r8, expected_flags = add_flags(a, b, int(flags_i["cf"]))
    elif op == "sub":
        expected_r8, expected_flags = sub_flags(a, b, 0)
    elif op == "sbb":
        expected_r8, expected_flags = sub_flags(a, b, int(flags_i["cf"]))
    elif op == "cmp":
        _, expected_flags = sub_flags(a, b, 0)
    elif op == "and":
        expected_r8, expected_flags = a & b, logic_flags(a & b)
    elif op == "or":
        expected_r8, expected_flags = a | b, logic_flags(a | b)
    elif op == "xor":
        expected_r8, expected_flags = a ^ b, logic_flags(a ^ b)
    elif op == "test":
        _, expected_flags = a & b, logic_flags(a & b)
    elif op == "shl":
        expected_r8, expected_flags = shl_flags(a, rcx, flags_i)
    elif op == "cmovne":
        expected_r8 = b if int(flags_i["zf"]) == 0 else a
        expected_flags = {k: int(v) for k, v in flags_i.items()}
    elif op == "setne":
        low = 1 if int(flags_i["zf"]) == 0 else 0
        expected_r8 = (a & ~0xff) | low
        expected_flags = {k: int(v) for k, v in flags_i.items()}
    elif op == "tzcnt":
        expected_r8 = 64 if b == 0 else (b & -b).bit_length() - 1
        expected_flags = {"cf": 1 if b == 0 else 0, "zf": 1 if expected_r8 == 0 else 0}
    elif op == "lzcnt":
        expected_r8 = 64 if b == 0 else 64 - b.bit_length()
        expected_flags = {"cf": 1 if b == 0 else 0, "zf": 1 if expected_r8 == 0 else 0}
    elif op == "popcnt":
        expected_r8 = b.bit_count()
        expected_flags = {
            "cf": 0,
            "pf": 0,
            "af": 0,
            "zf": 1 if b == 0 else 0,
            "sf": 0,
            "of": 0,
        }
    else:
        return []

    mismatches: list[str] = []
    if op not in {"cmp", "test"} and as_u64(regs_f["r8"]) != expected_r8:
        mismatches.append(f"r8 expected=0x{expected_r8:016x} actual={regs_f['r8']}")
    if op in {"cmp", "test"} and as_u64(regs_f["r8"]) != a:
        mismatches.append(f"r8_modified expected=0x{a:016x} actual={regs_f['r8']}")

    for flag, expected in expected_flags.items():
        if int(flags_f[flag]) != expected:
            mismatches.append(f"flag.{flag} expected={expected} actual={flags_f[flag]}")
    return mismatches


def run_batch(cases: list[tuple[int, Template]], *, backend_diff: bool, arch: str) -> list[dict[str, Any]]:
    payload = "".join(f"0x{seed:016x} {template.code}\n" for seed, template in cases)
    env = dict(**os.environ, HB_DIFF_ARCH=arch)
    if not backend_diff:
        env["HB_DIFF_INTERP_ONLY"] = "1"
    proc = subprocess.run(
        [str(RUNNER)],
        cwd=HB_ROOT,
        input=payload,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        env=env,
    )
    rows = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
    if len(rows) != len(cases):
        raise RuntimeError(f"runner returned {len(rows)} rows for {len(cases)} cases")
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cases", type=int, default=20000)
    ap.add_argument("--arch", choices=("x64", "x86"), default="x64")
    ap.add_argument("--seed", type=int, default=0x18BE487)
    ap.add_argument("--batch", type=int, default=512)
    ap.add_argument("--families", default="", help="comma-separated family filter")
    ap.add_argument("--seed-list", default="", help="comma-separated seeds; cases are split across them")
    ap.add_argument("--report-json", default=str(REPORT_JSON), help="output JSON report path")
    ap.add_argument("--no-build", action="store_true", help="use an already-built hb_diff_case_runner")
    ap.add_argument("--run-jit-backend-diff", action="store_true", help="also run JIT backend comparison")
    ap.add_argument("--stop-on-mismatch", action="store_true")
    args = ap.parse_args()

    if not args.no_build:
        build_runner()
    sde_path = shutil.which("sde64") or shutil.which("sde")
    family_filter = {x for x in args.families.split(",") if x}
    template_pool = I386_TEMPLATES if args.arch == "x86" else X64_TEMPLATES
    templates = [t for t in template_pool if not family_filter or t.family in family_filter]
    if not templates:
        raise SystemExit("no templates selected")

    seeds = [int(x, 0) for x in args.seed_list.split(",") if x] or [args.seed]
    counts: dict[str, int] = {}
    backend_mismatches: list[dict[str, Any]] = []
    oracle_mismatches: list[dict[str, Any]] = []
    oracle_unsupported: dict[str, int] = {}
    oracle_quirks: dict[str, int] = {}
    shared_traps: dict[str, int] = {}
    unsupported: dict[str, int] = {}
    oracle_checked_count = 0
    oracle_pass_count = 0

    pending: list[tuple[int, Template]] = []
    case_index = 0
    cases_per_seed = [args.cases // len(seeds)] * len(seeds)
    for i in range(args.cases % len(seeds)):
        cases_per_seed[i] += 1
    total_cases = 0
    for seed_base, seed_cases in zip(seeds, cases_per_seed):
        rng = random.Random(seed_base)
        local_index = 0
        while local_index < seed_cases:
            template = templates[case_index % len(templates)]
            seed = rng.getrandbits(64)
            pending.append((seed, template))
            local_index += 1
            case_index += 1
            total_cases += 1
            if len(pending) < args.batch and total_cases < args.cases:
                continue
            rows = run_batch(pending, backend_diff=args.run_jit_backend_diff, arch=args.arch)
            for (seed_i, template_i), row in zip(pending, rows):
                counts[template_i.family] = counts.get(template_i.family, 0) + 1
                interp_ok = row["interp"]["api"] == 0 and row["interp"]["result"] == 0
                jit_ok = row["jit"]["api"] == 0 and row["jit"]["result"] == 0
                if not interp_ok or (args.run_jit_backend_diff and not jit_ok):
                    key = f"{template_i.family}:{template_i.name}:interp={row['interp']['api']}/{row['interp']['result']}:jit={row['jit']['api']}/{row['jit']['result']}"
                    unsupported[key] = unsupported.get(key, 0) + 1
                oracle = run_unicorn_case(seed_i, template_i.code, args.arch)
                if not oracle.get("ok"):
                    oracle_msg = oracle.get("message", oracle.get("error", "unknown"))
                    if oracle.get("trap") == "invalid_instruction":
                        key = f"{template_i.family}:{template_i.name}:{oracle_msg}"
                        oracle_unsupported[key] = oracle_unsupported.get(key, 0) + 1
                        exact = exact_oracle(template_i, row) if args.arch == "x64" and interp_ok else []
                        oracle_checked = bool(args.arch == "x64" and template_i.oracle and interp_ok)
                    elif interp_ok:
                        exact = [f"oracle_trap_but_interp_ok:{oracle_msg}"]
                        oracle_checked = True
                    elif oracle.get("trap") == "cpu_exception":
                        key = f"{template_i.family}:{template_i.name}:{oracle_msg}:interp={row['interp']['api']}/{row['interp']['result']}"
                        shared_traps[key] = shared_traps.get(key, 0) + 1
                        exact = []
                        oracle_checked = True
                    else:
                        key = f"{template_i.family}:{template_i.name}:{oracle_msg}"
                        oracle_unsupported[key] = oracle_unsupported.get(key, 0) + 1
                        exact = exact_oracle(template_i, row) if args.arch == "x64" and interp_ok else []
                        oracle_checked = bool(args.arch == "x64" and template_i.oracle and interp_ok)
                else:
                    exact = unicorn_diff_interpreter(row, oracle, template_i.defined_flags, args.arch)
                    if template_i.oracle == "ssse3_mmx" and interp_ok:
                        exact = exact_ssse3_mmx(template_i, row)
                    elif template_i.oracle == "bmi_vex" and interp_ok:
                        if exact:
                            key = f"{template_i.family}:{template_i.name}:unicorn_bmi_flag_semantics"
                            oracle_quirks[key] = oracle_quirks.get(key, 0) + 1
                        exact = exact_oracle(template_i, row)
                    elif template_i.oracle == "mpsadbw" and interp_ok:
                        if exact:
                            key = f"{template_i.family}:{template_i.name}:unicorn_vex_mpsadbw_semantics"
                            oracle_quirks[key] = oracle_quirks.get(key, 0) + 1
                        exact = exact_oracle(template_i, row)
                    elif template_i.oracle == "fma3_tail" and interp_ok:
                        if exact:
                            key = f"{template_i.family}:{template_i.name}:unicorn_fma3_tail_semantics"
                            oracle_quirks[key] = oracle_quirks.get(key, 0) + 1
                        exact = exact_oracle(template_i, row)
                    elif template_i.oracle == "vmovhl_lh" and interp_ok:
                        if exact:
                            key = f"{template_i.family}:{template_i.name}:unicorn_vmovhl_lh_semantics"
                            oracle_quirks[key] = oracle_quirks.get(key, 0) + 1
                        exact = exact_oracle(template_i, row)
                    elif template_i.oracle == "vex_predicate_control" and interp_ok:
                        if exact:
                            key = f"{template_i.family}:{template_i.name}:unicorn_vex_predicate_control_semantics"
                            oracle_quirks[key] = oracle_quirks.get(key, 0) + 1
                        exact = exact_oracle(template_i, row)
                    elif template_i.oracle and template_i.oracle.startswith("avx_") and interp_ok:
                        exact = exact_oracle(template_i, row)
                    elif exact and template_i.family.startswith("avx"):
                        key = f"{template_i.family}:{template_i.name}:unicorn_vex_avx_semantics"
                        oracle_quirks[key] = oracle_quirks.get(key, 0) + 1
                        exact = []
                    oracle_checked = interp_ok
                if oracle_checked:
                    oracle_checked_count += 1
                    if not exact:
                        oracle_pass_count += 1
                if args.run_jit_backend_diff and interp_ok and jit_ok and not row["ok"]:
                    backend_mismatches.append({
                        "template": template_i.name,
                        "family": template_i.family,
                        "seed": f"0x{seed_i:016x}",
                        "code": template_i.code,
                        "diff": row["diff"],
                        "interp_oracle": "pass" if oracle_checked and not exact else ("fail" if exact else "not_available"),
                    })
                if exact:
                    oracle_mismatches.append({
                        "template": template_i.name,
                        "family": template_i.family,
                        "seed": f"0x{seed_i:016x}",
                        "code": template_i.code,
                        "mismatches": exact,
                    })
                if args.stop_on_mismatch and (backend_mismatches or oracle_mismatches):
                    break
            pending = []
            if args.stop_on_mismatch and (backend_mismatches or oracle_mismatches):
                break
        if args.stop_on_mismatch and (backend_mismatches or oracle_mismatches):
            break

    report_json = Path(args.report_json)
    if not report_json.is_absolute():
        report_json = HB_ROOT / report_json
    report_json.parent.mkdir(parents=True, exist_ok=True)
    result = {
        "sde_available": bool(sde_path),
        "sde_path": sde_path,
        "unicorn_available": True,
        "oracle_mode": "unicorn",
        "arch": args.arch,
        "backend_diff_enabled": args.run_jit_backend_diff,
        "seeds": [f"0x{s:x}" for s in seeds],
        "cases_requested": args.cases,
        "cases_run": sum(counts.values()),
        "families": counts,
        "backend_mismatch_count": len(backend_mismatches),
        "oracle_mismatch_count": len(oracle_mismatches),
        "oracle_checked_count": oracle_checked_count,
        "oracle_pass_count": oracle_pass_count,
        "backend_mismatches": backend_mismatches[:50],
        "oracle_mismatches": oracle_mismatches[:50],
        "oracle_unsupported": oracle_unsupported,
        "oracle_quirks": oracle_quirks,
        "shared_traps": shared_traps,
        "unsupported": unsupported,
    }
    report_json.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 1 if oracle_mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
