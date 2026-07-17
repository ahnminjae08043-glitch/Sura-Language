#!/usr/bin/env python3
"""Explicit PyTorch checkpoint <-> Safetensors bridge for Sura Language.

The native Sura runtime reads/writes Safetensors without Python. This helper
exists only for legacy .pt/.pth state_dict files and always requests PyTorch's
weights_only loader. Never convert an untrusted pickle checkpoint.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Dict


def dependencies():
    try:
        import torch  # type: ignore
        from safetensors.torch import load_file, save_file  # type: ignore
    except ImportError as exc:
        raise SystemExit(
            "PyTorch bridge unavailable: install 'torch' and 'safetensors'. "
            "Sura's native .safetensors APIs do not require these packages.\n"
            f"Missing dependency: {exc}"
        ) from exc
    return torch, load_file, save_file


def tensor_state_dict(value, torch) -> Dict[str, object]:
    if isinstance(value, dict) and "state_dict" in value and isinstance(value["state_dict"], dict):
        value = value["state_dict"]
    if not isinstance(value, dict) or not value:
        raise SystemExit("checkpoint must contain a non-empty tensor state_dict")
    result: Dict[str, object] = {}
    for name, tensor in value.items():
        if not isinstance(name, str) or not name:
            raise SystemExit("state_dict names must be non-empty strings")
        if not torch.is_tensor(tensor):
            raise SystemExit(f"state_dict entry {name!r} is not a tensor")
        if tensor.layout != torch.strided:
            raise SystemExit(f"state_dict entry {name!r} must use strided layout")
        result[name] = tensor.detach().cpu().contiguous()
    return result


def pt_to_safe(source: pathlib.Path, target: pathlib.Path) -> None:
    torch, _, save_file = dependencies()
    # weights_only avoids the general object unpickler, but users must still
    # treat the source as untrusted input and keep PyTorch patched.
    loaded = torch.load(source, map_location="cpu", weights_only=True)
    state = tensor_state_dict(loaded, torch)
    target.parent.mkdir(parents=True, exist_ok=True)
    save_file(state, str(target), metadata={"producer": "Sura PyTorch bridge"})


def safe_to_pt(source: pathlib.Path, target: pathlib.Path) -> None:
    torch, load_file, _ = dependencies()
    state = load_file(str(source), device="cpu")
    target.parent.mkdir(parents=True, exist_ok=True)
    torch.save(state, target)


def inspect(source: pathlib.Path) -> None:
    torch, load_file, _ = dependencies()
    state = load_file(str(source), device="cpu")
    records = []
    for name in sorted(state):
        tensor = state[name]
        records.append(
            {
                "name": name,
                "shape": list(tensor.shape),
                "dtype": str(tensor.dtype).removeprefix("torch."),
                "numel": tensor.numel(),
            }
        )
    print(json.dumps({"schema": "sura.torch-bridge.inspect.v1", "tensors": records}, ensure_ascii=False))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("pt-to-safetensors", "safetensors-to-pt"):
        command = sub.add_parser(name)
        command.add_argument("source", type=pathlib.Path)
        command.add_argument("target", type=pathlib.Path)
    inspect_parser = sub.add_parser("inspect")
    inspect_parser.add_argument("source", type=pathlib.Path)
    args = parser.parse_args()
    if args.command == "pt-to-safetensors":
        pt_to_safe(args.source, args.target)
    elif args.command == "safetensors-to-pt":
        safe_to_pt(args.source, args.target)
    else:
        inspect(args.source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
