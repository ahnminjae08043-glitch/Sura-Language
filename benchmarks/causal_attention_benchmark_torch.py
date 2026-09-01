#!/usr/bin/env python3
"""PyTorch companion for Sura's public causal-attention benchmark."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import time
from typing import Any


def percentile95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * 0.95) - 1)]


def gradient_fingerprint(loss: Any, q: Any, k: Any, v: Any) -> dict[str, float]:
    def stats(tensor: Any) -> tuple[float, float]:
        gradient = tensor.grad.detach().double()
        return float(gradient.sum().cpu().item()), float(gradient.abs().sum().cpu().item())

    q_sum, q_l1 = stats(q)
    k_sum, k_l1 = stats(k)
    v_sum, v_l1 = stats(v)
    return {
        "loss": float(loss.detach().float().cpu().item()),
        "q_grad_sum": q_sum,
        "q_grad_l1": q_l1,
        "k_grad_sum": k_sum,
        "k_grad_l1": k_l1,
        "v_grad_sum": v_sum,
        "v_grad_l1": v_l1,
    }


def run_device(torch: Any, functional: Any, q_source: Any, k_source: Any,
               v_source: Any, device: Any, warmup: int, runs: int) -> dict[str, Any]:
    q = q_source.to(device).detach().clone().requires_grad_(True)
    k = k_source.to(device).detach().clone().requires_grad_(True)
    v = v_source.to(device).detach().clone().requires_grad_(True)
    tensors = (q, k, v)
    output = None
    loss = None

    def clear_gradients() -> None:
        with torch.no_grad():
            for tensor in tensors:
                if tensor.grad is not None:
                    tensor.grad.zero_()

    def forward_backward() -> None:
        nonlocal output, loss
        output = functional.scaled_dot_product_attention(
            q, k, v, dropout_p=0.0, is_causal=True
        )
        loss = output.sum()
        loss.backward()

    for _ in range(warmup):
        clear_gradients()
        forward_backward()
    if device.type == "cuda":
        torch.cuda.synchronize(device)
        torch.cuda.reset_peak_memory_stats(device)

    samples: list[float] = []
    for _ in range(runs):
        # Match Sura: persistent gradient clearing is required for each sample,
        # but it is outside the timed forward+sum+backward region.
        clear_gradients()
        if device.type == "cuda":
            torch.cuda.synchronize(device)
        started = time.perf_counter()
        forward_backward()
        if device.type == "cuda":
            torch.cuda.synchronize(device)
        samples.append((time.perf_counter() - started) * 1000.0)

    allocated = None
    peak_allocated = None
    if device.type == "cuda":
        allocated = int(torch.cuda.memory_allocated(device))
        peak_allocated = int(torch.cuda.max_memory_allocated(device))
    fingerprint = gradient_fingerprint(loss, q, k, v)
    result: dict[str, Any] = {
        "status": "ok",
        "device": str(device),
        "backend": "torch.nn.functional.scaled_dot_product_attention(auto)",
        "workload": "causal_attention+sum+backward",
        "median_ms": statistics.median(samples),
        "p95_ms": percentile95(samples),
        "raw_ms": samples,
        "fingerprint": fingerprint,
        "allocated_bytes_before_read": allocated,
        "peak_allocated_bytes": peak_allocated,
    }
    if device.type == "cuda":
        result["device_name"] = torch.cuda.get_device_name(device)
    return result


def unavailable(reason: str, **extra: Any) -> dict[str, Any]:
    result: dict[str, Any] = {"status": "unavailable", "reason": reason}
    result.update(extra)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs")
    parser.add_argument("--batch", type=int, required=True)
    parser.add_argument("--heads", type=int, required=True)
    parser.add_argument("--sequence", type=int, required=True)
    parser.add_argument("--head-dim", type=int, required=True)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--cpu-threads", type=int, default=1)
    parser.add_argument("--device-index", type=int, default=0)
    args = parser.parse_args()

    if min(args.batch, args.heads, args.sequence, args.head_dim, args.runs,
           args.cpu_threads) < 1:
        raise SystemExit("shape, runs, and --cpu-threads must be positive")
    if args.warmup < 0:
        raise SystemExit("--warmup must be non-negative")

    try:
        import torch
        import torch.nn.functional as functional
    except ImportError as exc:
        print(json.dumps(unavailable(f"PyTorch import skipped: {exc}")))
        return 0

    if not hasattr(functional, "scaled_dot_product_attention"):
        print(json.dumps(unavailable(
            "PyTorch scaled_dot_product_attention is unavailable",
            torch_version=torch.__version__,
        )))
        return 0

    try:
        from safetensors.torch import load_file
    except ImportError as exc:
        print(json.dumps(unavailable(
            f"PyTorch comparison skipped because safetensors is unavailable: {exc}",
            torch_version=torch.__version__,
        )))
        return 0

    torch.set_num_threads(args.cpu_threads)
    expected_shape = (args.batch, args.heads, args.sequence, args.head_dim)
    try:
        state = load_file(args.inputs, device="cpu")
        q_source, k_source, v_source = state["q"], state["k"], state["v"]
    except Exception as exc:  # The report must retain Sura results on companion failure.
        print(json.dumps({
            "status": "error",
            "reason": f"failed to load shared inputs: {exc}",
            "torch_version": torch.__version__,
        }))
        return 0

    for name, tensor in (("q", q_source), ("k", k_source), ("v", v_source)):
        if tuple(tensor.shape) != expected_shape:
            print(json.dumps({
                "status": "error",
                "reason": f"{name} shape {tuple(tensor.shape)} != {expected_shape}",
                "torch_version": torch.__version__,
            }))
            return 0
        if tensor.dtype != torch.float32:
            print(json.dumps({
                "status": "error",
                "reason": f"{name} dtype {tensor.dtype} is not float32",
                "torch_version": torch.__version__,
            }))
            return 0

    input_checksum = float(
        q_source.double().sum().item()
        + k_source.double().sum().item()
        + v_source.double().sum().item()
    )
    report: dict[str, Any] = {
        "status": "ok",
        "torch_version": torch.__version__,
        "cpu_threads": torch.get_num_threads(),
        "same_inputs": True,
        "shape": {
            "batch": args.batch,
            "heads": args.heads,
            "sequence": args.sequence,
            "head_dim": args.head_dim,
        },
        "layout": "B,H,T,D",
        "dtype": "float32",
        "causal": True,
        "scale": "1/sqrt(head_dim)",
        "loss_reduction": "sum",
        "transpose_included": False,
        "projections_included": False,
        "warmup": args.warmup,
        "runs": args.runs,
        "input_checksum": input_checksum,
        "cpu": run_device(
            torch, functional, q_source, k_source, v_source,
            torch.device("cpu"), args.warmup, args.runs,
        ),
        "cuda": unavailable("torch.cuda.is_available() is false"),
    }
    if torch.cuda.is_available():
        if args.device_index < 0 or args.device_index >= torch.cuda.device_count():
            report["cuda"] = unavailable(
                f"device index {args.device_index} is outside the available CUDA range"
            )
        else:
            device = torch.device(f"cuda:{args.device_index}")
            report["cuda"] = run_device(
                torch, functional, q_source, k_source, v_source,
                device, args.warmup, args.runs,
            )

    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
