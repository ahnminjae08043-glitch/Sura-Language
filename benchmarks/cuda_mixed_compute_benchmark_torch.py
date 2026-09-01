#!/usr/bin/env python3
"""PyTorch companion for Sura's CUDA mixed-compute benchmark."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import statistics
import sys
import time
from typing import Any


SCHEMA = "sura.cuda-mixed-compute-benchmark.torch.v2"


def percentile95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * 0.95) - 1)]


def storage_bytes(tensor: Any) -> int:
    return int(tensor.numel() * tensor.element_size())


def dtype_name(tensor: Any) -> str:
    return str(tensor.dtype).removeprefix("torch.")


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tensor_fingerprint(tensor: Any) -> dict[str, Any]:
    # Host float64 reductions make this deterministic and independent of the
    # GPU reduction tree. Observation occurs only after performance timing.
    values = tensor.detach().float().cpu().double()
    rows, columns = (int(values.shape[0]), int(values.shape[1]))
    squared = values.square().sum().item()
    return {
        "sum": float(values.sum().item()),
        "l1": float(values.abs().sum().item()),
        "l2": math.sqrt(float(squared)),
        "samples": {
            "first": float(values[0, 0].item()),
            "center": float(values[rows // 2, columns // 2].item()),
            "last": float(values[rows - 1, columns - 1].item()),
        },
    }


def timed_forward(
    torch: Any, left: Any, right: Any, warmup: int, runs: int
) -> dict[str, Any]:
    output = None
    with torch.no_grad():
        for _ in range(warmup):
            output = torch.matmul(left, right)
        torch.cuda.synchronize(left.device)
        samples: list[float] = []
        for _ in range(runs):
            torch.cuda.synchronize(left.device)
            started = time.perf_counter()
            output = torch.matmul(left, right)
            torch.cuda.synchronize(left.device)
            samples.append((time.perf_counter() - started) * 1000.0)
    assert output is not None
    checksum = float(output.detach().float().cpu().double().sum().item())
    return {
        "status": "ok",
        "workload": "synchronized eager matmul forward",
        "median_ms": statistics.median(samples),
        "p95_ms": percentile95(samples),
        "raw_ms": samples,
        "output_checksum": checksum,
        "input_dtype": dtype_name(left),
        "weight_dtype": dtype_name(right),
        "output_dtype": dtype_name(output),
        "input_storage_bytes": storage_bytes(left),
        "weight_storage_bytes": storage_bytes(right),
        "output_storage_bytes": storage_bytes(output),
    }


def timed_forward_backward(
    torch: Any, left: Any, right: Any, seed: Any, warmup: int, runs: int
) -> dict[str, Any]:
    output = None

    def clear_gradients() -> None:
        if left.grad is None:
            left.grad = torch.zeros_like(left)
        else:
            left.grad.zero_()
        if right.grad is None:
            right.grad = torch.zeros_like(right)
        else:
            right.grad.zero_()

    for _ in range(warmup):
        clear_gradients()
        output = torch.matmul(left, right)
        output.backward(seed)
    # Allocate/clear persistent leaf gradients before the first timed sample,
    # including when warmup is zero.
    clear_gradients()
    torch.cuda.synchronize(left.device)

    samples: list[float] = []
    for _ in range(runs):
        clear_gradients()
        torch.cuda.synchronize(left.device)
        started = time.perf_counter()
        output = torch.matmul(left, right)
        output.backward(seed)
        torch.cuda.synchronize(left.device)
        samples.append((time.perf_counter() - started) * 1000.0)

    assert output is not None and left.grad is not None and right.grad is not None
    checksum = float(output.detach().float().cpu().double().sum().item())
    return {
        "status": "ok",
        "workload": "synchronized eager matmul forward+autograd backward",
        "gradient_zeroing_timed": False,
        "median_ms": statistics.median(samples),
        "p95_ms": percentile95(samples),
        "raw_ms": samples,
        "output_checksum": checksum,
        "input_dtype": dtype_name(left),
        "weight_dtype": dtype_name(right),
        "output_dtype": dtype_name(output),
        "input_gradient_dtype": dtype_name(left.grad),
        "weight_gradient_dtype": dtype_name(right.grad),
        "input_storage_bytes": storage_bytes(left),
        "weight_storage_bytes": storage_bytes(right),
        "output_storage_bytes": storage_bytes(output),
        "input_gradient_storage_bytes": storage_bytes(left.grad),
        "weight_gradient_storage_bytes": storage_bytes(right.grad),
    }


def torch_dtype(torch: Any, dtype_name_value: str) -> Any:
    return {
        "float32": torch.float32,
        "float16": torch.float16,
        "bfloat16": torch.bfloat16,
    }[dtype_name_value]


def dtype_unavailable_reason(torch: Any, dtype_name_value: str) -> str | None:
    if dtype_name_value == "bfloat16" and not torch.cuda.is_bf16_supported():
        return "torch.cuda.is_bf16_supported() is false"
    return None


def run_performance_case(
    torch: Any,
    state: dict[str, Any],
    device: Any,
    dtype_name_value: str,
    warmup: int,
    runs: int,
) -> dict[str, Any]:
    unavailable = dtype_unavailable_reason(torch, dtype_name_value)
    if unavailable:
        return {
            "status": "unavailable",
            "reason": unavailable,
            "compute_dtype": dtype_name_value,
        }
    dtype = torch_dtype(torch, dtype_name_value)
    try:
        left = state["input"].to(device=device, dtype=dtype)
        right = state["weight"].to(device=device, dtype=dtype)
        seed = state["seed"].to(device=device, dtype=dtype)
        forward = timed_forward(
            torch, left.detach(), right.detach(), warmup, runs
        )

        training_left = left.detach().clone().requires_grad_(True)
        training_right = right.detach().clone().requires_grad_(True)
        forward_backward = timed_forward_backward(
            torch, training_left, training_right, seed, warmup, runs
        )
        return {
            "status": "ok",
            "compute_dtype": dtype_name_value,
            "storage_dtype": dtype_name(left),
            "storage_model": "native input-weight-output-gradient storage",
            "forward": forward,
            "forward_backward": forward_backward,
        }
    except RuntimeError as exc:
        return {
            "status": "unavailable",
            "reason": str(exc),
            "compute_dtype": dtype_name_value,
        }


def run_correctness_case(
    torch: Any, state: dict[str, Any], device: Any, dtype_name_value: str
) -> dict[str, Any]:
    unavailable = dtype_unavailable_reason(torch, dtype_name_value)
    if unavailable:
        return {
            "status": "unavailable",
            "reason": unavailable,
            "compute_dtype": dtype_name_value,
        }
    dtype = torch_dtype(torch, dtype_name_value)
    try:
        left = (
            state["correctness_input"]
            .to(device=device, dtype=dtype)
            .requires_grad_(True)
        )
        right = (
            state["correctness_weight"]
            .to(device=device, dtype=dtype)
            .requires_grad_(True)
        )
        seed = state["correctness_seed"].to(device=device, dtype=dtype)
        output = torch.matmul(left, right)
        output.backward(seed)
        torch.cuda.synchronize(device)
        assert left.grad is not None and right.grad is not None
        output_fingerprint = tensor_fingerprint(output)
        input_gradient_fingerprint = tensor_fingerprint(left.grad)
        weight_gradient_fingerprint = tensor_fingerprint(right.grad)
        return {
            "status": "ok",
            "compute_dtype": dtype_name_value,
            "shape": list(output.shape),
            "output": output_fingerprint,
            "input_gradient": input_gradient_fingerprint,
            "weight_gradient": weight_gradient_fingerprint,
            "training_fingerprint": {
                "output": output_fingerprint,
                "input_gradient": input_gradient_fingerprint,
                "weight_gradient": weight_gradient_fingerprint,
            },
            "tensor_contract": {
                "input_dtype": dtype_name(left),
                "weight_dtype": dtype_name(right),
                "output_dtype": dtype_name(output),
                "input_gradient_dtype": dtype_name(left.grad),
                "weight_gradient_dtype": dtype_name(right.grad),
                "input_storage_bytes": storage_bytes(left),
                "weight_storage_bytes": storage_bytes(right),
                "output_storage_bytes": storage_bytes(output),
                "input_gradient_storage_bytes": storage_bytes(left.grad),
                "weight_gradient_storage_bytes": storage_bytes(right.grad),
            },
        }
    except RuntimeError as exc:
        return {
            "status": "unavailable",
            "reason": str(exc),
            "compute_dtype": dtype_name_value,
        }


def optional_device_property(properties: Any, *names: str) -> str | None:
    for name in names:
        value = getattr(properties, name, None)
        if value is None:
            continue
        if isinstance(value, bytes):
            return value.hex()
        return str(value)
    return None


def base_runtime_report(previous_tf32_override: str | None) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "python_version": sys.version,
        "python_executable": sys.executable,
        "python_implementation": platform.python_implementation(),
        "platform": platform.platform(),
        "nvidia_tf32_override": os.environ.get("NVIDIA_TF32_OVERRIDE"),
        "nvidia_tf32_override_before_helper": previous_tf32_override,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs")
    parser.add_argument("--matrix-size", type=int, required=True)
    parser.add_argument("--correctness-size", type=int, default=16)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--device-index", type=int, default=0)
    args = parser.parse_args()

    if args.matrix_size < 128 or args.matrix_size % 16 != 0:
        raise SystemExit("--matrix-size must be at least 128 and divisible by 16")
    if args.correctness_size < 16 or args.correctness_size % 16 != 0:
        raise SystemExit(
            "--correctness-size must be at least 16 and divisible by 16"
        )
    if args.warmup < 0 or args.runs < 1:
        raise SystemExit("--warmup must be non-negative and --runs positive")

    # This must be set before importing torch/cuBLAS. The PowerShell driver also
    # sets it for the Sura process and restores the caller's previous value.
    previous_tf32_override = os.environ.get("NVIDIA_TF32_OVERRIDE")
    os.environ["NVIDIA_TF32_OVERRIDE"] = "0"
    runtime = base_runtime_report(previous_tf32_override)

    try:
        import torch
        from safetensors.torch import load_file
    except ImportError as exc:
        runtime.update({"status": "unavailable", "reason": str(exc)})
        print(json.dumps(runtime, ensure_ascii=False, allow_nan=False))
        return 0

    runtime["torch_version"] = torch.__version__
    runtime["cuda_runtime_version"] = torch.version.cuda
    runtime["torch_git_version"] = getattr(torch.version, "git_version", None)
    if not torch.cuda.is_available():
        runtime.update({
            "status": "unavailable",
            "reason": "torch.cuda.is_available() is false",
        })
        print(json.dumps(runtime, ensure_ascii=False, allow_nan=False))
        return 0
    if args.device_index < 0 or args.device_index >= torch.cuda.device_count():
        runtime.update({
            "status": "unavailable",
            "reason": f"CUDA device index {args.device_index} is unavailable",
        })
        print(json.dumps(runtime, ensure_ascii=False, allow_nan=False))
        return 0

    # Keep float32 GEMM in PyTorch's highest/IEEE policy and disable optional
    # reduced-precision accumulation for native f16/bf16 matrices. The report
    # records requested and effective values instead of assuming success.
    torch.set_float32_matmul_precision("highest")
    fp32_matmul_policy = "highest"
    try:
        torch.backends.cuda.matmul.fp32_precision = "ieee"
        fp32_matmul_policy = str(torch.backends.cuda.matmul.fp32_precision)
    except (AttributeError, RuntimeError):
        torch.backends.cuda.matmul.allow_tf32 = False
        fp32_matmul_policy = "allow_tf32=false"
    reduced_precision_reduction: dict[str, bool] = {}
    for attribute in (
        "allow_fp16_reduced_precision_reduction",
        "allow_bf16_reduced_precision_reduction",
    ):
        if hasattr(torch.backends.cuda.matmul, attribute):
            setattr(torch.backends.cuda.matmul, attribute, False)
            reduced_precision_reduction[attribute] = bool(
                getattr(torch.backends.cuda.matmul, attribute)
            )

    device = torch.device(f"cuda:{args.device_index}")
    properties = torch.cuda.get_device_properties(device)
    state = load_file(args.inputs, device="cpu")
    required_tensors = {
        "input": args.matrix_size,
        "weight": args.matrix_size,
        "seed": args.matrix_size,
        "correctness_input": args.correctness_size,
        "correctness_weight": args.correctness_size,
        "correctness_seed": args.correctness_size,
    }
    for name, dimension in required_tensors.items():
        expected_shape = (dimension, dimension)
        if name not in state:
            raise SystemExit(f"shared Safetensors file is missing {name}")
        if tuple(state[name].shape) != expected_shape or state[name].dtype != torch.float32:
            raise SystemExit(
                f"{name} must be float32 with shape {expected_shape}, got "
                f"{state[name].dtype} {tuple(state[name].shape)}"
            )

    checksums = {
        name: float(state[name].double().sum().item())
        for name in ("input", "weight", "seed")
    }
    correctness_checksums = {
        short_name: float(state[f"correctness_{short_name}"].double().sum().item())
        for short_name in ("input", "weight", "seed")
    }

    dtype_names = ("float32", "float16", "bfloat16")
    cases: dict[str, Any] = {}
    for dtype_name_value in dtype_names:
        cases[dtype_name_value] = run_performance_case(
            torch, state, device, dtype_name_value, args.warmup, args.runs
        )
        torch.cuda.empty_cache()

    correctness: dict[str, Any] = {}
    for dtype_name_value in dtype_names:
        correctness[dtype_name_value] = run_correctness_case(
            torch, state, device, dtype_name_value
        )
        torch.cuda.empty_cache()

    unavailable_cases = [
        f"performance.{name}: {entry.get('reason', entry.get('status'))}"
        for name, entry in cases.items()
        if entry.get("status") != "ok"
    ] + [
        f"correctness.{name}: {entry.get('reason', entry.get('status'))}"
        for name, entry in correctness.items()
        if entry.get("status") != "ok"
    ]
    all_cases_ok = not unavailable_cases
    runtime.update({
        "status": "ok" if all_cases_ok else "partial",
        "reason": None if all_cases_ok else "; ".join(unavailable_cases),
        "device": str(device),
        "device_index": args.device_index,
        "device_name": torch.cuda.get_device_name(device),
        "device_uuid": optional_device_property(properties, "uuid"),
        "pci_bus_id": optional_device_property(
            properties, "pci_bus_id", "_pci_bus_id"
        ),
        "compute_capability": ".".join(
            str(value) for value in torch.cuda.get_device_capability(device)
        ),
        "total_memory_bytes": int(properties.total_memory),
        "cudnn_version": torch.backends.cudnn.version(),
        "matrix_size": args.matrix_size,
        "correctness_size": args.correctness_size,
        "aligned_multiple": 16,
        "warmup": args.warmup,
        "runs": args.runs,
        "source_dtype": "float32",
        "same_inputs_file": True,
        "input_file_sha256": sha256_file(args.inputs),
        "input_checksums": checksums,
        "correctness_input_checksums": correctness_checksums,
        "timing_scope": (
            "synchronized eager forward/autograd wall-clock; includes Python/"
            "PyTorch dispatch, allocation, graph work, and ending synchronization; "
            "not a pure GEMM-kernel timer"
        ),
        "setup_and_upload_excluded": True,
        "process_startup_excluded": True,
        "correctness_observation_excluded": True,
        "synchronization_inside_each_sample": True,
        "gradient_zeroing_timed": False,
        "float32_matmul_policy": fp32_matmul_policy,
        "float32_matmul_precision": torch.get_float32_matmul_precision(),
        "allow_tf32_effective": bool(torch.backends.cuda.matmul.allow_tf32),
        "reduced_precision_reduction": reduced_precision_reduction,
        "environment": {
            "NVIDIA_TF32_OVERRIDE": os.environ.get("NVIDIA_TF32_OVERRIDE"),
            "CUDA_VISIBLE_DEVICES": os.environ.get("CUDA_VISIBLE_DEVICES"),
            "CUBLAS_WORKSPACE_CONFIG": os.environ.get("CUBLAS_WORKSPACE_CONFIG"),
        },
        "cases": cases,
        "correctness": correctness,
    })
    print(json.dumps(runtime, ensure_ascii=False, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
