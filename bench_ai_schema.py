import json
import re
import time


AGENT_SCHEMA = {
    "type": "dict",
    "required": ["id", "status", "score", "steps", "usage", "meta"],
    "properties": {
        "id": {"type": "string", "pattern": r"^agent-[0-9]+$"},
        "status": {"enum": ["ok", "review", "blocked"]},
        "score": {"type": "integer", "min": 0, "max": 100},
        "steps": {
            "type": "array",
            "min_len": 2,
            "items": {
                "type": "dict",
                "required": ["tool", "approved", "cost"],
                "properties": {
                    "tool": {"enum": ["search", "http", "summarize"]},
                    "approved": {"type": "bool"},
                    "cost": {"type": "number", "min": 0, "max": 50},
                },
                "additional": False,
            },
        },
        "usage": {
            "type": "dict",
            "required": ["input_tokens", "output_tokens"],
            "properties": {
                "input_tokens": {"type": "integer", "min": 1, "max": 2000},
                "output_tokens": {"type": "integer", "min": 1, "max": 1000},
            },
            "additional": False,
        },
        "meta": {
            "type": "dict",
            "required": ["risk", "kind"],
            "properties": {
                "risk": {"type": "integer", "min": 0, "max": 10},
                "kind": {"enum": ["agent", "workflow"]},
            },
            "additional": False,
        },
    },
    "additional": False,
}


def type_name(value):
    if value is None:
        return "nil"
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int) and not isinstance(value, bool):
        return "integer"
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "dict"
    return type(value).__name__


def type_matches(value, expected):
    if expected == "nil":
        return value is None
    if expected == "bool":
        return isinstance(value, bool)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "string":
        return isinstance(value, str)
    if expected == "array":
        return isinstance(value, list)
    if expected == "dict":
        return isinstance(value, dict)
    return False


def child_path(path, key):
    if isinstance(key, int):
        return f"{path}[{key}]"
    return f"{path}.{key}"


def schema_errors(value, schema, path="$", errors=None):
    if errors is None:
        errors = []
    if isinstance(schema, str):
        if not type_matches(value, schema):
            errors.append(f"{path}: expected {schema}, got {type_name(value)}")
        return errors
    if not isinstance(schema, dict):
        errors.append(f"{path}: schema must be a type string or dict")
        return errors

    expected = schema.get("type")
    if expected is not None and not type_matches(value, expected):
        errors.append(f"{path}: expected {expected}, got {type_name(value)}")
        return errors
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: value is not in enum")
    if "min" in schema and (not isinstance(value, (int, float)) or isinstance(value, bool) or value < schema["min"]):
        errors.append(f"{path}: number is below min {schema['min']}")
    if "max" in schema and (not isinstance(value, (int, float)) or isinstance(value, bool) or value > schema["max"]):
        errors.append(f"{path}: number is above max {schema['max']}")
    if "min_len" in schema and (not hasattr(value, "__len__") or len(value) < schema["min_len"]):
        errors.append(f"{path}: length is below min_len {schema['min_len']}")
    if "max_len" in schema and (not hasattr(value, "__len__") or len(value) > schema["max_len"]):
        errors.append(f"{path}: length is above max_len {schema['max_len']}")
    if "pattern" in schema and (not isinstance(value, str) or re.search(schema["pattern"], value) is None):
        errors.append(f"{path}: string does not match pattern")
    if "items" in schema:
        if isinstance(value, list):
            for index, item in enumerate(value):
                schema_errors(item, schema["items"], child_path(path, index), errors)
        else:
            errors.append(f"{path}: expected array for items check, got {type_name(value)}")

    properties = schema.get("properties")
    required = schema.get("required", [])
    if properties is not None or required:
        if not isinstance(value, dict):
            errors.append(f"{path}: expected dict for properties check, got {type_name(value)}")
        else:
            for key in required:
                if key not in value:
                    errors.append(f"{child_path(path, key)}: missing required field")
            if isinstance(properties, dict):
                for key, child_schema in properties.items():
                    if key in value:
                        schema_errors(value[key], child_schema, child_path(path, key), errors)
                if schema.get("additional") is False:
                    for key in value:
                        if key not in properties:
                            errors.append(f"{child_path(path, key)}: unexpected field")
    return errors


def type_value(value):
    if value == "dict":
        return "object"
    if value == "array":
        return "array"
    if value == "integer":
        return "integer"
    if value == "number":
        return "number"
    if value == "string":
        return "string"
    if value == "bool":
        return "boolean"
    if value == "nil":
        return "null"
    return None


def schema_to_json_schema(schema, strict=True):
    if isinstance(schema, str):
        mapped = type_value(schema)
        return {"type": mapped} if mapped else {}
    if not isinstance(schema, dict):
        return schema
    out = {}
    if "type" in schema:
        mapped = type_value(schema["type"])
        if mapped:
            out["type"] = mapped
    if "enum" in schema:
        out["enum"] = schema["enum"]
    if "pattern" in schema:
        out["pattern"] = schema["pattern"]
    if "min" in schema:
        out["minimum"] = schema["min"]
    if "max" in schema:
        out["maximum"] = schema["max"]
    if "min_len" in schema:
        out["minLength"] = schema["min_len"]
    if "max_len" in schema:
        out["maxLength"] = schema["max_len"]
    if "items" in schema:
        out["items"] = schema_to_json_schema(schema["items"], strict)
    if "properties" in schema:
        out.setdefault("type", "object")
        out["properties"] = {key: schema_to_json_schema(child, strict) for key, child in schema["properties"].items()}
        if "required" not in schema and strict:
            out["required"] = list(schema["properties"].keys())
    if "required" in schema:
        out["required"] = schema["required"]
    if "additional" in schema:
        out["additionalProperties"] = schema["additional"]
    elif strict and "properties" in schema:
        out["additionalProperties"] = False
    return out


def step_tool(i, offset):
    code = (i + offset) % 3
    if code == 0:
        return "search"
    if code == 1:
        return "http"
    return "summarize"


def make_payload(i):
    status = "ok"
    if i % 11 == 0:
        status = "review"
    if i % 37 == 0:
        status = "blocked"
    kind = "workflow" if i % 7 == 0 else "agent"
    return {
        "id": "agent-" + str(i % 1000),
        "status": status,
        "score": i % 101,
        "steps": [
            {"tool": step_tool(i, 0), "approved": True, "cost": (i % 25) + 0.5},
            {"tool": step_tool(i, 1), "approved": i % 5 != 0, "cost": (i % 17) + 1},
        ],
        "usage": {
            "input_tokens": 100 + (i % 400),
            "output_tokens": 40 + (i % 200),
        },
        "meta": {"risk": i % 11, "kind": kind},
    }


def make_bad_payload(i):
    return {
        "id": "bad-" + str(i),
        "status": "unknown",
        "score": 150,
        "steps": [
            {"tool": "shell", "approved": "yes", "cost": 99},
        ],
        "usage": {"input_tokens": -1},
        "meta": {"risk": 99, "kind": "other"},
        "extra": True,
    }


runs = 5
items = 30_000
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    valid_count = 0
    invalid_count = 0
    error_count = 0
    i = 0
    while i < items:
        payload = make_bad_payload(i) if i % 8 == 0 else make_payload(i)
        errors = schema_errors(payload, AGENT_SCHEMA)
        if len(errors) == 0:
            valid_count += 1
        else:
            invalid_count += 1
            error_count += len(errors)
        if i % 64 == 0:
            checksum += len(json.dumps(schema_to_json_schema(AGENT_SCHEMA), separators=(",", ":")))
        i += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum += valid_count * 3 + invalid_count * 7 + error_count

avg = total_ms / runs
print(f"schema validation payloads: {items}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
