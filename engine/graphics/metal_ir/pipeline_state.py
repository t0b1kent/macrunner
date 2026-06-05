from __future__ import annotations

from dataclasses import asdict, dataclass, field
from enum import Enum
from typing import Any


class PipelineValidationError(ValueError):
    pass


class PrimitiveTopology(str, Enum):
    TRIANGLELIST = "trianglelist"
    TRIANGLESTRIP = "trianglestrip"
    LINELIST = "linelist"
    POINTLIST = "pointlist"


@dataclass
class PipelineState:
    api: str
    primitive_topology: str | None = None
    input_layout: list[str] = field(default_factory=list)
    vertex_shader: str | None = None
    pixel_shader: str | None = None
    render_target_bound: bool = False
    depth_target_bound: bool = False
    swapchain_backbuffer_bound: bool = False
    vertex_buffer_bound: bool = False
    index_buffer_bound: bool = False
    constant_buffer_bound: bool = False
    shader_resource_bound: bool = False
    viewport_set: bool = False
    scissor_set: bool = False
    render_target_format: str | None = "rgba8"
    depth_format: str | None = None
    resource_states: dict[str, str] = field(default_factory=dict)
    metadata: dict[str, Any] = field(default_factory=dict)

    def validate_for_draw(self, *, indexed: bool = False, require_shader: bool = False) -> list[str]:
        errors: list[str] = []
        if not self.render_target_bound and not self.swapchain_backbuffer_bound:
            errors.append("draw without render target")
        if not self.primitive_topology:
            errors.append("draw without topology")
        if not self.vertex_buffer_bound:
            errors.append("draw without vertex buffer")
        if indexed and not self.index_buffer_bound:
            errors.append("draw indexed without index buffer")
        if require_shader and not (self.vertex_shader and self.pixel_shader):
            errors.append("missing shader without registered placeholder")
        if self.render_target_format not in {
            None,
            "rgba8",
            "bgra8",
            "rgba8unorm",
            "rgba10a2",
            "bgra10a2",
            "rgba4",
            "a8",
            "r8",
            "r16",
            "rg8",
            "rg8_snorm",
            "rgba8_snorm",
            "rg16_snorm",
            "bc1",
            "bc2",
            "bc3",
            "r16f",
            "rg16f",
            "rgba16f",
            "r32f",
            "rg32f",
            "rgba32f",
            "rg16",
            "rgba16",
            "null",
        }:
            errors.append(f"unsupported format: {self.render_target_format}")
        return errors

    def validate_barrier(self, before: str, after: str) -> str | None:
        if not before or not after:
            return "invalid D3D12 barrier transition"
        if before == after:
            return "invalid D3D12 barrier transition"
        return None

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)
