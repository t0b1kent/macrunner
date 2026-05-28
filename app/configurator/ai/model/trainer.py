"""XGBoost training pipeline for MacRunner profile prediction (Phase A.5).

Pipeline:
1. Load dataset (synthetic + real profiles)
2. Encode features → dense numeric vector
3. Encode labels → categorical / ordinal targets
4. Train with cross-validation and early stopping
5. Hyperparameter tuning (Optuna or grid search)
6. Persist model to disk
"""

from __future__ import annotations

import json
import pickle
from pathlib import Path
from typing import Any, Mapping, Sequence

import numpy as np


# ---------------------------------------------------------------------------
# Feature encoder
# ---------------------------------------------------------------------------

# Ordered list of feature keys expected in PE feature dict.
# Must match the order used at inference time.
_FEATURE_ORDER: list[str] = [
    # Architecture one-hot
    "arch_x86_64",
    "arch_arm64",
    "arch_i386",
    "arch_arm64ec",
    "arch_arm64x",
    # Subsystem
    "subsystem_windows_gui",
    "subsystem_windows_cui",
    "subsystem_efi_app",
    # Graphics imports
    "import_d3d12",
    "import_d3d11",
    "import_d3d9",
    "import_opengl32",
    "import_vulkan",
    # Core imports
    "import_kernel32",
    "import_user32",
    "import_gdi32",
    "import_shell32",
    "import_advapi32",
    "import_ws2_32",
    # Heuristics
    "has_anticheat_signature",
    "is_dotnet",
    "is_installer",
    "is_packed",
    "framework_unity",
    "framework_unreal",
    "framework_wpf",
    "framework_winforms",
    "framework_electron",
    "framework_qt",
    "framework_gtk",
    "is_text_editor",
    # PE stats
    "section_count",
    "import_count",
    "export_count",
    "has_resources",
    "has_signature",
    "has_debug",
    "entry_point_present",
    "large_address_aware",
]


def encode_features(features: Mapping[str, Any]) -> list[float]:
    """Convert a PE feature dict to a dense numeric vector.

    Missing features default to 0.0. Boolean/int features are cast to float.
    """
    out: list[float] = []
    for key in _FEATURE_ORDER:
        raw = features.get(key)
        if raw is None:
            out.append(0.0)
        elif isinstance(raw, bool):
            out.append(1.0 if raw else 0.0)
        elif isinstance(raw, (int, float)):
            out.append(float(raw))
        elif isinstance(raw, str):
            # Hash string to a deterministic float in [-1, 1]
            out.append((hash(raw) % 1000) / 500.0 - 1.0)
        else:
            out.append(0.0)
    return out


def feature_dim() -> int:
    return len(_FEATURE_ORDER)


# ---------------------------------------------------------------------------
# Label encoders
# ---------------------------------------------------------------------------

class LabelEncoder:
    """Simple string → int encoder for categorical targets."""

    def __init__(self) -> None:
        self._classes: list[str] = []
        self._index: dict[str, int] = {}

    def fit(self, values: Sequence[str]) -> None:
        self._classes = sorted(set(values))
        self._index = {v: i for i, v in enumerate(self._classes)}

    def encode(self, value: str) -> int:
        return self._index.get(value, len(self._classes))

    def decode(self, idx: int) -> str:
        if 0 <= idx < len(self._classes):
            return self._classes[idx]
        return "unknown"

    def save(self, path: Path | str) -> None:
        Path(path).write_text(json.dumps(self._classes, indent=2), encoding="utf-8")

    @classmethod
    def load(cls, path: Path | str) -> "LabelEncoder":
        inst = cls()
        inst._classes = json.loads(Path(path).read_text(encoding="utf-8"))
        inst._index = {v: i for i, v in enumerate(inst._classes)}
        return inst


# ---------------------------------------------------------------------------
# Dataset builder
# ---------------------------------------------------------------------------

from app.configurator.ai.model.synthetic_data import generate_synthetic_dataset


def build_training_dataset(
    n_synthetic: int = 500,
    seed: int | None = None,
    real_profiles: Sequence[dict[str, Any]] | None = None,
) -> tuple[list[list[float]], dict[str, list[int]], LabelEncoder, LabelEncoder, LabelEncoder]:
    """Build feature matrix and label dicts from synthetic + real data.

    Returns:
        - X: list of feature vectors
        - Y: dict of label sequences per target field
        - category_encoder: fitted LabelEncoder for category
        - api_encoder: fitted LabelEncoder for graphics.api
        - lane_encoder: fitted LabelEncoder for default_lane
    """
    synthetic = generate_synthetic_dataset(n_samples=n_synthetic, seed=seed, noise_intensity=0.2)
    samples = list(synthetic)
    if real_profiles:
        samples.extend(real_profiles)

    X: list[list[float]] = []
    categories: list[str] = []
    apis: list[str] = []
    lanes: list[str] = []

    for sample in samples:
        features = sample["features"]
        labels = sample["labels"]
        X.append(encode_features(features))
        categories.append(labels.get("category", "utility"))
        apis.append(labels.get("graphics.api", "none"))
        lanes.append(labels.get("default_lane", "arm64-native"))

    cat_enc = LabelEncoder()
    cat_enc.fit(categories)
    api_enc = LabelEncoder()
    api_enc.fit(apis)
    lane_enc = LabelEncoder()
    lane_enc.fit(lanes)

    Y = {
        "category": [cat_enc.encode(v) for v in categories],
        "graphics.api": [api_enc.encode(v) for v in apis],
        "default_lane": [lane_enc.encode(v) for v in lanes],
    }

    return X, Y, cat_enc, api_enc, lane_enc


# ---------------------------------------------------------------------------
# Trainer
# ---------------------------------------------------------------------------

class XGBoostTrainer:
    """Train XGBoost multi-output models for profile prediction."""

    def __init__(self, seed: int = 42) -> None:
        self.seed = seed
        self.models: dict[str, Any] = {}
        self.encoders: dict[str, LabelEncoder] = {}
        self._feature_order = list(_FEATURE_ORDER)

    def train(
        self,
        X: Sequence[Sequence[float]],
        Y: Mapping[str, Sequence[int]],
        encoders: Mapping[str, LabelEncoder],
        test_size: float = 0.2,
    ) -> dict[str, float]:
        """Train models for each target field.

        Returns per-field accuracy on held-out test set.
        """
        import xgboost as xgb
        from sklearn.model_selection import train_test_split

        X_arr = np.array(X, dtype=np.float32)
        self.encoders = dict(encoders)
        metrics: dict[str, float] = {}

        for field, y in Y.items():
            y_arr = np.array(y, dtype=np.int32)
            X_train, X_test, y_train, y_test = train_test_split(
                X_arr, y_arr, test_size=test_size, random_state=self.seed, stratify=y_arr
            )

            dtrain = xgb.DMatrix(X_train, label=y_train)
            dtest = xgb.DMatrix(X_test, label=y_test)

            params = {
                "objective": "multi:softprob",
                "num_class": len(encoders[field]._classes) + 1,
                "eval_metric": "mlogloss",
                "max_depth": 5,
                "learning_rate": 0.1,
                "subsample": 0.8,
                "colsample_bytree": 0.8,
                "seed": self.seed,
            }

            model = xgb.train(
                params,
                dtrain,
                num_boost_round=200,
                evals=[(dtest, "test")],
                early_stopping_rounds=10,
                verbose_eval=False,
            )
            self.models[field] = model

            preds = model.predict(dtest)
            pred_labels = np.argmax(preds, axis=1)
            accuracy = float(np.mean(pred_labels == y_test))
            metrics[field] = round(accuracy, 4)

        return metrics

    def predict(self, features: Mapping[str, Any]) -> dict[str, tuple[str, float]]:
        """Predict labels for each target field.

        Returns dict of field → (predicted_label, max_probability).
        """
        import xgboost as xgb

        vec = encode_features(features)
        dmat = xgb.DMatrix(np.array([vec], dtype=np.float32))
        result: dict[str, tuple[str, float]] = {}
        for field, model in self.models.items():
            pred = model.predict(dmat)
            probs = pred[0]
            idx = int(np.argmax(probs))
            prob = float(probs[idx])
            encoder = self.encoders[field]
            label = encoder.decode(idx)
            result[field] = (label, round(prob, 4))
        return result

    def save(self, directory: Path | str) -> None:
        """Persist models and encoders to disk."""
        dir_path = Path(directory)
        dir_path.mkdir(parents=True, exist_ok=True)
        for field, model in self.models.items():
            model.save_model(str(dir_path / f"{field.replace('.', '_')}.json"))
        for field, encoder in self.encoders.items():
            encoder.save(dir_path / f"{field.replace('.', '_')}_encoder.json")
        meta = {
            "feature_order": self._feature_order,
            "seed": self.seed,
            "encoder_fields": list(self.encoders.keys()),
        }
        (dir_path / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")

    @classmethod
    def load(cls, directory: Path | str) -> "XGBoostTrainer":
        """Load persisted models and encoders from disk."""
        import xgboost as xgb

        dir_path = Path(directory)
        inst = cls()
        meta = json.loads((dir_path / "meta.json").read_text(encoding="utf-8"))
        inst._feature_order = meta["feature_order"]
        inst.seed = meta["seed"]

        encoder_fields = meta.get("encoder_fields", [])
        if encoder_fields:
            for field in encoder_fields:
                encoder_path = dir_path / f"{field.replace('.', '_')}_encoder.json"
                inst.encoders[field] = LabelEncoder.load(encoder_path)
        else:
            # Legacy fallback: infer from filenames (imprecise for underscores)
            for encoder_path in sorted(dir_path.glob("*_encoder.json")):
                field = encoder_path.name.replace("_encoder.json", "").replace("_", ".")
                inst.encoders[field] = LabelEncoder.load(encoder_path)

        for field in inst.encoders:
            model_path = dir_path / f"{field.replace('.', '_')}.json"
            if model_path.exists():
                inst.models[field] = xgb.Booster()
                inst.models[field].load_model(str(model_path))

        return inst


# ---------------------------------------------------------------------------
# Convenience: train from synthetic data
# ---------------------------------------------------------------------------

def train_from_synthetic(
    n_synthetic: int = 500,
    seed: int = 42,
    model_dir: Path | str | None = None,
) -> tuple[XGBoostTrainer, dict[str, float]]:
    """End-to-end: generate synthetic data, train, optionally save.

    Returns (trainer, metrics_dict).
    """
    X, Y, cat_enc, api_enc, lane_enc = build_training_dataset(
        n_synthetic=n_synthetic, seed=seed
    )
    trainer = XGBoostTrainer(seed=seed)
    metrics = trainer.train(X, Y, {
        "category": cat_enc,
        "graphics.api": api_enc,
        "default_lane": lane_enc,
    })
    if model_dir:
        trainer.save(model_dir)
    return trainer, metrics
