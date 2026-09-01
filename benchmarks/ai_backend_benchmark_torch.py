#!/usr/bin/env python3
"""PyTorch side of Sura's same-hardware, same-input matmul benchmark."""

from __future__ import annotations

import argparse
import json
import statistics
import time


def percentile95(values):
    ordered = sorted(values)
    return ordered[max(0, int(len(ordered) * 0.95 + 0.999999) - 1)]


def run(left, right, torch, device, warmup, runs):
    left = left.to(device)
    right = right.to(device)
    for _ in range(warmup):
        torch.matmul(left, right)
    if device.type == "cuda":
        torch.cuda.synchronize(device)
    samples = []
    output = None
    for _ in range(runs):
        start = time.perf_counter()
        output = torch.matmul(left, right)
        if device.type == "cuda":
            torch.cuda.synchronize(device)
        samples.append((time.perf_counter() - start) * 1000.0)
    return {
        "status": "ok",
        "device": str(device),
        "median_ms": statistics.median(samples),
        "p95_ms": percentile95(samples),
        "raw_ms": samples,
        "checksum": float(output.double().sum().cpu().item()),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs")
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--runs", type=int, default=12)
    parser.add_argument("--cpu-threads", type=int, default=1)
    args = parser.parse_args()
    try:
        import torch
        from safetensors.torch import load_file
    except ImportError as exc:
        print(json.dumps({"status": "unavailable", "reason": str(exc)}))
        return 0
    if args.cpu_threads < 1:
        raise SystemExit("--cpu-threads must be at least 1")
    torch.set_num_threads(args.cpu_threads)
    state = load_file(args.inputs, device="cpu")
    left, right = state["left"], state["right"]
    report = {
        "status": "ok",
        "torch_version": torch.__version__,
        "cpu_threads": torch.get_num_threads(),
        "same_inputs": True,
        "cpu": run(left, right, torch, torch.device("cpu"), args.warmup, args.runs),
        "cuda": {"status": "unavailable", "reason": "torch.cuda.is_available() is false"},
    }
    if torch.cuda.is_available():
        report["cuda"] = run(left, right, torch, torch.device("cuda:0"), args.warmup, args.runs)
        report["cuda"]["device_name"] = torch.cuda.get_device_name(0)
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
