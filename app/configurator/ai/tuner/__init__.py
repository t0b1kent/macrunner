"""MacRunner AI Configurator — Auto-Tuning Loop (Phase A.4).

Iteratively adjust Wine env vars / settings based on smoke-test feedback.

Architecture:
- SearchSpace: defines valid parameter ranges per category (game/business)
- TuningStrategy: hill-climbing + random restart
- TuningExecutor: runs smoke tests, collects metrics, applies next delta
- TuningSession: tracks state across iterations

Score function:
    score = (launch_success * 100) + fps_norm - (stability_penalty * 20)

Early termination: score plateau ≥ 3 iterations.
Rollback: if score drops > 20% from best, revert and try different direction.
"""

from __future__ import annotations

import copy
import json
import random
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence


# ---------------------------------------------------------------------------
# Search space definitions
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Parameter:
    """A tunable parameter with its valid range and step size."""

    name: str  # dot-path, e.g. "env.WINEESYNC"
    kind: str  # "bool", "int", "float", "choice", "string"
    default: Any
    low: float | None = None
    high: float | None = None
    step: float | None = None
    choices: tuple[Any, ...] = ()
    description: str = ""
    category: str = "all"  # "game", "business", "all"


# Pre-defined search spaces per category
_GAME_PARAMETERS: list[Parameter] = [
    Parameter(
        name="env.MACRUNNER_FAST_IO",
        kind="bool",
        default=False,
        description="Enable fast I/O for game assets",
        category="game",
    ),
    Parameter(
        name="env.WINEESYNC",
        kind="bool",
        default=True,
        description="Enable esync for better thread scheduling",
        category="game",
    ),
    Parameter(
        name="env.WINEFSYNC",
        kind="bool",
        default=False,
        description="Enable fsync (macOS futex support)",
        category="game",
    ),
    Parameter(
        name="env.MESA_GL_VERSION_OVERRIDE",
        kind="choice",
        default=None,
        choices=(None, "4.6", "4.5", "3.3"),
        description="Override OpenGL version reported to app",
        category="game",
    ),
    Parameter(
        name="env.MACRUNNER_SHADER_CACHE",
        kind="bool",
        default=True,
        description="Enable shader compilation cache",
        category="game",
    ),
    Parameter(
        name="env.MACRUNNER_HB_STACK_RESERVE",
        kind="choice",
        default="default",
        choices=("default", "large", "xlarge"),
        description="HyperBridge stack reserve size",
        category="game",
    ),
    Parameter(
        name="performance.threading",
        kind="choice",
        default="auto",
        choices=("auto", "single", "multi"),
        description="Threading mode",
        category="game",
    ),
    Parameter(
        name="env.MACRUNNER_GFX_PRESENT_MODE",
        kind="choice",
        default="fifo",
        choices=("fifo", "mailbox", "immediate"),
        description="Graphics present mode",
        category="game",
    ),
    Parameter(
        name="env.MACRUNNER_DISABLE_OVERLAYS",
        kind="bool",
        default=False,
        description="Disable Steam/overlay injection",
        category="game",
    ),
]

_BUSINESS_PARAMETERS: list[Parameter] = [
    Parameter(
        name="wine_settings.locale",
        kind="choice",
        default="en_US.UTF-8",
        choices=("en_US.UTF-8", "ja_JP.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8"),
        description="Locale for business apps",
        category="business",
    ),
    Parameter(
        name="bottle_policy.isolation",
        kind="choice",
        default="shared",
        choices=("shared", "dedicated"),
        description="Bottle isolation level",
        category="business",
    ),
    Parameter(
        name="wine_settings.font_smoothing",
        kind="choice",
        default="auto",
        choices=("auto", "enabled", "disabled"),
        description="Font smoothing mode",
        category="business",
    ),
    Parameter(
        name="env.MACRUNNER_SAFE_MODE",
        kind="bool",
        default=False,
        description="Disable risky optimizations for stability",
        category="business",
    ),
    Parameter(
        name="wine_settings.desktop",
        kind="choice",
        default="none",
        choices=("none", "virtual", "fullscreen"),
        description="Desktop emulation mode",
        category="business",
    ),
    Parameter(
        name="env.WINEDEBUG",
        kind="choice",
        default="-all",
        choices=("-all", "fixme-all", "err+all"),
        description="Wine debug channel filter",
        category="business",
    ),
]

_COMMON_PARAMETERS: list[Parameter] = [
    Parameter(
        name="env.MACRUNNER_HB_X64_LOADER",
        kind="bool",
        default=True,
        description="Enable HyperBridge x64 loader",
        category="all",
    ),
    Parameter(
        name="env.MACRUNNER_NATIVE_LEVEL",
        kind="choice",
        default="auto",
        choices=("auto", "full", "partial", "none"),
        description="Native API call level",
        category="all",
    ),
    Parameter(
        name="default_lane",
        kind="choice",
        default="arm64-native",
        choices=("arm64-native", "x86_64-rosetta", "arm64ec-bridge"),
        description="Execution lane",
        category="all",
    ),
]


def get_search_space(category: str = "game") -> list[Parameter]:
    """Return parameters appropriate for the given category."""
    params = list(_COMMON_PARAMETERS)
    if category == "game":
        params.extend(_GAME_PARAMETERS)
    elif category == "business":
        params.extend(_BUSINESS_PARAMETERS)
    else:
        params.extend(_GAME_PARAMETERS)
        params.extend(_BUSINESS_PARAMETERS)
    return params


# ---------------------------------------------------------------------------
# Tuning session state
# ---------------------------------------------------------------------------

@dataclass
class TuningIteration:
    """Result of a single tuning iteration."""

    iteration: int
    params: dict[str, Any]
    score: float
    launch_success: bool
    fps: float | None
    stability_penalty: float
    duration_ms: int
    stderr_analysis: dict[str, Any] = field(default_factory=dict)


@dataclass
class TuningSession:
    """Tracks state across a complete tuning run."""

    profile_id: str
    category: str
    max_iterations: int = 10
    best_score: float = 0.0
    best_params: dict[str, Any] = field(default_factory=dict)
    history: list[TuningIteration] = field(default_factory=list)
    plateau_count: int = 0
    terminated_early: bool = False

    def record(self, iteration: TuningIteration) -> None:
        self.history.append(iteration)
        if iteration.score > self.best_score:
            self.best_score = iteration.score
            self.best_params = copy.deepcopy(iteration.params)
            self.plateau_count = 0
        else:
            self.plateau_count += 1

    @property
    def current_iteration(self) -> int:
        return len(self.history)

    def should_terminate(self) -> bool:
        if self.current_iteration >= self.max_iterations:
            return True
        if self.plateau_count >= 3:
            self.terminated_early = True
            return True
        return False


# ---------------------------------------------------------------------------
# Score function
# ---------------------------------------------------------------------------

def compute_score(
    launch_success: bool,
    fps: float | None,
    stability_penalty: float = 0.0,
    error_count: int = 0,
) -> float:
    """Compute a composite score for a tuning iteration.

    - launch_success: 100 points if app launched, 0 otherwise
    - fps: normalized 0-50 (capped at 60 fps = 50 points)
    - stability_penalty: -20 per major error/warning
    - error_count: -5 per matched error pattern
    """
    score = 100.0 if launch_success else 0.0
    if fps is not None:
        score += min(fps / 60.0 * 50.0, 50.0)
    score -= stability_penalty * 20.0
    score -= error_count * 5.0
    return max(score, 0.0)


# ---------------------------------------------------------------------------
# Strategy: hill-climbing with random restart
# ---------------------------------------------------------------------------

class HillClimbingStrategy:
    """Hill-climbing tuner with random restarts and rollback."""

    def __init__(self, parameters: Sequence[Parameter], seed: int | None = None) -> None:
        self.parameters = list(parameters)
        self.rng = random.Random(seed)
        self._rollback_threshold: float = 0.20  # 20% regression triggers rollback

    def initial_params(self) -> dict[str, Any]:
        """Start from defaults."""
        return {p.name: p.default for p in self.parameters}

    def mutate(self, params: dict[str, Any]) -> dict[str, Any]:
        """Create a neighboring configuration by mutating one parameter."""
        out = copy.deepcopy(params)
        param = self.rng.choice(self.parameters)
        current = out.get(param.name, param.default)

        if param.kind == "bool":
            out[param.name] = not current
        elif param.kind == "choice" and param.choices:
            # Pick a different choice
            choices = [c for c in param.choices if c != current]
            if choices:
                out[param.name] = self.rng.choice(choices)
        elif param.kind == "int":
            low = int(param.low) if param.low is not None else 0
            high = int(param.high) if param.high is not None else 10
            step = int(param.step) if param.step is not None else 1
            delta = self.rng.choice([-1, 1]) * step
            new_val = current + delta if isinstance(current, int) else low
            out[param.name] = max(low, min(high, new_val))
        elif param.kind == "float":
            low = param.low if param.low is not None else 0.0
            high = param.high if param.high is not None else 1.0
            step = param.step if param.step is not None else 0.1
            delta = self.rng.choice([-1.0, 1.0]) * step
            new_val = current + delta if isinstance(current, float) else low
            out[param.name] = max(low, min(high, new_val))
        elif param.kind == "string":
            # No mutation for free strings
            pass

        return out

    def should_rollback(self, current_score: float, best_score: float) -> bool:
        if best_score <= 0:
            return False
        drop = (best_score - current_score) / best_score
        return drop > self._rollback_threshold

    def random_restart_params(self) -> dict[str, Any]:
        """Generate a completely random configuration."""
        out: dict[str, Any] = {}
        for param in self.parameters:
            if param.kind == "bool":
                out[param.name] = self.rng.choice([True, False])
            elif param.kind == "choice" and param.choices:
                out[param.name] = self.rng.choice(param.choices)
            elif param.kind == "int":
                low = int(param.low) if param.low is not None else 0
                high = int(param.high) if param.high is not None else 10
                out[param.name] = self.rng.randint(low, high)
            elif param.kind == "float":
                low = param.low if param.low is not None else 0.0
                high = param.high if param.high is not None else 1.0
                out[param.name] = self.rng.uniform(low, high)
            else:
                out[param.name] = param.default
        return out


# ---------------------------------------------------------------------------
# Executor interface
# ---------------------------------------------------------------------------

TuningRunFunction = Callable[[dict[str, Any]], dict[str, Any]]
"""Signature: take params dict, return metrics dict with keys:
    - launch_success: bool
    - fps: float | None
    - stability_penalty: float
    - error_count: int
    - duration_ms: int
    - stderr_analysis: dict (optional)
"""


def run_tuning_session(
    profile_id: str,
    category: str,
    run_fn: TuningRunFunction,
    max_iterations: int = 10,
    seed: int | None = None,
    enable_random_restart: bool = True,
    restart_every: int = 5,
) -> TuningSession:
    """Run a complete auto-tuning session.

    Args:
        profile_id: identifier for the profile being tuned
        category: "game" or "business"
        run_fn: callable that executes a smoke test with given params
        max_iterations: maximum tuning iterations
        seed: RNG seed for reproducibility
        enable_random_restart: whether to inject random restarts
        restart_every: restart after this many iterations

    Returns:
        TuningSession with full history and best_params.
    """
    parameters = get_search_space(category)
    strategy = HillClimbingStrategy(parameters, seed=seed)
    session = TuningSession(
        profile_id=profile_id,
        category=category,
        max_iterations=max_iterations,
        best_params=strategy.initial_params(),
    )
    current_params = strategy.initial_params()

    for iteration_num in range(1, max_iterations + 1):
        metrics = run_fn(current_params)
        score = compute_score(
            launch_success=metrics.get("launch_success", False),
            fps=metrics.get("fps"),
            stability_penalty=metrics.get("stability_penalty", 0.0),
            error_count=metrics.get("error_count", 0),
        )

        iteration = TuningIteration(
            iteration=iteration_num,
            params=copy.deepcopy(current_params),
            score=score,
            launch_success=metrics.get("launch_success", False),
            fps=metrics.get("fps"),
            stability_penalty=metrics.get("stability_penalty", 0.0),
            duration_ms=metrics.get("duration_ms", 0),
            stderr_analysis=metrics.get("stderr_analysis", {}),
        )
        session.record(iteration)

        if session.should_terminate():
            break

        # Check rollback
        if strategy.should_rollback(score, session.best_score):
            current_params = copy.deepcopy(session.best_params)
            continue

        # Random restart
        if enable_random_restart and iteration_num > 0 and iteration_num % restart_every == 0:
            current_params = strategy.random_restart_params()
            continue

        # Hill-climb: mutate best or current
        base = session.best_params if session.best_score > score else current_params
        current_params = strategy.mutate(base)

    return session


# ---------------------------------------------------------------------------
# CLI / report helpers
# ---------------------------------------------------------------------------

def format_tuning_report(session: TuningSession) -> str:
    """Return a human-readable tuning report."""
    lines = [
        f"Tuning Report: {session.profile_id} ({session.category})",
        f"Iterations: {session.current_iteration} / {session.max_iterations}",
        f"Terminated early: {session.terminated_early}",
        f"Best score: {session.best_score:.1f}",
        "Best parameters:",
    ]
    for k, v in sorted(session.best_params.items()):
        lines.append(f"  {k} = {v!r}")
    lines.append("History:")
    for it in session.history:
        status = "✓" if it.launch_success else "✗"
        fps_str = f"{it.fps:.1f} fps" if it.fps is not None else "n/a"
        lines.append(
            f"  [{status}] iter {it.iteration}: score={it.score:.1f} {fps_str} "
            f"errors={it.stderr_analysis.get('errors_found', [])}"
        )
    return "\n".join(lines)


def export_tuning_report(session: TuningSession, path: Path | str) -> None:
    """Export tuning session as JSON."""
    data = {
        "profile_id": session.profile_id,
        "category": session.category,
        "max_iterations": session.max_iterations,
        "best_score": session.best_score,
        "best_params": session.best_params,
        "terminated_early": session.terminated_early,
        "history": [
            {
                "iteration": it.iteration,
                "params": it.params,
                "score": it.score,
                "launch_success": it.launch_success,
                "fps": it.fps,
                "stability_penalty": it.stability_penalty,
                "duration_ms": it.duration_ms,
                "stderr_analysis": it.stderr_analysis,
            }
            for it in session.history
        ],
    }
    Path(path).write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
