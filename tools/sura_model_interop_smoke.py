#!/usr/bin/env python3
"""Validate Sura weight interchange against official Python libraries.

Dependencies are intentionally optional developer tools:
    python -m pip install torch safetensors onnx
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


def run(command: list[str], env: dict[str, str]) -> str:
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    if completed.returncode != 0:
        raise SystemExit(
            f"command failed ({completed.returncode}): {command}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=pathlib.Path)
    parser.add_argument("--json-out", type=pathlib.Path)
    args = parser.parse_args()

    try:
        import onnx
        import safetensors
        import torch
        from onnx import TensorProto, checker, helper
        from safetensors.torch import load_file
    except ImportError as exc:
        raise SystemExit(
            "interop smoke requires optional developer dependencies: "
            "python -m pip install torch safetensors onnx\n"
            f"missing dependency: {exc}"
        ) from exc

    root = pathlib.Path(__file__).resolve().parents[1]
    engine = args.engine.resolve()
    if not engine.is_file():
        raise SystemExit(f"Sura engine not found: {engine}")

    export_fixture = root / "tools" / "fixtures" / "onnx_export_fixture.sura"
    onnx_import_fixture = root / "tools" / "fixtures" / "onnx_import_fixture.sura"
    safe_import_fixture = root / "tools" / "fixtures" / "safetensors_import_fixture.sura"
    bridge = root / "tools" / "sura_torch_bridge.py"

    with tempfile.TemporaryDirectory(prefix="sura-model-interop-") as temporary:
        directory = pathlib.Path(temporary)
        sura_onnx = directory / "sura.onnx"
        sura_safe = directory / "sura.safetensors"
        bridge_pt = directory / "bridge.pt"
        bridge_safe = directory / "bridge.safetensors"
        external_onnx = directory / "external.onnx"

        environment = os.environ.copy()
        environment["SURA_ONNX_FIXTURE"] = str(sura_onnx)
        environment["SURA_SAFE_FIXTURE"] = str(sura_safe)
        run([str(engine), str(export_fixture)], environment)

        model = onnx.load(str(sura_onnx))
        checker.check_model(model, full_check=True)
        state = load_file(str(sura_safe), device="cpu")
        expected_dtypes = {
            "f32": torch.float32,
            "f16": torch.float16,
            "bf16": torch.bfloat16,
            "f64": torch.float64,
        }
        if {name: tensor.dtype for name, tensor in state.items()} != expected_dtypes:
            raise SystemExit("official Safetensors reader observed unexpected Sura dtypes")

        run(
            [sys.executable, str(bridge), "safetensors-to-pt", str(sura_safe), str(bridge_pt)],
            environment,
        )
        run(
            [sys.executable, str(bridge), "pt-to-safetensors", str(bridge_pt), str(bridge_safe)],
            environment,
        )
        environment["SURA_SAFE_FIXTURE"] = str(bridge_safe)
        run([str(engine), str(safe_import_fixture)], environment)

        external_f32 = helper.make_tensor(
            "external_f32",
            TensorProto.FLOAT,
            [2, 2],
            struct.pack("<4f", 1.0, 2.0, 3.0, 4.0),
            raw=True,
        )
        external_f64 = helper.make_tensor(
            "external_f64",
            TensorProto.DOUBLE,
            [2],
            struct.pack("<2d", 0.25, -0.5),
            raw=True,
        )
        nodes = [
            helper.make_node("Identity", ["external_f32"], ["external_f32_out"]),
            helper.make_node("Identity", ["external_f64"], ["external_f64_out"]),
        ]
        outputs = [
            helper.make_tensor_value_info("external_f32_out", TensorProto.FLOAT, [2, 2]),
            helper.make_tensor_value_info("external_f64_out", TensorProto.DOUBLE, [2]),
        ]
        graph = helper.make_graph(
            nodes,
            "official-python-weights",
            [],
            outputs,
            initializer=[external_f32, external_f64],
        )
        official_model = helper.make_model(
            graph,
            producer_name="Sura interop smoke",
            opset_imports=[helper.make_opsetid("", 18)],
        )
        checker.check_model(official_model, full_check=True)
        onnx.save(official_model, str(external_onnx))
        environment["SURA_ONNX_FIXTURE"] = str(external_onnx)
        run([str(engine), str(onnx_import_fixture)], environment)

    report = {
        "schema": "sura.model-interop-smoke.v1",
        "status": "pass",
        "generated_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "engine": engine.name,
        "engine_sha256": hashlib.sha256(engine.read_bytes()).hexdigest(),
        "python_version": sys.version.split()[0],
        "torch_version": torch.__version__,
        "safetensors_version": safetensors.__version__,
        "onnx_version": onnx.__version__,
        "validated": [
            "Sura -> Safetensors official reader",
            "Safetensors -> PyTorch -> Safetensors -> Sura",
            "Sura -> ONNX checker full_check",
            "official ONNX initializer model -> Sura",
        ],
    }
    encoded = json.dumps(report, ensure_ascii=False, indent=2)
    if args.json_out:
        target = args.json_out.resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
