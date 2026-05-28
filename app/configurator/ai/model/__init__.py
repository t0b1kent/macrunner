"""MacRunner AI Configurator — ML Prediction Model (Phase A.5).

Rule-based MVP for profile prediction from PE feature vectors.
Architecture: rule-based MVP → XGBoost V2 → optional neural net (deferred).

Usage:
    predictor = ProfilePredictor(strategy="rule-based")
    overrides = predictor.predict(pe_features)
"""

from __future__ import annotations

import copy
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping


@dataclass(frozen=True)
class Prediction:
    """A single predicted profile override with confidence and reasoning."""

    field: str  # dot-path, e.g. "graphics.api"
    value: Any
    confidence: float  # 0.0–1.0
    reason: str


@dataclass
class PredictionResult:
    """Complete prediction result for a PE binary."""

    predicted_category: str
    overrides: list[Prediction]
    confidence: float  # average field-level confidence
    strategy: str


def _pe_features_to_flat(pe_features: Mapping[str, Any]) -> dict[str, Any]:
    """Bridge: map hierarchical PE analyzer features → flat XGBoost input.

    The 91-dim PE analyzer uses namespaced keys; the XGBoost trainer
    expects the compact flat keys from _FEATURE_ORDER.
    """
    flat: dict[str, Any] = {}

    # Arch one-hot
    flat["arch_x86_64"] = int(pe_features.get("pe_header.arch_x86_64", 0))
    flat["arch_arm64"] = int(pe_features.get("pe_header.arch_arm64", 0))
    flat["arch_i386"] = int(pe_features.get("pe_header.arch_i386", 0))
    flat["arch_arm64ec"] = int(pe_features.get("pe_header.arch_arm64ec", 0))
    flat["arch_arm64x"] = int(pe_features.get("pe_header.arch_arm64x", 0))

    # Subsystem (simplified)
    sub = pe_features.get("pe_header.subsystem_gui", 0)
    flat["subsystem_windows_gui"] = int(sub)
    flat["subsystem_windows_cui"] = int(pe_features.get("pe_header.subsystem_console", 0))
    flat["subsystem_efi_app"] = int(
        pe_features.get("pe_header.subsystem_other", 0) if pe_features.get("pe_header.subsystem_native", 0) == 0 and sub == 0 else 0
    )

    # Graphics imports
    flat["import_d3d12"] = int(pe_features.get("imports.has_d3d12", 0))
    flat["import_d3d11"] = int(pe_features.get("imports.has_d3d11", 0))
    flat["import_d3d9"] = int(pe_features.get("imports.has_d3d9", 0))
    flat["import_opengl32"] = int(pe_features.get("imports.has_opengl32", 0))
    flat["import_vulkan"] = int(pe_features.get("imports.has_vulkan", 0))

    # Core imports
    flat["import_kernel32"] = int(pe_features.get("imports.has_kernel32", 0))
    flat["import_user32"] = int(pe_features.get("imports.has_user32", 0))
    flat["import_gdi32"] = int(pe_features.get("imports.has_gdi32", 0))
    flat["import_shell32"] = int(pe_features.get("imports.has_shell32", 0))
    flat["import_advapi32"] = int(pe_features.get("imports.has_advapi32", 0))
    flat["import_ws2_32"] = int(pe_features.get("imports.has_ws2_32", 0))

    # Heuristics
    any_ac = any(
        pe_features.get(f"anticheat.{k}", 0)
        for k in ("battleye", "eac", "vanguard", "punkbuster", "denuvo", "vmprotect", "themida", "widevine")
    )
    flat["has_anticheat_signature"] = int(any_ac)
    flat["is_dotnet"] = int(pe_features.get("heuristics.is_dotnet", 0))
    flat["is_installer"] = int(pe_features.get("heuristics.is_installer", 0))
    flat["is_packed"] = int(pe_features.get("heuristics.is_packed", 0))
    flat["framework_unity"] = int(pe_features.get("heuristics.framework_unity", 0))
    flat["framework_unreal"] = int(pe_features.get("heuristics.framework_unreal", 0))
    flat["framework_wpf"] = int(pe_features.get("heuristics.framework_wpf", 0))
    flat["framework_winforms"] = int(
        pe_features.get("heuristics.is_dotnet", 0)
    )  # proxy: .NET → likely WinForms/WPF
    flat["framework_electron"] = int(pe_features.get("heuristics.framework_electron", 0))
    flat["framework_qt"] = int(pe_features.get("heuristics.framework_qt", 0))
    flat["framework_gtk"] = int(pe_features.get("heuristics.framework_gtk", 0))
    flat["is_text_editor"] = int(pe_features.get("heuristics.is_text_editor", 0))

    # PE stats
    flat["section_count"] = int(pe_features.get("pe_header.number_of_sections", 0))
    flat["import_count"] = int(pe_features.get("imports.total_imports", 0))
    flat["export_count"] = int(pe_features.get("exports.count", 0))
    flat["has_resources"] = int(pe_features.get("sections.rsrc_present", 0))
    flat["has_signature"] = int(pe_features.get("signatures.has_signature", 0))
    flat["has_debug"] = int(pe_features.get("heuristics.has_debug_dir", 0))
    flat["entry_point_present"] = int(pe_features.get("pe_header.entry_point_norm", 0) > 0)
    flat["large_address_aware"] = int(
        (int(pe_features.get("pe_header.dll_characteristics", 0)) & 0x0020) != 0
    )

    return flat


_DEFAULT_MODEL_DIR: Path = Path(__file__).resolve().parents[4] / "models" / "xgboost_profile_v1_3"


class ProfilePredictor:
    """Predict optimal profile overrides from PE analysis features.

    Strategies:
    - "rule-based": hard-coded heuristics (MVP, no external deps)
    - "xgboost": trained XGBoost model (V2, requires xgboost package)
    """

    def __init__(self, strategy: str = "rule-based", model_dir: Path | str | None = None) -> None:
        if strategy not in {"rule-based", "xgboost"}:
            raise ValueError(f"unknown strategy: {strategy}")
        self.strategy = strategy
        self._model: Any = None
        self._model_loaded = False
        self._model_dir = Path(model_dir) if model_dir else _DEFAULT_MODEL_DIR
        if strategy == "xgboost":
            self._load_xgboost_model()

    def _load_xgboost_model(self) -> None:
        import importlib.util

        if importlib.util.find_spec("xgboost") is None:  # type: ignore[arg-type]
            raise ImportError(
                "xgboost strategy requires the xgboost package; "
                "install with: pip install xgboost"
            )
        try:
            from app.configurator.ai.model.trainer import XGBoostTrainer

            self._model = XGBoostTrainer.load(self._model_dir)
            self._model_loaded = True
        except (FileNotFoundError, OSError, ValueError):
            # Model not trained/persisted yet — silently keep None so stub can fall back
            self._model = None
            self._model_loaded = False

    # -----------------------------------------------------------------------
    # Public API
    # -----------------------------------------------------------------------

    def predict(self, pe_features: Mapping[str, Any]) -> PredictionResult:
        """Predict profile overrides from PE feature dict."""
        if self.strategy == "rule-based":
            return _predict_rule_based(pe_features)
        return _predict_xgboost(pe_features, self._model)

    def predict_profile_dict(self, pe_features: Mapping[str, Any]) -> dict[str, Any]:
        """Predict and return as a flat profile overrides dict."""
        result = self.predict(pe_features)
        out: dict[str, Any] = {}
        for pred in result.overrides:
            parts = pred.field.split(".")
            node = out
            for part in parts[:-1]:
                if part not in node:
                    node[part] = {}
                node = node[part]
            node[parts[-1]] = pred.value
        return out


# ---------------------------------------------------------------------------
# Rule-based MVP
# ---------------------------------------------------------------------------

def _predict_rule_based(features: Mapping[str, Any]) -> PredictionResult:
    """Hard-coded heuristics mapping PE features → profile overrides.

    Rules are ordered by specificity: most-specific first, least-specific last.
    """
    overrides: list[Prediction] = []

    # --- Arch / lane selection ---
    arch = _detect_arch(features)
    if arch == "arm64":
        overrides.append(
            Prediction(
                field="default_lane",
                value="arm64-native",
                confidence=0.90,
                reason="ARM64 PE → native lane",
            )
        )
    elif arch == "x86_64":
        overrides.append(
            Prediction(
                field="default_lane",
                value="x86_64-rosetta",
                confidence=0.75,
                reason="x86_64 PE → Rosetta lane (safer default)",
            )
        )
        # But if HyperBridge is available and no risky patterns, prefer HB
        if _bool(features.get("import_kernel32")) and not _bool(
            features.get("has_anticheat_signature")
        ):
            overrides.append(
                Prediction(
                    field="default_lane",
                    value="arm64-hyperbridge",
                    confidence=0.60,
                    reason="x86_64 PE with safe imports → HyperBridge for better performance",
                )
            )
    elif arch == "i386":
        overrides.append(
            Prediction(
                field="default_lane",
                value="x86-rosetta-wow64",
                confidence=0.85,
                reason="x86 PE → Rosetta WoW64 lane",
            )
        )

    # --- Graphics API / backend ---
    if _bool(features.get("import_d3d12")):
        overrides.append(
            Prediction(
                field="graphics.api",
                value="d3d12",
                confidence=0.90,
                reason="Imports d3d12.dll → D3D12 API",
            )
        )
        overrides.append(
            Prediction(
                field="graphics.preferred_backend",
                value="d3dmetal",
                confidence=0.85,
                reason="D3D12 on macOS → D3DMetal preferred",
            )
        )
        overrides.append(
            Prediction(
                field="graphics.fallback_backend",
                value="vkd3d-moltenvk",
                confidence=0.70,
                reason="D3D12 fallback → VKD3D+MoltenVK",
            )
        )
    elif _bool(features.get("import_d3d11")):
        overrides.append(
            Prediction(
                field="graphics.api",
                value="d3d11",
                confidence=0.90,
                reason="Imports d3d11.dll → D3D11 API",
            )
        )
        overrides.append(
            Prediction(
                field="graphics.preferred_backend",
                value="d3dmetal",
                confidence=0.80,
                reason="D3D11 on macOS → D3DMetal preferred",
            )
        )
        overrides.append(
            Prediction(
                field="graphics.fallback_backend",
                value="dxvk-moltenvk",
                confidence=0.70,
                reason="D3D11 fallback → DXVK+MoltenVK",
            )
        )
    elif _bool(features.get("import_d3d9")):
        overrides.append(
            Prediction(
                field="graphics.api",
                value="d3d9",
                confidence=0.90,
                reason="Imports d3d9.dll → D3D9 API",
            )
        )
        overrides.append(
            Prediction(
                field="graphics.preferred_backend",
                value="dxvk-moltenvk",
                confidence=0.75,
                reason="D3D9 on macOS → DXVK+MoltenVK",
            )
        )
    elif _bool(features.get("import_opengl32")):
        overrides.append(
            Prediction(
                field="graphics.api",
                value="opengl",
                confidence=0.80,
                reason="Imports opengl32.dll → OpenGL API",
            )
        )
        overrides.append(
            Prediction(
                field="graphics.preferred_backend",
                value="native-metal-experimental",
                confidence=0.50,
                reason="OpenGL on macOS → experimental Metal translation",
            )
        )
    elif _bool(features.get("import_vulkan")):
        overrides.append(
            Prediction(
                field="graphics.api",
                value="vulkan",
                confidence=0.80,
                reason="Imports vulkan-1.dll → Vulkan API",
            )
        )
        overrides.append(
            Prediction(
                field="graphics.preferred_backend",
                value="moltenvk",
                confidence=0.85,
                reason="Vulkan on macOS → MoltenVK",
            )
        )

    # --- Shader cache ---
    if any(
        _bool(features.get(k))
        for k in ("import_d3d11", "import_d3d12", "import_d3d9", "import_opengl32")
    ):
        overrides.append(
            Prediction(
                field="graphics.shader_cache",
                value=True,
                confidence=0.85,
                reason="Graphics API detected → enable shader cache",
            )
        )

    # --- Category detection ---
    category = _detect_category(features)
    overrides.append(
        Prediction(
            field="category",
            value=category,
            confidence=0.75,
            reason=f"Detected as {category} from PE heuristics",
        )
    )

    # --- Anti-cheat ---
    if _bool(features.get("has_anticheat_signature")):
        overrides.append(
            Prediction(
                field="anti_cheat.mode",
                value="warn",
                confidence=0.85,
                reason="Anti-cheat signature detected → warn user",
            )
        )
        overrides.append(
            Prediction(
                field="anti_cheat.online_supported",
                value=False,
                confidence=0.90,
                reason="Anti-cheat present → online play not supported",
            )
        )
        overrides.append(
            Prediction(
                field="default_lane",
                value="x86_64-rosetta",
                confidence=0.80,
                reason="Anti-cheat present → safest lane is Rosetta",
            )
        )

    # --- .NET / Framework ---
    if _bool(features.get("is_dotnet")):
        overrides.append(
            Prediction(
                field="required_dlls",
                value=("dotnet48",),
                confidence=0.70,
                reason=".NET binary → install .NET Framework 4.8",
            )
        )
        overrides.append(
            Prediction(
                field="wine_settings.mono",
                value=True,
                confidence=0.60,
                reason="Enable Wine Mono as .NET fallback",
            )
        )

    # --- Installer ---
    if _bool(features.get("is_installer")):
        overrides.append(
            Prediction(
                field="category",
                value="utility",
                confidence=0.80,
                reason="Installer detected → utility category",
            )
        )
        overrides.append(
            Prediction(
                field="default_lane",
                value="x86_64-rosetta",
                confidence=0.70,
                reason="Installers often x86 → Rosetta lane",
            )
        )

    # --- Threading for games ---
    if category == "game":
        overrides.append(
            Prediction(
                field="performance.threading",
                value="multi",
                confidence=0.65,
                reason="Games benefit from multi-threading",
            )
        )
        overrides.append(
            Prediction(
                field="performance.fast_io",
                value=True,
                confidence=0.55,
                reason="Games may benefit from fast I/O prefetch",
            )
        )

    # --- Business app defaults ---
    if category == "business":
        overrides.append(
            Prediction(
                field="bottle_policy.isolation",
                value="dedicated",
                confidence=0.60,
                reason="Business apps benefit from dedicated bottle isolation",
            )
        )
        overrides.append(
            Prediction(
                field="wine_settings.locale",
                value="en_US.UTF-8",
                confidence=0.70,
                reason="Force US UTF-8 for business app compatibility",
            )
        )

    # --- Lane conflict resolution ---
    # If multiple default_lane predictions exist, keep the highest-confidence one
    lane_preds = [p for p in overrides if p.field == "default_lane"]
    if len(lane_preds) > 1:
        best = max(lane_preds, key=lambda p: p.confidence)
        overrides = [p for p in overrides if p.field != "default_lane"]
        overrides.append(best)

    avg_confidence = (
        sum(p.confidence for p in overrides) / len(overrides) if overrides else 0.0
    )
    return PredictionResult(
        predicted_category=category,
        overrides=overrides,
        confidence=round(avg_confidence, 3),
        strategy="rule-based",
    )


def _predict_xgboost(features: Mapping[str, Any], model: Any) -> PredictionResult:
    """XGBoost prediction with model; falls back to rule-based if model unavailable."""
    if model is None:
        result = _predict_rule_based(features)
        return PredictionResult(
            predicted_category=result.predicted_category,
            overrides=result.overrides,
            confidence=result.confidence * 0.9,
            strategy="xgboost-stub",
        )

    preds = model.predict(features)
    overrides: list[Prediction] = []
    category = preds.get("category", ("utility", 0.5))[0]

    # Map predicted fields to Prediction objects
    field_map = {
        "category": "predicted category from XGBoost",
        "graphics.api": "graphics API from XGBoost",
        "default_lane": "execution lane from XGBoost",
    }
    confidences: list[float] = []
    for field, (value, prob) in preds.items():
        overrides.append(
            Prediction(
                field=field,
                value=value,
                confidence=round(prob, 3),
                reason=field_map.get(field, f"XGBoost prediction for {field}"),
            )
        )
        confidences.append(prob)

    avg_confidence = round(sum(confidences) / len(confidences), 3) if confidences else 0.5
    return PredictionResult(
        predicted_category=category,
        overrides=overrides,
        confidence=avg_confidence,
        strategy="xgboost",
    )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _bool(value: object | None) -> bool:
    if value is None:
        return False
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.lower() in {"true", "1", "yes", "on"}
    return bool(value)


def _detect_arch(features: Mapping[str, Any]) -> str:
    """Detect architecture from one-hot feature fields."""
    if _bool(features.get("arch_arm64")):
        return "arm64"
    if _bool(features.get("arch_x86_64")):
        return "x86_64"
    if _bool(features.get("arch_i386")):
        return "i386"
    if _bool(features.get("arch_arm64ec")):
        return "arm64ec"
    if _bool(features.get("arch_arm64x")):
        return "arm64x"
    return "unknown"


def _detect_category(features: Mapping[str, Any]) -> str:
    """Detect app category from PE heuristics."""
    if _bool(features.get("is_installer")):
        return "utility"
    if _bool(features.get("framework_unity")) or _bool(features.get("framework_unreal")):
        return "game"
    if _bool(features.get("framework_wpf")) or _bool(features.get("framework_winforms")):
        return "business"
    if _bool(features.get("import_d3d11")) or _bool(features.get("import_d3d12")) or _bool(
        features.get("import_d3d9")
    ):
        return "game"
    if _bool(features.get("import_gdi32")) and _bool(features.get("import_user32")):
        # Could be game or business; default to business for GUI apps without graphics API
        if not _bool(features.get("import_d3d11")) and not _bool(features.get("import_d3d12")):
            return "business"
    return "utility"
