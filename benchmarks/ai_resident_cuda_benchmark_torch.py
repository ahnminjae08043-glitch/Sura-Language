#!/usr/bin/env python3
"""PyTorch companion for Sura's GPU-resident forward/training benchmark."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import time


def percentile95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * 0.95) - 1)]


def timed_forward(torch, input_tensor, weight, warmup: int, runs: int) -> dict:
    output = None
    with torch.no_grad():
        for _ in range(warmup):
            output = torch.relu(torch.matmul(input_tensor, weight))
        torch.cuda.synchronize()
        torch.cuda.reset_peak_memory_stats()
        samples = []
        for _ in range(runs):
            torch.cuda.synchronize()
            started = time.perf_counter()
            output = torch.relu(torch.matmul(input_tensor, weight))
            torch.cuda.synchronize()
            samples.append((time.perf_counter() - started) * 1000.0)
        allocated = torch.cuda.memory_allocated()
        peak_allocated = torch.cuda.max_memory_allocated()
        checksum = float(output.float().sum().cpu().item())
    return {
        "status": "ok",
        "workload": "relu(matmul(input, weight))",
        "median_ms": statistics.median(samples),
        "p95_ms": percentile95(samples),
        "raw_ms": samples,
        "final_checksum": checksum,
        "allocated_bytes_before_read": allocated,
        "peak_allocated_bytes": peak_allocated,
    }


def timed_training(torch, input_tensor, target, weight, warmup: int,
                   runs: int, learning_rate: float) -> dict:
    optimizer = torch.optim.SGD([weight], lr=learning_rate)
    loss = None

    def step() -> None:
        nonlocal loss
        optimizer.zero_grad(set_to_none=False)
        prediction = torch.relu(torch.matmul(input_tensor, weight))
        loss = torch.mean((prediction - target) ** 2)
        loss.backward()
        optimizer.step()

    for _ in range(warmup):
        step()
    torch.cuda.synchronize()
    torch.cuda.reset_peak_memory_stats()
    samples = []
    for _ in range(runs):
        torch.cuda.synchronize()
        started = time.perf_counter()
        step()
        torch.cuda.synchronize()
        samples.append((time.perf_counter() - started) * 1000.0)
    allocated = torch.cuda.memory_allocated()
    peak_allocated = torch.cuda.max_memory_allocated()
    final_loss = float(loss.detach().float().cpu().item())
    return {
        "status": "ok",
        "workload": "zero_grad+matmul+relu+mse+backward+sgd",
        "optimizer": "sgd",
        "learning_rate": learning_rate,
        "median_step_ms": statistics.median(samples),
        "p95_step_ms": percentile95(samples),
        "raw_step_ms": samples,
        "final_loss": final_loss,
        "allocated_bytes_before_read": allocated,
        "peak_allocated_bytes": peak_allocated,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs")
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--learning-rate", type=float, default=0.0001)
    parser.add_argument("--device-index", type=int, default=0)
    args = parser.parse_args()
    try:
        import torch
        from safetensors.torch import load_file
    except ImportError as exc:
        print(json.dumps({"status": "unavailable", "reason": str(exc)}))
        return 0
    if not torch.cuda.is_available():
        print(json.dumps({
            "status": "unavailable",
            "reason": "torch.cuda.is_available() is false",
            "torch_version": torch.__version__,
        }))
        return 0

    if args.device_index < 0 or args.device_index >= torch.cuda.device_count():
        raise SystemExit(
            f"--device-index {args.device_index} is outside the available CUDA device range"
        )
    device = torch.device(f"cuda:{args.device_index}")
    state = load_file(args.inputs, device="cpu")
    input_tensor = state["input"].to(device)
    initial_weight = state["weight"].to(device)
    target = state["target"].to(device)
    with torch.no_grad():
        initial_checksum = float(
            torch.relu(torch.matmul(input_tensor, initial_weight)).float().sum().cpu().item()
        )

    forward_weight = initial_weight.detach().clone()
    training_weight = initial_weight.detach().clone().requires_grad_(True)
    report = {
        "status": "ok",
        "torch_version": torch.__version__,
        "device": str(device),
        "device_name": torch.cuda.get_device_name(device),
        "dtype": "float32",
        "same_inputs": True,
        "initial_checksum": initial_checksum,
        "forward": timed_forward(
            torch, input_tensor, forward_weight, args.warmup, args.runs
        ),
        "training": timed_training(
            torch, input_tensor, target, training_weight, args.warmup,
            args.runs, args.learning_rate
        ),
    }
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
