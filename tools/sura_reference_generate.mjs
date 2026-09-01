import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(scriptDir, "..");
const siteRoot = path.join(root, "sura_presentation");
const checkOnly = process.argv.includes("--check");
const versionContract = JSON.parse(fs.readFileSync(path.join(root, "version.json"), "utf8"));
if (versionContract.schema !== "sura.version.v1" ||
    !/^\d+\.\d+\.\d+$/.test(versionContract.version) ||
    versionContract.series !== versionContract.version.split(".").slice(0, 2).join(".")) {
  throw new Error("version.json does not satisfy sura.version.v1");
}
const expectedVersion = versionContract.version;
const targetSeries = versionContract.series;
const compatibilityContract = JSON.parse(fs.readFileSync(path.join(root, "compatibility.json"), "utf8"));
if (compatibilityContract.schema !== "sura.compatibility.v1" ||
    compatibilityContract.language_version !== expectedVersion ||
    compatibilityContract.stable_series !== targetSeries ||
    !compatibilityContract.source?.historical_probes?.length ||
    !compatibilityContract.support_tiers?.stable?.length ||
    !compatibilityContract.support_tiers?.platform_limited?.length ||
    !compatibilityContract.support_tiers?.experimental?.length) {
  throw new Error("compatibility.json does not match the reference version or support-tier contract");
}
const apiPath = path.join(root, "build", "reference-docs", "api.json");
const cssPath = path.join(siteRoot, "public", "reference.css");
const targetAuditPath = path.join(root, "artifacts", "target_lowering_audit.json");
const goalAuditPath = path.join(root, "artifacts", "goal_audit.json");
const nativePerformancePath = path.join(root, "artifacts", "native_perf.json");
const releaseEvidencePath = path.join(root, "artifacts", "release_evidence.json");
const releaseManifestPath = path.join(siteRoot, "public", "downloads", `release-${expectedVersion}.json`);
const verificationManifestPath = path.join(siteRoot, "public", "downloads", `verification-${expectedVersion}.json`);
const verificationManifest = JSON.parse(fs.readFileSync(verificationManifestPath, "utf8"));

function selectVerifiedBinary(envName, fileName, expectedBytes, expectedSha256) {
  if (process.env[envName]) return path.resolve(root, process.env[envName]);
  const candidates = [
    path.join(root, fileName),
    path.join(root, "dist", "SuraLanguage-windows-x64", "payload", fileName),
    path.join(root, "build", `release-verify-${expectedVersion}-final`, "zip", "payload", fileName),
  ];
  return candidates.find((candidate) => {
    if (!fs.existsSync(candidate) || fs.statSync(candidate).size !== expectedBytes) return false;
    return crypto.createHash("sha256").update(fs.readFileSync(candidate)).digest("hex") === expectedSha256;
  }) || candidates[0];
}

const engine = selectVerifiedBinary(
  "SURA_REFERENCE_ENGINE",
  verificationManifest.engine_file,
  verificationManifest.engine_bytes,
  verificationManifest.engine_sha256,
);
const packageManager = selectVerifiedBinary(
  "SURA_REFERENCE_PACKAGE_MANAGER",
  verificationManifest.package_helper.file,
  verificationManifest.package_helper.bytes,
  verificationManifest.package_helper.sha256,
);

function run(file, args) {
  const result = spawnSync(file, args, {
    cwd: root,
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.status !== 0) {
    throw new Error((result.stderr || result.stdout || "command failed").trim());
  }
  return result.stdout.trim();
}

function parseJsonCommand(file, args) {
  return JSON.parse(run(file, args));
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function code(value) {
  return "<code>" + escapeHtml(value) + "</code>";
}

function pre(value) {
  return "<pre><code>" + escapeHtml(value) + "</code></pre>";
}

function paragraph(value) {
  return "<p>" + value + "</p>";
}

function list(items) {
  return "<ul>" + items.map((item) => "<li>" + item + "</li>").join("") + "</ul>";
}

function table(headers, rows) {
  const head = "<thead><tr>" + headers.map((item) => "<th>" + item + "</th>").join("") + "</tr></thead>";
  const body = "<tbody>" + rows.map((row) => "<tr>" + row.map((item) => "<td>" + item + "</td>").join("") + "</tr>").join("") + "</tbody>";
  return "<div class='table-wrap'><table>" + head + body + "</table></div>";
}

function section(id, title, body) {
  return "<section id='" + id + "'><h2>" + title + "</h2>" + body + "</section>";
}

const versionOutput = run(engine, ["--version"]);
const versionMatch = versionOutput.match(/([0-9]+\.[0-9]+\.[0-9]+)/);
if (!versionMatch) throw new Error("could not read Sura version");
const version = versionMatch[1];
if (version !== expectedVersion) throw new Error(`reference requires Sura ${expectedVersion}, found ${version}`);
const engineSha256 = crypto.createHash("sha256").update(fs.readFileSync(engine)).digest("hex");

const runtimeHelp = run(engine, ["--help"]);
const packageHelp = run(packageManager, ["--help"]);
const catalog = parseJsonCommand(packageManager, ["list", "--json"]);
const api = JSON.parse(fs.readFileSync(apiPath, "utf8"));
const targetAudit = JSON.parse(fs.readFileSync(targetAuditPath, "utf8"));
const goalAudit = JSON.parse(fs.readFileSync(goalAuditPath, "utf8"));
const nativePerformance = JSON.parse(fs.readFileSync(nativePerformancePath, "utf8"));
const releaseEvidence = JSON.parse(fs.readFileSync(releaseEvidencePath, "utf8"));
const releaseManifest = JSON.parse(fs.readFileSync(releaseManifestPath, "utf8"));
if (releaseManifest.version !== version || releaseManifest.schema !== "sura.public.release.v1") {
  throw new Error("release manifest does not match the reference version");
}
if (verificationManifest.version !== version || verificationManifest.schema !== "sura.public.verification.v1") {
  throw new Error("verification manifest does not match the reference version");
}
if (verificationManifest.engine_file !== path.basename(engine) ||
    verificationManifest.engine_bytes !== fs.statSync(engine).size ||
    verificationManifest.engine_sha256 !== engineSha256) {
  throw new Error("verification manifest engine identity does not match SuraLanguage.exe");
}
const performanceRecord = verificationManifest.performance_record;
if (!performanceRecord || performanceRecord.runs !== 30 || performanceRecord.target_ratio !== 10 ||
    typeof performanceRecord.vec2_sura_jit_ms !== "number" || typeof performanceRecord.vec2_cpp_o3_ms !== "number" ||
    typeof performanceRecord.vec3_sura_jit_ms !== "number" || typeof performanceRecord.vec3_cpp_o3_ms !== "number" ||
    !performanceRecord.host?.cpu || !performanceRecord.compiler_version || !/^[0-9a-f]{64}$/.test(performanceRecord.sha256) ||
    !/^[0-9a-f]{64}$/.test(performanceRecord.measured_engine_sha256) || !performanceRecord.measured_language_version) {
  throw new Error("verification manifest performance record is incomplete");
}
const performanceSha256 = performanceRecord.sha256;
const performanceReport = {
  generated_utc: performanceRecord.generated_utc,
  engine: {
    file: verificationManifest.engine_file,
    version_output: `Sura Language ${performanceRecord.measured_language_version}`,
    bytes: performanceRecord.measured_engine_bytes,
    sha256: performanceRecord.measured_engine_sha256,
  },
  host: performanceRecord.host,
  compiler_version: performanceRecord.compiler_version,
  compiler_flags: performanceRecord.compiler_flags,
  baselines: [
    {
      id: "vec2",
      sura_jit_ms: performanceRecord.vec2_sura_jit_ms,
      native_ms: performanceRecord.vec2_cpp_o3_ms,
      sura_native_ratio: performanceRecord.vec2_ratio,
    },
    {
      id: "vec3",
      sura_jit_ms: performanceRecord.vec3_sura_jit_ms,
      native_ms: performanceRecord.vec3_cpp_o3_ms,
      sura_native_ratio: performanceRecord.vec3_ratio,
    },
  ],
};
const packageManagerBytes = fs.readFileSync(packageManager);
const packageManagerSha256 = crypto.createHash("sha256").update(packageManagerBytes).digest("hex");
if (verificationManifest.package_helper?.file !== path.basename(packageManager) ||
    verificationManifest.package_helper?.bytes !== packageManagerBytes.length ||
    verificationManifest.package_helper?.sha256 !== packageManagerSha256) {
  throw new Error("verification manifest package helper identity does not match surapkg.exe");
}
for (const artifact of releaseManifest.artifacts || []) {
  const artifactPath = path.join(siteRoot, "public", "downloads", artifact.name);
  const bytes = fs.readFileSync(artifactPath);
  const sha256 = crypto.createHash("sha256").update(bytes).digest("hex");
  if (bytes.length !== artifact.bytes || sha256 !== artifact.sha256) {
    throw new Error("release artifact metadata mismatch: " + artifact.name);
  }
}
const modules = api.stdlibModules;
const apiSymbolCount = modules.reduce((sum, item) => sum + item.symbol_count, 0);
const stdlibSource = fs.readFileSync(path.join(root, "stdlib.hpp"), "utf8");
const registryStart = stdlibSource.indexOf("inline const std::unordered_map<std::string, BuiltinFn>& table()");
const registryEnd = stdlibSource.indexOf("\n    return T;", registryStart);
if (registryStart < 0 || registryEnd < 0) throw new Error("could not locate the global builtin registry");
const registryText = stdlibSource.slice(registryStart, registryEnd);
const globalBuiltinNames = Array.from(new Set(
  Array.from(registryText.matchAll(/\{"([^"]+)",\s*b_[A-Za-z0-9_]+\}/g), (match) => match[1])
)).sort();
const asyncApi = modules.find((module) => module.name === "async");
const asyncNames = new Set((asyncApi?.symbols || []).map((symbol) => symbol.name));
if (catalog.stdlib_count !== 39 || modules.length !== 34 ||
    !asyncApi || asyncApi.symbol_count !== 27 ||
    !asyncNames.has("sura") || !asyncNames.has("ready_all")) {
  throw new Error("stdlib inventory changed: entries=" + catalog.stdlib_count +
    ", modules=" + modules.length + ", signatures=" + apiSymbolCount +
    ", global_names=" + globalBuiltinNames.length +
    ", async_signatures=" + (asyncApi?.symbol_count ?? "missing"));
}
if (targetAudit.schema !== "sura.target.lowering_audit.v1" || targetAudit.ast_node_count !== 43 ||
    targetAudit.js.full !== 41 || targetAudit.wasm.full !== 27 || targetAudit.wasm.partial !== 14) {
  throw new Error("target lowering inventory changed; audit target status before publishing");
}
if (goalAudit.schema !== "sura.goal.audit.v1" || goalAudit.passed_count !== 126 ||
    goalAudit.required_count !== 127 || goalAudit.progress_percent !== 99.2) {
  throw new Error("goal audit inventory changed; audit progress before publishing");
}
if (nativePerformance.schema !== "sura.native.performance.v1" || nativePerformance.passed !== true ||
    nativePerformance.engine?.version_output !== `Sura Language ${version}` ||
    !Array.isArray(nativePerformance.baselines) || nativePerformance.baselines.length < 2) {
  throw new Error("native performance record does not match the current language version");
}
if (releaseEvidence.schema !== "sura.release.evidence_gate.v1" ||
    typeof releaseEvidence.passed !== "boolean" ||
    releaseEvidence.passed_count + releaseEvidence.failed_count !== releaseEvidence.required_count ||
    releaseEvidence.passed !== (releaseEvidence.failed_count === 0)) {
  throw new Error("release evidence gate counts or status are inconsistent");
}
const currentVec2Perf = nativePerformance.baselines.find((item) => item.id === "vec2");
const currentVec3Perf = nativePerformance.baselines.find((item) => item.id === "vec3");
if (!currentVec2Perf?.fair_scope_passed || !currentVec3Perf?.fair_scope_passed) {
  throw new Error("native performance fair-scope checks did not pass");
}

const moduleDescriptions = {
  array: "배열 생성·복사·정렬·검색·집계",
  async: "bounded worker pool 기반 command·HTTP·timer task와 structured scope",
  autograd: "typed Tensor, reverse-mode 자동미분, CPU/CUDA 연산, optimizer와 bounded CPU ONNX 실행",
  cli: "스크립트 인자와 명령행 문자열 파싱",
  crypto: "SHA-256, HMAC, 인코딩, URL·header·cookie 도우미",
  dataset: "uint32 text shard 생성과 seek 기반 batch reader",
  datetime: "현재 시각, 형식화, 파싱, 날짜 계산",
  db: "파일 기반 key/value와 record 작업",
  fs: "파일·디렉터리 읽기, 쓰기, 탐색, 이동",
  dict: "dictionary keys·values·merge·pick·path",
  ffi: "동적 C ABI library load와 symbol call",
  graphics3d: "4x4 matrix, cube mesh, transform, bounds, camera projection",
  http: "HTTP client, retry, local server, URL·form·header helpers",
  json: "JSON·JSONL·SSE·CSV·INI 변환, path와 schema 검사",
  llm: "model HTTP request 조립, schema, tool call, usage·cost 처리",
  log: "level, text/JSON event, file output",
  math: "수학 함수, clamp, min/max, 난수",
  nn: "CPU dense MLP 생성·학습·평가·JSON 저장",
  os: "환경 변수, argv, 경로, command 실행, sleep",
  path: "경로 결합·정규화·상대/절대 경로",
  media: "픽셀과 로컬 영상을 문자 프레임으로 변환",
  plugin: "Sura plugin ABI library load·call·lifecycle",
  python: "선택형 Python interpreter 탐색·eval·module call",
  rag: "검색 결과를 context·source·message 형태로 조립",
  random: "seed 기반 숫자·선택·shuffle·bytes·UUID",
  regex: "정규식 match·replace·capture·split",
  set: "배열 기반 합집합·교집합·차집합",
  stream: "lazy stream map·filter·window·batch·집계",
  string: "문자열 길이·분할·검색·치환·padding·chunk",
  tensor: "일반 배열 기반 tensor 연산과 통계",
  test: "assertion, check, summary, report",
  tokenizer: "UTF-8 raw-byte와 bounded byte-level BPE tokenizer, tokenizer 파일",
  tool: "도구 schema·policy 검사와 호출",
  vector: "2D/3D vector, cosine, normalize, transform, search",
};

const consoleMethods = [
  "log", "print", "write", "write_line", "writeln", "println", "line",
  "info", "debug", "warn", "warning", "error", "exception", "raw", "flush",
  "json", "inspect", "hrtime", "beep", "clear", "assert", "time", "time_end",
  "timeEnd", "time_log", "timeLog", "time_stamp", "timeStamp", "count",
  "count_reset", "countReset", "table", "dir", "dirxml", "trace", "group",
  "group_collapsed", "groupCollapsed", "group_end", "groupEnd", "profile",
  "profile_end", "profileEnd", "style", "color", "colour", "strip_ansi",
  "stripAnsi", "set_color", "setColor", "set_colour", "setColour",
  "reset_color", "resetColor", "reset_colour", "resetColour", "is_tty",
  "isTTY", "width", "height", "size", "status", "input", "read_line",
  "readline", "readLine", "prompt",
];
const vec2Perf = performanceReport.baselines.find((item) => item.id === "vec2");
const vec3Perf = performanceReport.baselines.find((item) => item.id === "vec3");
if (!vec2Perf || !vec3Perf) throw new Error("performance report is missing Vec2 or Vec3 baseline");

const machineFacts = {
  schema: "sura.public.reference.v1",
  language: "Sura",
  version,
  published_date: "2026-07-17",
  source_extension: ".sura",
  package_manifest: "sura.pkg.json",
  compatibility: compatibilityContract,
  project_governance: {
    scope_policy: "SCOPE.md",
    contribution_guide: "CONTRIBUTING.md",
    new_public_surface_default: "experimental",
    stable_promotion_requires: [
      "declared platforms and external dependencies",
      "tested success, failure, and fallback behavior",
      "current-syntax examples and public reference",
      "recorded source, bytecode, package-format, and ABI impact",
      "applicable Windows and cross-platform CI coverage",
    ],
  },
  execution: {
    pipeline: ["lexer", "parser/AST", "strict type checker", "register bytecode compiler", "register VM"],
    bytecode: { writes_version: 3, reads_versions: [2, 3], magic: "SURB" },
    optional_native_jit: {
      flag: "--jit",
      platform_abi: "Windows x64 / Win64, Linux x86-64 / System V, and little-endian Windows/Linux/macOS ARM64 / AAPCS64 baseline",
      emitter_backend: "x64-win64 partial compiler; x64-sysv-baseline and arm64-aapcs-baseline exception-free straight-line subsets",
      backends: [
        { os: "windows", architecture: "x86-64", abi: "win64", backend: "x64-win64", scope: "supported functions, methods, helpers, and guarded optimization paths" },
        { os: "linux", architecture: "x86-64", abi: "sysv-x86-64", backend: "x64-sysv-baseline", scope: "constant loads, moves, statically proven numeric add/subtract/multiply and unary negation, and returns; no helper calls" },
        { os: "windows/linux/macos", architecture: "arm64", abi: "aapcs64", backend: "arm64-aapcs-baseline", scope: "little-endian constant loads, moves, statically proven numeric add/subtract/multiply and unary negation, and returns; no helper calls" },
      ],
      mode: "lazy partial compilation inside the register VM",
      warmup_thresholds: { function_calls: 6, method_calls: 5 },
      fallback: "unsupported or failed native compilation continues in the register VM",
      strict_vector_loop_shortcut: {
        scope: "recognized top-level counted loops calling canonical 2D/3D vector updates; source class and function names are arbitrary",
        proof: "x/y(/z) layout, closure identity, field-copy constructor, exact add/scale/cross and update bytecode graphs, numeric inputs, and escape/non-alias guards",
        result_identity: "one or more iterations store a fresh result instance; zero iterations preserve the original instance",
        fallback: "any compile-time proof or runtime guard mismatch executes the original VM/native bytecode path",
      },
    },
    value_representation: "NaN-boxed dynamic Value",
    garbage_collector: "mark-sweep; process-global heap",
    diagnostics_default: "English",
    diagnostics_korean: "--lang ko or SURA_LANG=ko",
    strict_types_default: true,
    type_system: "strict-by-default gradual type checker",
    legacy_source_flag: "--legacy-types",
    legacy_disallowed_for: ["--compile", "--release"],
  },
  syntax: {
    lexical: {
      encoding: "UTF-8; optional UTF-8 BOM is accepted",
      line_comments: ["#", "//"],
      strings: "double quoted; supported escapes are \\n, \\r, \\t, \\\", and \\\\",
      interpolation: "{expression} inside a double-quoted string",
      statement_separator: "newline",
    },
    declaration: "name is value",
    typed_declaration: "name: type is value",
    function: "func name(params) do ... end",
    default_parameter: ["name is value", "name: type is value"],
    default_evaluation: {
      trigger: "only when the positional argument is omitted",
      explicit_nil: "passed unchanged",
      order: "left to right",
      visible_bindings: "earlier parameters; methods also expose self",
      supported_forms: ["named function", "anonymous function", "lambda", "method", "class field", "struct field"],
      class_fields: "evaluated for each new instance in parent-to-child declaration order",
      automatic_struct_fields: "evaluated for each omitted constructor argument; explicit arguments skip the field expression",
    },
    lambda: "|params| expression; || expression",
    blocks: ["if/elif/else/end", "while/do/end", "repeat/do/end", "for/do/end"],
    object_forms: ["class", "extends", "new", "self", "super", "struct", "enum"],
    errors: ["try", "catch", "finally do", "throw"],
    optional_access: "value?.field",
    null_coalescing: "left ?? right",
    ternary: "condition ? then_value : else_value",
    module_import: ["use module", "import \"path.sura\""],
    operator_precedence_low_to_high: [
      { operators: ["??"], associativity: "right" },
      { operators: ["? :"], associativity: "right" },
      { operators: ["or"], associativity: "left; short-circuit and returns the selected operand" },
      { operators: ["and"], associativity: "left; short-circuit and returns the selected operand" },
      { operators: ["not"], associativity: "prefix" },
      { operators: ["==", "!=", "<", "<=", ">", ">=", "in"], associativity: "one comparison per unparenthesized expression" },
      { operators: ["|"], associativity: "left" },
      { operators: ["^"], associativity: "left" },
      { operators: ["&"], associativity: "left" },
      { operators: ["<<", ">>"], associativity: "left" },
      { operators: ["+", "-"], associativity: "left" },
      { operators: ["*", "/", "%"], associativity: "left" },
      { operators: ["unary -", "~"], associativity: "prefix" },
      { operators: ["call", "index", ".", "?."], associativity: "postfix" },
    ],
    evaluation_order: {
      call_arguments: "left to right",
      array_elements: "left to right",
      dictionary_values: "source key order",
      null_coalescing: "right side runs only when the left side is nil",
      ternary: "only the selected branch runs",
    },
  },
  truthiness: {
    false_values: ["nil", "false", "number 0", "empty string", "empty array", "empty dict"],
    true_values: ["non-zero number", "non-empty string", "non-empty array", "non-empty dict", "functions", "instances", "tensors"],
  },
  bitwise_contract: {
    operand: "finite integral number in [-9007199254740991, 9007199254740991]",
    shift_count: "integer 0..63",
    left_shift_result: "safe integer range",
    right_shift: "arithmetic floor division by 2^count",
  },
  stdlib: {
    package_inventory_entry_count: catalog.stdlib_count,
    runtime_module_namespace_count: 35,
    catalogued_runtime_module_count: modules.length,
    api_signature_count: apiSymbolCount,
    global_builtin_name_and_alias_count: globalBuiltinNames.length,
    global_builtin_names_and_aliases: globalBuiltinNames,
    inventory_only_entries: {
      data: "alias of json",
      time: "alias of datetime",
      web: "alias of http",
      game: "package inventory source file; no VM use-module namespace",
      system: "package inventory source file; no VM use-module namespace",
    },
    runtime_module_aliases: {
      data: "json",
      time: "datetime",
      web: "http",
      logging: "log",
      filesystem: "fs",
      file: "fs",
      testing: "test",
      rng: "random",
      py: "python",
      g3d: "graphics3d",
      graphics: "graphics3d",
      ai: "nn",
    },
    console_runtime_module_catalogued_by_surapkg: false,
    console_methods: consoleMethods,
    catalog: catalog.entries,
    api_modules: modules.map((module) => ({
      ...module,
      description: moduleDescriptions[module.name] || "",
      documentation_scope: "signature and source location; return and error contracts are present only when encoded by the signature or prose sections",
    })),
  },
  async: {
    default_workers: "hardware concurrency clamped to 1..8; fallback 4",
    default_max_queue: 1024,
    configurable_worker_range: "1..256",
    configurable_queue_range: "1..1000000",
    task_states: ["queued", "running", "succeeded", "failed", "cancelled"],
    task_kinds: ["command", "HTTP request", "timer", "isolated Sura program"],
    closure_spawn: false,
    sura_program_task: {
      call: "async.sura({program, input, timeout_ms?}, optional_scope)",
      isolation: "child Sura process; no closure, instance, tensor, native resource, or Value pointer is shared",
      program_extensions: [".sura", ".sura.bc", ".bc", ".sura.srp", ".srp"],
      program_snapshot_limit_bytes: 67108864,
      input_format: "JSON-safe nil, bool, finite number, string, array, and dict",
      input_limit_bytes: 67108864,
      input_node_limit: 1000000,
      input_depth_limit: 128,
      rejected_input: ["closure", "instance", "tensor", "native resource", "cycle"],
      alias_identity_preserved: false,
      default_timeout_ms: 30000,
      timeout_range_ms: [1, 3600000],
      child_input: "argv()[0] contains the JSON input snapshot",
      result: "captured child stdout returned by await",
      nonzero_exit: "task fails",
      recursive_child_async_sura: false,
      cleanup: "temporary program/input files are removed after success, failure, timeout, cancellation, and runtime shutdown",
    },
    structured_scopes: true,
    wait_strategy: "completion epoch and condition variable; any/all_timeout do not poll",
    queued_cancellation: "removes the task from the FIFO queue in O(1)",
    result_limit_bytes: 67108864,
    retained_result_budget_bytes: 268435456,
    retained_error_limit_bytes: 65536,
    retained_budget_observability: ["async.limits().max_retained_bytes", "async.limits().retained_bytes"],
    output_limit_action: "command/curl process tree is terminated and the task fails",
    file_url_reads: "regular files only; 64 KiB chunks with cancellation checkpoints; 64 MiB result cap",
  },
  tokenizer: {
    byte: "lossless raw UTF-8 bytes with IDs 0..255",
    bpe: "deterministic byte-level BPE; pair-count ties use the smallest numeric pair key",
    bpe_training_corpus_limit_bytes: 1048576,
    bpe_vocabulary_range: [256, 4096],
    bpe_training_work_limit_token_visits: 67108864,
    bpe_stream_flush_bytes: 65536,
    direct_encode_decode_limit_bytes: 16777216,
    persistence: "byte tokenizer file v1 remains readable; BPE uses checksummed tokenizer file v2",
    lossless_utf8: true,
    normalization: false,
    external_tokenizer_format_compatibility: false,
    dataset_pack_text: "byte and BPE tokenizers; BPE retains a raw suffix as long as the largest learned piece so merges remain exact across text/file chunks",
    verification: "tests/23_tokenizer_dataset.sura and tests/70_bpe_tokenizer.sura",
  },
  onnx_execution: {
    api: "autograd.run_onnx(path, inputs, [options])",
    status: "experimental bounded CPU subset; not a general ONNX runtime",
    device: "cpu",
    ir_version_range: [3, 10],
    default_opset_range: [7, 18],
    max_nodes: 4096,
    max_node_inputs: 16,
    max_node_outputs: 16,
    max_node_attributes: 64,
    supported_operators: ["Identity", "Add", "Sub", "Mul", "Div", "Neg", "MatMul", "Relu", "Tanh", "Sigmoid", "Gemm", "Transpose", "Flatten", "Reshape", "Softmax on the last axis"],
    shape_operator_contract: "Transpose validates a complete perm or reverses axes by default; Flatten validates axis in [-rank, rank]; Reshape accepts only a raw-data rank-1 INT64 initializer of at most 8 values, copies axes for 0 with allowzero=0, and permits one inferred -1; all preserve the Sura autograd graph",
    initializer_storage: "raw_data only",
    initializer_dtypes: ["FLOAT", "FLOAT16", "DOUBLE", "BFLOAT16"],
    reshape_shape_initializer: "raw-data INT64, rank 1, 0..8 values, usable only as Reshape input 1; not exposed by load_onnx_weights",
    topological_order_required: true,
    declared_value_metadata: "when ValueInfo contains Tensor dtype and static shape, inputs, initializers, intermediate values, and outputs must match; symbolic dimensions are rejected",
    gradient: "Sura autograd graph is preserved from outputs to supplied inputs",
    rejected_or_unsupported: ["custom operator domains", "GPU inputs", "external data", "integer Tensor initializers outside bounded Reshape shapes", "zero-sized floating Tensor outputs", "Conv", "dynamic shapes", "control flow", "unknown operators and attributes"],
    verification: "tests/71_onnx_execution.sura",
  },
  targets: {
    native_vm_jit: "primary",
    javascript: {
      ast_node_count: targetAudit.ast_node_count,
      full: targetAudit.js.full,
      ignored: targetAudit.js.ignored,
      partial: targetAudit.js.partial,
      classification_complete: targetAudit.js.complete,
      runtime_parity_with_native: false,
      native_only_or_missing_areas: ["Python bridge", "FFI", "plugin", "async.cmd", "autograd", "media", "nn", "dataset", "tokenizer"],
    },
    wasm: {
      ast_node_count: targetAudit.ast_node_count,
      full: targetAudit.wasm.full,
      ignored: targetAudit.wasm.ignored,
      partial: targetAudit.wasm.partial,
      classification_complete: targetAudit.wasm.complete,
      runtime_parity_with_native: false,
    },
  },
  freestanding: {
    status: "experimental compiler target with an executed QEMU graphical desktop milestone; not a complete operating system",
    target: "uefi-x86_64",
    output: "position-independent PE32+ EFI application",
    hosted_runtime_dependencies: [],
    entry_order: ["efi_main", "kernel_main", "main", "synthetic top-level entry"],
    modules: {
      syntax: "import \"relative/path.sura\"",
      resolution: "relative to the importing file, recursively",
      duplicate_policy: "one inclusion per normalized path",
      errors: ["missing file", "circular import", "import parse failure"],
      namespace: "one shared freestanding global namespace",
    },
    scalar_types: ["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "isize", "usize", "ptr", "ptr[StructName]"],
    control_flow: ["signedness-aware integer comparison, division, remainder, and right shift", "if/while/repeat", "break/continue", "and/or short-circuit with selected-operand result"],
    integer_semantics: {
      signed: ["signed comparisons", "signed division and remainder", "arithmetic right shift"],
      unsigned: ["unsigned comparisons", "unsigned division and remainder", "logical right shift"],
      executed_verification: ["examples/os/integer_semantics_qemu_gate.sura", "tools/sura_integer_semantics_qemu_gate.ps1", "0xffffffffffffffff and high-bit boundary cases", "negative i64 boundary cases", "SURA_INTEGER_SEMANTICS_OK"],
    },
    tls13_crypto: {
      libraries: ["stdlib/freestanding/sha256.sura", "stdlib/freestanding/sha384.sura", "stdlib/freestanding/hkdf_sha256.sura", "stdlib/freestanding/aes128_gcm.sura", "stdlib/freestanding/x25519.sura", "stdlib/freestanding/tls13_record.sura", "stdlib/freestanding/tls13_handshake.sura", "stdlib/freestanding/tls13_hello.sura", "stdlib/freestanding/tls13_messages.sura", "stdlib/freestanding/tls13_verify.sura", "stdlib/freestanding/entropy_x86.sura", "stdlib/freestanding/rsa_public.sura", "stdlib/freestanding/rsa_sha256.sura", "stdlib/freestanding/rsa_sha384.sura", "stdlib/freestanding/der.sura", "stdlib/freestanding/x509.sura", "stdlib/freestanding/x509_chain.sura"],
      algorithms: ["streaming SHA-256", "streaming SHA-384", "HMAC-SHA256", "HKDF-Extract and HKDF-Expand", "TLS 1.3 HKDF-Expand-Label", "AES-128 block encryption", "AES-128-GCM with 96-bit nonce and 128-bit tag", "X25519 with all-zero shared-secret rejection", "TLS 1.3 record nonce, encryption, authenticated decryption, content-type and zero-padding recovery", "RFC 8448 ClientHello plus ServerHello handshake key schedule", "TLS 1.3 application traffic secrets and Finished calculation", "SNI/TLS 1.3/X25519/signature-scheme/AES-128-GCM/HTTP-1.1-ALPN ClientHello builder", "strict ServerHello and encrypted handshake-message parsers", "RDRAND-backed hardware entropy with fail-closed retry", "32-bit-limb Montgomery RSA public exponent 65537 operation", "RSA-PSS-SHA256 plus PKCS#1 v1.5 SHA-256 and SHA-384 verification", "bounded DER and X.509 parsing", "certificate chain, hostname, validity-time, key-usage, basic-constraints, and explicit trust-anchor validation"],
      storage: "caller-owned fixed buffers; no allocator, hosted runtime, OS API, or firmware service after initialization",
      executed_gate: "tools/sura_tls_crypto_qemu_gate.ps1",
      executed_environment: "QEMU/OVMF after ExitBootServices",
      known_answers: ["RFC 6234 SHA-256 and HMAC-SHA256", "SHA-384 empty/abc and 111/112-byte padding-boundary vectors", "RFC 5869 HKDF test case 1", "RFC 8448 HKDF-Expand-Label finished key", "AES-128 known answer", "NIST SP 800-38D AES-128-GCM known answer", "RFC 7748 direct X25519 and Alice/Bob shared secret", "RFC 8448 encrypted TLS 1.3 application-data record", "RFC 8448 complete initial handshake secrets, traffic keys, IVs, and Finished keys", "independently generated application traffic secret, key, IV, and Finished values", "independent BigInteger RSA exponent-65537 result", "OpenSSL-generated RSA-PSS and PKCS#1 SHA-256 signatures", "official pinned SHA-384/RSA root self-signatures", "OpenSSL-generated three-certificate suralang.site test chain"],
      executed_markers: ["SURA_SHA256_OK", "SURA_SHA384_OK", "SURA_HMAC_SHA256_OK", "SURA_HKDF_SHA256_OK", "SURA_TLS13_LABEL_OK", "SURA_AES128_OK", "SURA_AES128_GCM_OK", "SURA_X25519_OK", "SURA_TLS13_RECORD_OK", "SURA_TLS13_KEY_SCHEDULE_OK", "SURA_TLS13_HELLO_OK", "SURA_ENTROPY_X86_OK", "SURA_RSA_PUBLIC_OK", "SURA_RSA_SHA256_VERIFY_OK", "SURA_DER_READER_OK", "SURA_X509_CHAIN_OK", "SURA_TLS13_APPLICATION_OK", "SURA_TLS13_MESSAGES_OK", "SURA_TLS_CRYPTO_OK"],
      negative_checks: ["modified GCM tag rejected", "unauthenticated plaintext output remains unchanged", "malformed ServerHello length rejected", "HelloRetryRequest rejected", "all-zero server X25519 share rejected", "wrong hostname rejected", "expired certificate rejected", "untrusted root rejected", "invalid calendar date rejected", "unknown critical X.509 extension rejected"],
      trust_anchors: ["ISRG Root X1", "DigiCert Global Root G2", "GlobalSign Root CA - R3", "Amazon Root CA 1", "USERTrust RSA Certification Authority", "Microsoft RSA Root Certificate Authority 2017"],
      trust_store: ["tools/sura_trust_store_generate.ps1 pins DER length and SHA-256 before source replacement", "indexed DER, byte-count, and fingerprint accessors", "HTTPS initialization loop accepts 1 through 16 generated roots", "tools/sura_trust_store_qemu_gate.ps1", "post-ExitBootServices digest, X.509 parse, CA/key-usage/time/RSA validation, SHA-256/SHA-384 self-signature verification, and one-byte signature-mutation rejection for every generated root", "SURA_TRUST_STORE_OK"],
      limitations: ["the isolated gate is not an Internet-connection proof", "implemented HTTPS profile is TLS_AES_128_GCM_SHA256 with X25519, RSA/SHA-256 or RSA/SHA-384 chains, and RSA-PSS-RSAE-SHA256 CertificateVerify", "six pinned built-in trust anchors rather than a broad operating-system root store", "no revocation checking or session resumption", "table-based AES implementation is not claimed to be cache-side-channel hardened"],
    },
    static_data: ["static.zero", "static.bytes", "static.u8", "static.u16", "static.u32", "static.u64", "static.utf8", "static.utf16", "static.struct"],
    layout: ["typed struct fields", "natural alignment", "struct Name packed", "sizeof", "alignof", "offset_of", "typed pointer field load/store"],
    low_level: ["raw memory 8/16/32/64", "port I/O 8/16/32", "control registers", "MSR", "GDT", "IDT", "INVLPG", "CPUID", "RDTSC/RDTSCP", "RDRAND", "XGETBV/XSETBV"],
    cpu_state: {
      tss: ["RSP0..2", "IST1..7", "I/O-map offset", "checked 16-byte GDT TSS descriptor", "LTR/STR"],
      descriptors: ["direct table+size LGDT/LIDT", "CS/DS/ES/SS reload"],
      extended_state: ["FNINIT", "CLTS", "FXSAVE64/FXRSTOR64", "XSAVE64/XRSTOR64", "SWAPGS", "STAC/CLAC", "WBINVD"],
    },
    smp_primitives: {
      percpu: ["IA32_GS_BASE", "IA32_KERNEL_GS_BASE", "GS-relative 8/16/32/64-bit read/write"],
      apic: ["runtime xAPIC/x2APIC selection", "APIC ID", "register read/write", "EOI", "ICR busy", "IPI", "INIT", "SIPI"],
      compile_time_checks: ["APIC register offset alignment/range", "constant SIPI address alignment/range", "constant destination width"],
    },
    ap_startup: {
      library: "stdlib/freestanding/ap_startup.sura",
      trampoline: ["230-byte template", "16-bit real mode", "PAE and EFER.LME", "caller PML4", "small GDT", "64-bit Sura entry", "atomic ready flag"],
      sequence: ["bounded ICR waits", "INIT", "10 ms TSC delay", "SIPI", "200 us TSC delay", "optional second SIPI", "bounded ready wait"],
      example: "examples/os/ap_startup_features.sura",
      verification: "tools/sura_ap_startup_smoke.ps1 checks the exact assembled 230-byte template in the generated EFI image",
      limitations: ["caller discovers APs and calibrates TSC", "caller supplies a writable virtual alias for the low page", "caller identity-maps the trampoline and maps all entry parameters", "PML4 physical address must fit in 32 bits", "no per-AP descriptor/extended-state/scheduler initialization", "no 82489DX INIT-deassert sequence", "compile-only verification; no executed AP-start proof"],
    },
    paging_primitives: {
      address: ["PML4/PDPT/PD/PT index", "page offset", "48-bit canonical-address test"],
      entries: ["construct", "address", "flags", "present", "large", "read", "write", "map", "clear"],
      tlb: ["CR3 root", "activate root", "INVLPG", "local CR3 flush"],
      compile_time_checks: ["constant page-table index range", "constant physical/root 4-KiB alignment"],
    },
    memory_libraries: {
      physical: ["UEFI conventional-memory import", "bitmap reserve/release", "single-page allocation", "aligned contiguous allocation", "free-page accounting"],
      virtual: ["conflict-checked table linking", "4-KiB map/unmap/protect", "4-KiB/2-MiB/1-GiB translation", "local invalidation"],
      lifecycle: ["memory-map retry", "ExitBootServices before direct allocation", "no firmware calls or return after successful exit"],
      example: "examples/os/memory_kernel.sura",
      limitations: ["no internal SMP lock", "no NUMA/DMA zones", "generic virtual helper requires caller-provided intermediate tables", "no remote TLB shootdown"],
    },
    framebuffer_graphics: {
      libraries: ["stdlib/freestanding/framebuffer.sura", "stdlib/freestanding/font5x7.sura", "stdlib/freestanding/font_hangul.sura", "stdlib/freestanding/font_ui.sura", "stdlib/freestanding/font_ui_atlas.sura"],
      example: "examples/os/framebuffer_features.sura",
      surface_contract: "caller-owned 32-bit GOP RGB/BGR surface, bounded to 64 MiB",
      drawing: ["checked pixel access", "clipped fill and outline rectangles", "horizontal and vertical lines", "integer Bresenham lines", "basic application icon", "5x7 fixed-width bitmap text", "proportional 16-pixel ASCII and modern-Hangul text with 2-bit antialias blending"],
      presentation: ["caller-owned backbuffer", "64-bit full-frame copy presentation", "clipped rectangle copy for software-cursor damage repair", "sampled framebuffer hash"],
      executed_resolution: "1280x800 in the current QEMU/EDK2 gate",
      screenshot_gate: "tools/sura_os_screenshot.ps1 captures the actual QEMU framebuffer through loopback-only QMP after SURA_OS_DESKTOP_OK",
      limitations: ["no hardware acceleration, GPU driver, or accelerated compositing", "polling xHCI USB boot keyboard/mouse with PS/2 fallback rather than interrupt-driven physical-hardware input proof", "the UI atlas covers printable ASCII and modern precomposed Hangul rather than all Unicode or complex shaping"],
    },
    graphical_text_terminal: {
      library: "stdlib/freestanding/text_terminal.sura",
      model: "caller-owned fixed-capacity Unicode-code-point cell buffer",
      limits: ["1..256 columns", "1..128 rows"],
      operations: ["Unicode scalar", "checked UTF-8 output", "code-point Backspace", "newline", "wrap", "upward scroll", "clear", "bounded C-string output", "unsigned decimal output", "16-pixel UI-font framebuffer draw"],
      os_instance: "46x14 command history rendered with the proportional ASCII/modern-Hangul UI font and shared by COM1 and decoded xHCI-or-PS/2 keyboard input",
      commands: ["help", "status", "mem", "about", "clear", "shutdown", "reboot"],
      example: "examples/os/text_terminal_features.sura",
      executed_verification: "QMP keyboard input fills and scrolls the 14-row OS terminal, clears it, enters dkssudgktpdy as 안녕하세요 through the common two-set input path, and leaves a visible status result; serial markers prove scroll, clear, and exact Korean UTF-8 input",
      limitations: ["built-in command names are ASCII", "the font atlas covers printable ASCII and modern precomposed Hangul rather than all Unicode", "no ANSI escape parser", "no selection, clipboard, alternate buffer, or scrollback beyond visible rows"],
    },
    window_manager_foundation: {
      library: "stdlib/freestanding/window_manager.sura",
      model: "caller-owned fixed-capacity window records with no dynamic allocation",
      limits: ["1..64 registered windows", "windows must fit inside the desktop area above the taskbar"],
      operations: ["normal, minimized, maximized, and fullscreen state", "restore geometry", "visible and active state", "z-order", "focus", "top-window hit testing", "title-bar drag", "bottom-right resize", "minimize, maximize, restore, fullscreen, close, and show", "desktop-bound clamping"],
      example: "examples/os/window_manager_features.sura",
      os_integration: "Six application windows use the manager for focus, z-order, title-bar drag, interactive resize, minimize/taskbar restore, maximize/restore, fullscreen/restore, close/reopen, bounds, Start/taskbar activation, active-window keyboard routing, and FAT32-backed desktop-state restore",
      verification: "compile and EFI image verification plus QEMU focus, drag, resize, minimize/taskbar restore, maximize/restore, fullscreen/restore, close, Start activation, serial-marker, and screenshot verification",
      limitations: ["the manager stores geometry and state but does not own application rendering", "no animation, snap layout, drag-and-drop, or real multi-output presentation"],
    },
    window_server_and_common_ui: {
      window_server_library: "stdlib/freestanding/window_server.sura",
      common_ui_library: "stdlib/freestanding/ui.sura",
      ring3_worker_source: "os/user_window_server.sura",
      model: ["caller-owned application surfaces", "separate compositor output", "opaque z-order composition", "bounded damage rectangles", "failed-owner surface removal", "clipboard", "light and dark themes", "DPI scaling", "two-monitor virtual-desktop records"],
      common_controls: ["button", "text input", "list", "menu", "checkbox", "focus and hit testing", "activation", "accessible role and name metadata"],
      shared_buffer_path: "the kernel maps the real GOP backbuffer pages into the dedicated Window Server ProcessAddressSpace through process_space_map_shared_page; the executed boot reads one pixel, writes a test value, verifies it, and restores the original value",
      example: "examples/os/window_server_features.sura",
      gate: "tools/sura_window_server_qemu_gate.ps1",
      executed_markers: ["SURA_WINDOW_SERVER_OK", "SURA_OS_WINDOW_SERVER_RING3_READY", "SURA_OS_WINDOW_SERVER_CR3_OK", "SURA_OS_WINDOW_SERVER_SHARED_BUFFER_OK", "SURA_OS_WINDOW_SERVER_RING3_OK"],
      limitations: ["the persistent Ring-3 worker executes a bounded 16x16 two-surface composition job, but the six built-in application content renderers still draw in ring 0", "full-size per-application Ring-3 surfaces have not been connected to the compositor", "no window animations, drag-and-drop service, real multi-output scanout, or complete accessibility service"],
    },
    desktop_shell: {
      library: "stdlib/freestanding/desktop_shell.sura",
      model: "fixed-layout caller-rendered Start menu, taskbar, and desktop-icon hit testing",
      actions: ["Start toggle", "Terminal/System/Files/Editor/Calculator menu", "shutdown", "Terminal/System/Files/Editor/Calculator taskbar", "About/Files/Editor/Calculator icon"],
      state: ["screen and taskbar dimensions", "Start open", "last action"],
      example: "examples/os/desktop_shell_features.sura",
      os_integration: "Start and taskbar activation, close/reopen routing, mounted Files window, and desktop shutdown dispatch",
      limitations: ["fixed coordinates and launcher entries", "caller performs rendering and window actions", "no settings store, notifications, or process launcher"],
    },
    desktop_applications: {
      library: "stdlib/freestanding/desktop_apps.sura",
      ring3_worker_source: "os/user_worker.sura",
      ring3_system_info_source: "os/user_system_info.sura",
      ring3_terminal_source: "os/user_terminal.sura",
      ring3_file_explorer_source: "os/user_file_explorer.sura",
      ring3_editor_source: "os/user_text_editor.sura",
      ring3_calculator_source: "os/user_calculator.sura",
      ring3_window_server_source: "os/user_window_server.sura",
      ring3_browser_source: "os/user_browser.sura",
      applications: {
        system_information: ["kernel-gathered bounded framebuffer, free-page, storage, and network snapshot", "fixed CPL-3 snapshot validation and derived free-memory MiB/pixel count worker with a bounded mailbox", "ring-0 activation and rendering after return"],
        file_explorer: ["bounded entry count and selection", "fixed CPL-3 selection worker with a bounded mailbox", "executed OS traverses the SuraFS UTF-8 /문서 tree through VFS, opens /문서/메모.txt, creates files and folders, renames UTF-8 names, transfers entries to /휴지통, permanently deletes only from the recycle bin, and performs Ctrl+C/Ctrl+X/Ctrl+V file and bounded complete-directory-tree copy/move after returning to ring 0"],
        text_editor: ["4096-byte caller-owned buffer", "UTF-8 byte-boundary cursor and selection anchor", "printable and packed UTF-8 insertion at the cursor", "selected-range replacement", "LF-normalized Enter", "UTF-8 code-point Backspace and Delete", "dirty state", "8288-byte three-page CPL-3 editing mailbox with separate document and insertion payload", "selection highlighting", "automatic UTF-8 wrapping and cursor-following viewport", "Ctrl+A/C/X/V selection clipboard", "Shift+Left/Right and Shift+Home/End selection extension", "Ctrl+F next-match selection", "Ctrl+H bounded replacement through the CPL-3 editor worker", "lower-case .sura path association with bounded lexical colors for keywords, quoted strings, # line comments, and numbers", "non-destructive 62-byte /문서/main.sura starter creation", "executed OS atomically replaces and autosaves the selected SuraFS file", "Ctrl+S forced save", "Ctrl+Shift+S UTF-8 Save As without silent overwrite"],
        calculator: ["unsigned integer input", "+", "-", "*", "/", "=", "C", "Backspace", "full expression and result display", "clickable 4x4 keypad plus keyboard input", "overflow, underflow, and division-by-zero error state", "fixed CPL-3 state worker with a bounded mailbox"],
      },
      example: "examples/os/desktop_apps_features.sura",
      os_integration: "registered windows 2..6, desktop/Start/taskbar launch and reopen, and active-app input routing; System Information, Terminal, File Explorer, Text Editor, Calculator, Window Server, and Browser request validator are seven persistent UserProcessScheduler processes with separate CR3 roots, event queues, and kernel stacks. The Window Server has a mapped real backbuffer and executes a bounded compositor job. The Browser worker validates a private copied URL snapshot and returns scheme/host/path plus network/storage/device capabilities before a request, while network, TLS, DOM, layout, and rendering remain in ring 0",
      executed_verification: "QEMU initializes the five-process graphical scheduler, validates a System Information snapshot through its persistent CPL-3 path before ring-0 rendering, edits and recognizes Terminal commands through the Terminal CPL-3 path before ring-0 rendering and privileged execution, mounts SuraFS through VFS, selects /문서 and opens /문서/메모.txt through the File Explorer CPL-3 path before ring-0 traversal, creates and renames ASCII and Korean entries, transfers a folder to /휴지통, copies an exact 43-byte UTF-8 file, moves it into movebox, recursively duplicates that complete folder tree, appends ASCII, LF, and Korean UTF-8 through the Text Editor CPL-3 mailbox, completes Ctrl+F and bounded Ctrl+H replacement, selects and copies the complete document, cuts it through CPL-3 range deletion, pastes it through the bounded CPL-3 payload, saves an exact /문서/copy.sura duplicate through Save As, activates bounded .sura syntax coloring, flushes through AHCI after returning to ring 0, restores the exact payload and full mutation tree on a preserved second boot, evaluates keyboard 50 - 31 = 19 and graphical-keypad 7 + 5 = 12 through the Calculator CPL-3 path, verifies five distinct worker CR3 roots and stable saved frames/kernel stacks after repeated GUI requests, deliberately faults the blocked Calculator through faultapp, requires SURA_OS_USER_PROCESS_FAULT and SURA_OS_USER_PROCESS_ISOLATED, reaps and recreates it with a new process ID and CR3, requires SURA_OS_USER_PROCESS_RESTARTED, executes a real Calculator event and requires SURA_OS_USER_PROCESS_RESTART_EVENT_OK, then requires the persistent Terminal status command to print kernel: ready; it next sends hangapp, queues a System Information request, waits for SURA_OS_USER_PROCESS_HANG_STARTED, injects mouse movement during the non-yielding Calculator loop, requires SURA_OS_USER_PROCESS_WATCHDOG, SURA_OS_USER_PROCESS_HANG_INPUT_OK, and SURA_OS_USER_PROCESSES_CONCURRENT_OK, verifies that System Information completed under its own CR3 through SURA_OS_USER_PROCESS_BACKGROUND_OK, isolates and reconstructs Calculator again, executes a real event, requires SURA_OS_USER_PROCESS_HANG_RECOVERED, and finally requires another persistent Terminal status response before shutdown",
      limitations: ["built-in application content rendering remains kernel-owned; the Ring-3 Window Server currently proves bounded composition and real-backbuffer access rather than presenting full-size app-owned surfaces", "the scheduler accepts events for multiple workers and executes a real System Information job while Calculator is non-yielding, but ordinary UI handlers still wait for their own result; a user-facing background-job API is not implemented", "automatic recovery reconstructs the failed worker from its built-in app image; it does not restore an arbitrary instruction-level process checkpoint", "hardware snapshot gathering, terminal rendering and privileged commands, VFS/SuraFS traversal, mutation, and Text Editor persistence remain ring-0 operations after their workers return", "File Explorer enumerates at most the 32-node formatted capacity in a six-row scrolling viewport and has no search, sorting, properties dialog, progress UI, or interactive overwrite-conflict dialog; paste resolves conflicts with bounded Copy suffixes", "Text Editor has keyboard selection, clipboard editing, bounded find/replace, and lexical .sura colors but no mouse selection, vertical line-aware cursor movement, multi-document editing, parser diagnostics, completion, or semantic highlighting", "Calculator is unsigned integer only"],
    },
    cmos_rtc: {
      library: "stdlib/freestanding/rtc.sura",
      operations: ["bounded update-window wait", "two matching samples", "BCD conversion", "12-hour to 24-hour conversion", "hour/minute/second validation"],
      example: "examples/os/rtc_features.sura",
      os_integration: "HH:MM taskbar clock refreshed on sampled second changes with SURA_OS_RTC_OK after a valid QEMU RTC read",
      limitations: ["PC CMOS ports 0x70/0x71 only", "time only; no date, timezone, monotonic clock, or alarm"],
    },
    ps2_desktop_input: {
      library: "stdlib/freestanding/ps2.sura",
      controller: ["bounded i8042 waits", "translated Set-1 keyboard enable", "standard auxiliary mouse enable", "polling data-source dispatch"],
      keyboard: ["physical Set-1 position", "press/release/repeat", "left/right Shift, Ctrl, and Alt", "Caps Lock", "Korean Hangul/Hanja make keys", "printable Unicode scalar", "Enter", "Backspace", "extended-key tracking"],
      desktop_queue: ["64-entry kernel-owned IpcQueue", "KeyEvent encode/decode round trip", "press and release delivery", "active-window routing after dequeue", "SURA_OS_INPUT_EVENT_OK"],
      mouse: ["three-byte relative packets", "signed movement", "screen bounds", "three button transitions"],
      example: "examples/os/ps2_features.sura",
      executed_verification: "tools/sura_os_screenshot.ps1 exercises Shift, Backspace, status, Korean mode and exact dkssudgktpdy to 안녕하세요 input, pointer movement, focus, title-bar drag, close, taskbar reopen, and Start-menu activation through QEMU QMP",
      limitations: ["kernel polling queue rather than IRQ1/IRQ12 device delivery", "no scheduled-process input delivery", "only English-US and Korean two-set layouts", "the PS/2 fallback negotiates IntelliMouse wheel packets but has no five-button support", "the desktop prefers xHCI when both USB devices enumerate"],
    },
    unicode_text_input: {
      libraries: ["stdlib/freestanding/key_event.sura", "stdlib/freestanding/text_input.sura", "stdlib/freestanding/font_hangul.sura", "stdlib/freestanding/font_ui.sura", "stdlib/freestanding/font_ui_atlas.sura"],
      model: "device-independent physical KeyEvent and Unicode scalar composition with caller-owned editor storage",
      key_event: ["physical key", "raw scan code", "Unicode scalar", "press/release/repeat", "Shift/Ctrl/Alt/Caps/Hangul modifiers", "monotonic sequence"],
      hangul: ["English/Hangul mode", "modern two-set Korean mapping", "initial/medial/final composition", "compound vowels and finals", "final split before following vowel", "composition-aware Backspace"],
      output: ["up to two committed Unicode scalars per key", "current composition preview", "checked UTF-8 encoding for one through four bytes"],
      desktop_integration: ["Text Editor and Terminal CPL-3 packed UTF-8 mailboxes", "UTF-8 code-point Backspace", "distinct lowercase ASCII", "proportional 16-pixel 2-bit-antialiased ASCII and modern-Hangul framebuffer atlas", "live composition preview", "global Right Alt and dedicated Hangul-key mode switching", "clickable System Information keyboard setting", "SETTINGS.CFG mode persistence", "Text Browser UTF-8 address-bar and page-body paths", "HTTP uppercase percent encoding"],
      example: "examples/os/text_input_features.sura",
      executed_gate: "tools/sura_text_input_qemu_gate.ps1 with SURA_KEY_EVENT_OK, SURA_TEXT_INPUT_ANNYEONGHASEYO_OK, and SURA_TEXT_INPUT_OK",
      executed_scope: ["Set-1 press/release/repeat", "Shift/Ctrl/Alt/Caps", "Caps-independent Korean physical mapping", "exact dkssudgktpdy to 안녕하세요 physical-key composition", "compound vowel", "final deletion", "exact UTF-8 bytes"],
      desktop_gate: "tools/sura_os_screenshot.ps1 with SURA_OS_INPUT_EVENT_OK, SURA_OS_INPUT_LAYOUT_OK, SURA_OS_KOREAN_INPUT_OK, SURA_OS_TERMINAL_KOREAN_INPUT_OK, SURA_OS_BROWSER_KOREAN_INPUT_OK, SURA_OS_BROWSER_LINK_OK, SURA_OS_BROWSER_WHEEL_OK, SURA_OS_BROWSER_SCROLL_OK, SURA_OS_KEY_REPEAT_OK, SURA_OS_BROWSER_BACKSPACE_REPEAT_OK, SURA_OS_BROWSER_NAV_ASYNC_BEGIN, SURA_OS_BROWSER_NAV_ASYNC_INPUT_OK, SURA_OS_BROWSER_NAV_ASYNC_DONE, SURA_OS_BROWSER_NAV_TLS_BEGIN, SURA_OS_BROWSER_NAV_TLS_DONE, SURA_OS_BROWSER_NAV_FETCH_CANCEL_REQUESTED, SURA_OS_BROWSER_NAV_FETCH_CANCELLED_OK, build/os/SuraOS-korean-input.ppm, build/os/SuraOS-browser.ppm, build/os/SuraOS-browser-scrolled.ppm, and build/os/SuraOS-browser-korean.ppm; the gate starts a real DNS query for a reserved invalid TLD, processes F6 while it is pending, cancels the stale URL snapshot, and then completes the separate explicit-http example.org live non-default-host response check; it also advances two live suralang.site TLS handshakes incrementally, cancels one later response fetch with F6, retries it, and then executes certificate validation, encrypted response, external CSS, DOM layout, anchor hit-testing, wheel scrolling, held Page Down, and held UTF-8 Backspace; tools/sura_input_layout_qemu_gate.ps1 preserves the data disk and requires SURA_OS_INPUT_LAYOUT_RESTORED on a second boot",
      limitations: ["modern Korean composition and precomposed-Hangul atlas only", "DNS host names need ASCII because IDNA is absent", "Terminal commands remain ASCII", "no Hanja conversion, candidate UI, full Unicode font, complex shaping, or normalization"],
    },
    context_primitives: {
      frame_bytes: 72,
      saved_registers: ["r15", "r14", "r13", "r12", "rsi", "rdi", "rbp", "rbx", "resume RIP"],
      operations: ["initialize aligned task frame", "save current RSP", "resume next RSP", "entry(argument)", "exit_handler(result)"],
      limitations: ["no RFLAGS", "no FPU/SIMD", "no CR3", "no FS/GS base", "integer cooperative context only"],
    },
    scheduler_library: {
      model: "single-CPU cooperative round-robin",
      operations: ["init", "create", "yield", "tick", "sleep", "block", "wake", "join", "reap"],
      states: ["unused", "runnable", "finished", "sleeping", "blocked"],
      example: "examples/os/scheduler_features.sura",
      limitations: ["no interrupt-frame preemption", "no SMP run-queue synchronization", "no FPU/SIMD context save", "caller supplies task storage and stacks"],
    },
    preemptive_scheduler_and_timer: {
      model: "single-CPU ring-0 round-robin from checked interrupt frames",
      frame: ["152-byte integer/error/RIP/CS/RFLAGS frame", "checked canonical addresses", "ring-0 CS", "CLI before IRETQ resume"],
      scheduler: ["create", "timer selection", "vector-129 voluntary yield", "sleep", "block", "wake", "join", "exit", "reap"],
      timer: ["local-APIC periodic/one-shot count", "bounded PIT channel-2 calibration", "TSC-deadline mode"],
      libraries: ["stdlib/freestanding/preempt.sura", "stdlib/freestanding/timer.sura"],
      example: "examples/os/preemptive_timer_features.sura",
      limitations: ["compile and machine-code verification only", "ring-0 tasks only", "no FPU/SIMD, CR3, FS/GS, debug, or process state", "no priorities", "no SMP locking or load balancing", "caller installs IDT and programs per-CPU timer"],
    },
    software_interrupt_syscalls: {
      invoke: "syscall.invoke(vector, number, argument...)",
      argument_registers: ["RAX number", "RDI", "RSI", "RDX", "R10", "R8"],
      dispatcher: "stdlib/freestanding/syscall.sura",
      example: "examples/os/syscall_features.sura",
      limitations: ["dispatcher does not automatically validate user pointers", "caller selects the current process and applies checked copy helpers", "caller owns CPL/DPL, permissions, locking, and fault policy"],
    },
    fast_syscalls_and_user_mode: {
      invoke: "syscall.fast(number, argument...)",
      configure: "syscall.fast_configure(dispatch, bad_return, kernel_cs, user_cs, flags_mask, kernel_rsp_offset, user_rsp_offset)",
      entry: "user.enter(entry, stack_pointer, argument, code_selector, stack_selector)",
      saved_frame: ["user.frame_size()", "user.frame_init(kernel_stack_top, entry, user_rsp, argument, code_selector, stack_selector)", "user.frame_valid(frame)", "user.resume(frame)"],
      address_check: "user.is_address(address)",
      argument_registers: ["RAX number", "RDI", "RSI", "RDX", "R10", "R8"],
      entry_contract: ["EFER.SCE and STAR/LSTAR/FMASK configuration", "conditional saved-CS SWAPGS and LFENCE in compiler interrupt wrappers", "GS-relative user-RSP save and kernel-RSP load", "15-register integer fast frame", "168-byte user IRET frame", "validated lower-half canonical RIP/RSP", "ring-3 CS/SS and sanitized user RFLAGS", "SYSRETQ/IRETQ"],
      example: "examples/os/user_mode_features.sura",
      executed_gate: "tools/sura_ring3_qemu_gate.ps1",
      executed_source: "examples/os/ring3_qemu_gate.sura",
      executed_markers: ["SURA_RING3_READY", "SURA_RING3_4K_PAGES or SURA_RING3_SPLIT_LARGE_PAGE", "SURA_RING3_CPL3_SYSCALL_OK"],
      executed_path: ["ExitBootServices", "GDT/TSS/IDT activation", "4-KiB user code and NX stack permissions", "IRETQ entry with CS 35", "DPL-3 INT 0x80 with saved-CS check", "IRETQ return to CPL 3", "SYSCALL kernel-stack entry", "ring-0 debug exit"],
      desktop_process_gate: ["System Information, Terminal, File Explorer, Text Editor, and Calculator use distinct ProcessAddressSpace CR3 roots", "shared kernel PML4 entries have U/S cleared", "process-owned W^X code", "writable/NX mailboxes", "guarded writable/NX user stacks", "SURA_OS_SYSTEM_CR3_OK, SURA_OS_TERMINAL_CR3_OK, SURA_OS_FILES_CR3_OK, SURA_OS_EDITOR_CR3_OK, and SURA_OS_CALCULATOR_CR3_OK after the interrupt handler observes each process root and restores the kernel root"],
      limitations: ["entry primitive does not automatically select an address space or validate syscall pointers", "standalone executed gate is one fixed test context, not a scheduled desktop application process", "the graphical desktop uses UserProcessScheduler rather than the standalone user.enter primitive and does not load those workers from ELF files", "generic interrupt wrapper is not an NMI/MCE paranoid-entry implementation", "no KPTI or comprehensive speculative-entry hardening", "no FPU/SIMD save"],
    },
    process_address_space_and_elf64: {
      address_space: ["new lower-half user PML4", "supervisor-only shared upper-half kernel entries", "automatic 4-KiB intermediate-table allocation", "empty-table reclamation", "fixed-capacity ownership and mapping records", "guarded writable NX stacks"],
      security_checks: ["nonzero lower-half canonical ranges", "non-present nonzero conflict rejection", "U/S page permissions", "W^X leaves", "complete-range copy preflight", "PTE-to-owned-physical-page agreement", "active-CR3 destruction rejection"],
      user_copy: ["process_copy_to_user", "process_copy_from_user"],
      elf_scope: "little-endian System V/Linux x86-64 static ET_EXEC",
      elf_checks: ["ELF identity and fixed header sizes", "bounded complete program-header table", "file and memory overflow bounds", "sorted readable PT_LOAD segments", "power-of-two alignment and page-offset congruence", "no page-overlapping loads", "no W+X", "entry inside executable load segment", "non-executable GNU stack"],
      elf_load: ["zeroed pages", "file-byte copy", "p_memsz tail zero-fill", "final per-segment W^X permissions", "mapping rollback on failure"],
      libraries: ["stdlib/freestanding/process_memory.sura", "stdlib/freestanding/elf64.sura"],
      example: "examples/os/process_elf_features.sura",
      limitations: ["no ET_DYN, ASLR, relocation, interpreter, dynamic linking, or TLS", "no demand paging, copy-on-write, shared memory, or memory-mapped files", "no signal policy", "no PCID, KPTI, or remote TLB shootdown", "caller serializes mappings and provides identity-accessible physical pages", "compile and image verification only; self-check not executed in the current gate"],
    },
    user_process_lifecycle: {
      model: "fixed-capacity single-CPU ring-3 round-robin with a ring-0 idle frame",
      frame: ["168-byte normalized GP/error/RIP/CS/RFLAGS/RSP/SS layout", "checked lower-half RIP/RSP", "RPL-3 CS/SS", "IOPL/NT/VM rejection", "CLI, SWAPGS, LFENCE, IRETQ resume"],
      switching: ["per-process CR3", "per-process TSS RSP0", "per-process user GS base", "timer preemption", "DPL-3 vector-130 voluntary yield", "timer return to a saved Ring-0 service frame", "last-user cursor for round-robin order across kernel slices"],
      lifecycle: ["checked creation from ProcessAddressSpace", "optional per-process kernel event queue", "blocked state with saved IRET frame", "atomic wake-on-event post", "block/wake counters", "kernel-slice counter", "authenticated kernel-side event delivery and receive", "non-returning self-exit", "kernel termination of a non-current runnable or blocked process", "ring-3 page-fault termination with CR2/error capture", "state/result/fault lookup", "address-space destruction on reap"],
      library: "stdlib/freestanding/user_process.sura",
      example: "examples/os/user_process_features.sura",
      executed_gate: "tools/sura_user_process_qemu_gate.ps1",
      executed_markers: ["SURA_USER_PROCESS_PREEMPT_OK", "SURA_USER_PROCESS_IPC_OK", "SURA_USER_PROCESS_FAULT_ISOLATED", "SURA_USER_PROCESS_CR3_ISOLATED"],
      executed_scope: ["two runnable CPL-3 processes", "periodic local-APIC preemption of a non-yielding loop", "distinct process CR3 roots", "authenticated IPC delivery", "ring-3 page-fault termination", "continued kernel and peer-process execution"],
      kernel_slice_gate: "tools/sura_user_process_kernel_slice_qemu_gate.ps1",
      kernel_slice_source: "examples/os/user_process_kernel_slice_qemu_gate.sura",
      kernel_slice_markers: ["SURA_USER_PROCESS_KERNEL_SLICE_OK", "SURA_USER_PROCESS_INPUT_PROGRESS_OK", "SURA_USER_PROCESS_RESUME_OK", "SURA_USER_PROCESS_TERMINATE_OK"],
      kernel_slice_scope: ["one non-yielding CPL-3 infinite loop", "periodic APIC timer", "two returns to the saved Ring-0 input continuation", "process frame preservation", "kernel CR3 restoration", "reschedule and second user execution slice", "kernel termination with exit code 137", "address-space reap after termination"],
      persistent_calculator_gate: "tools/sura_persistent_calculator_qemu_gate.ps1",
      persistent_calculator_source: "examples/os/persistent_calculator_qemu_gate.sura",
      persistent_calculator_worker: "os/user_calculator.sura",
      persistent_calculator_markers: ["SURA_PERSISTENT_CALCULATOR_READY", "SURA_PERSISTENT_CALCULATOR_EVENTS_OK", "SURA_PERSISTENT_CALCULATOR_RESULT_OK", "SURA_PERSISTENT_CALCULATOR_SAME_PROCESS_OK", "SURA_PERSISTENT_CALCULATOR_CR3_OK"],
      persistent_calculator_scope: ["actual copied Sura OS calculator worker", "argument-bit opt-in preserving legacy one-shot mode", "initial empty-queue block", "four kernel wake events", "syscall receive into a separate event page", "2 + 3 = 5 state transitions", "same process ID, CR3, user frame, and kernel stack across five slices"],
      persistent_desktop_apps_gate: "tools/sura_persistent_desktop_apps_qemu_gate.ps1",
      persistent_desktop_apps_source: "examples/os/persistent_desktop_apps_qemu_gate.sura",
      persistent_desktop_workers: ["os/user_calculator.sura", "os/user_text_editor.sura", "os/user_file_explorer.sura", "os/user_terminal.sura", "os/user_system_info.sura", "os/user_window_server.sura"],
      persistent_desktop_apps_markers: ["SURA_PERSISTENT_DESKTOP_APPS_READY", "SURA_PERSISTENT_DESKTOP_CALCULATOR_OK", "SURA_PERSISTENT_DESKTOP_EDITOR_OK", "SURA_PERSISTENT_DESKTOP_FILES_OK", "SURA_PERSISTENT_DESKTOP_TERMINAL_OK", "SURA_PERSISTENT_DESKTOP_SYSTEM_OK", "SURA_PERSISTENT_DESKTOP_WINDOW_SERVER_OK", "SURA_PERSISTENT_DESKTOP_SAME_PROCESS_OK", "SURA_PERSISTENT_DESKTOP_CR3_OK"],
      persistent_desktop_apps_scope: ["six actual copied Sura OS workers", "six independent ProcessAddressSpace roots", "six event queues and kernel stacks", "one shared UserProcessScheduler", "initial block of every process", "one kernel wake and real mailbox transition per worker", "exact bounded Window Server surface composition", "return to blocked state without recreation", "distinct CR3 roots and stable process identities"],
      graphical_desktop_integration: ["seven workers created once by the graphical boot path", "seven independent event queues and kernel stacks", "dedicated Window Server ProcessAddressSpace with mapped GOP backbuffer pages", "bounded Ring-3 compositor read, write, verification, and original-pixel restoration", "dedicated Browser ProcessAddressSpace with a private 384-byte mailbox containing a copied URL snapshot and explicit network/storage/device capabilities", "blocking syscall wait and checked receive", "kernel event wake per desktop request", "APIC timer return to the Ring-0 service continuation", "page-fault handler terminates only the current Ring-3 process", "64-slice watchdog termination of a non-yielding graphical worker", "mouse polling while the non-yielding worker is interrupted", "faulted, exited, or timed-out worker disabled and its window closed without ending the kernel shell", "failed address-space reap plus worker/event-queue reconstruction", "replacement process with a new ID and CR3 primed back into blocked event wait", "previously visible app window reopened after recovery", "same saved frame, CR3 root, and kernel stack checked after repeated requests"],
      graphical_desktop_markers: ["SURA_OS_USER_SCHEDULER_READY", "SURA_OS_USER_PROCESSES_PERSISTENT_OK", "SURA_OS_WINDOW_SERVER_RING3_READY", "SURA_OS_WINDOW_SERVER_CR3_OK", "SURA_OS_WINDOW_SERVER_SHARED_BUFFER_OK", "SURA_OS_WINDOW_SERVER_RING3_OK", "SURA_OS_BROWSER_RING3_READY", "SURA_OS_BROWSER_RING3_OK", "SURA_OS_BROWSER_CR3_OK", "SURA_OS_BROWSER_PROCESS_ISOLATED_OK", "SURA_OS_USER_PROCESS_FAULT", "SURA_OS_USER_PROCESS_ISOLATED", "SURA_OS_USER_PROCESS_RESTARTED", "SURA_OS_USER_PROCESS_RESTART_EVENT_OK", "SURA_OS_USER_PROCESS_HANG_STARTED", "SURA_OS_USER_PROCESS_WATCHDOG", "SURA_OS_USER_PROCESS_HANG_INPUT_OK", "SURA_OS_USER_PROCESSES_CONCURRENT_OK", "SURA_OS_USER_PROCESS_BACKGROUND_OK", "SURA_OS_USER_PROCESS_HANG_RECOVERED"],
      worker_event_mapping: ["optional page-aligned OsUserWorkerConfig.event_address", "writable NX mapping", "separate from bounded mailbox", "checked complete-page user access"],
      limitations: ["standard GDT selectors 8/27/35", "shared supervisor-only kernel code/data/TSS/stacks required", "no preemption while a runnable process executes trusted ring-0 code", "no fast-syscall blocking/resume conversion", "no FPU/SIMD or debug-state save", "no signals, priorities, SMP queues, PCID, KPTI, remote TLB shootdown, or NMI-safe entry", "ordinary graphical request handlers wait until their requested worker blocks again or reaches the bounded 64-slice service limit, although other already-queued workers advance during those slices", "recovery starts a fresh built-in worker and does not checkpoint arbitrary user-process memory or execution state"],
    },
    ipc_event_queues: {
      model: "fixed-capacity caller-owned non-blocking FIFO queues with kernel-authenticated endpoints",
      message: ["sequence", "kind", "source process", "target process", "flags", "four u64 values"],
      behavior: ["FIFO push/peek/pop", "monotonic nonzero sequence", "full-queue rejection", "saturating dropped counter", "endpoint sender/target overwrite", "receiver ownership check"],
      library: "stdlib/freestanding/ipc.sura",
      example: "examples/os/ipc_features.sura",
      executed_gate: "tools/sura_ipc_qemu_gate.ps1 with SURA_IPC_QUEUE_OK",
      limitations: ["kernel must serialize operations on SMP", "non-blocking only", "caller owns queue storage"],
    },
    process_syscall_boundary: {
      numbers: { getpid: 0, send: 1, receive: 2, yield: 3, exit: 4, wait: 5 },
      user_copy: ["send copies IpcMessage through process_copy_from_user", "receive preflights writable range, peeks, copies through process_copy_to_user, then consumes"],
      security: ["current scheduler process authenticates sender", "kernel overwrites source and target", "target must be runnable or event-blocked with a queue", "invalid output pointers do not consume events", "empty-queue check and block are serialized with post-and-wake on the current single CPU"],
      entry_policy: ["getpid/send/receive are table and fast-SYSCALL compatible", "yield/exit/wait require process_software_syscall_dispatch and its saved IRET frame"],
      library: "stdlib/freestanding/process_syscall.sura",
      example: "examples/os/process_syscall_features.sura",
      executed_gate: "tools/sura_process_syscall_qemu_gate.ps1 with SURA_PROCESS_SYSCALL_OK",
      executed_scope: ["checked ProcessAddressSpace", "getpid", "forged-identity replacement", "process-to-process send", "safe receive copy-out", "empty queue", "invalid user address"],
      blocking_wait_gate: "tools/sura_process_wait_qemu_gate.ps1",
      blocking_wait_source: "examples/os/process_wait_qemu_gate.sura",
      blocking_wait_markers: ["SURA_PROCESS_WAIT_BLOCK_OK", "SURA_PROCESS_WAIT_WAKE_OK", "SURA_PROCESS_WAIT_RECEIVE_OK", "SURA_PROCESS_WAIT_EXIT_OK", "SURA_PROCESS_WAIT_CR3_OK"],
      blocking_wait_scope: ["two CPL-3 processes with distinct CR3 roots", "empty-queue wait transitions to blocked", "send authenticates and wakes target", "saved IRET frame resumes after wait", "receive copies into the target user address space", "both exit paths return to the saved Ring-0 frame"],
      limitations: ["receive itself remains non-blocking; user code calls wait then receive", "single shared scratch message and single-CPU serialization", "yield is not entered by the process syscall firmware gate", "no fast-SYSCALL blocking, yield, or exit resume conversion"],
    },
    pci_configuration_library: {
      mechanism: "legacy PCI configuration mechanism 1 through 0xCF8/0xCFC",
      operations: ["BDF validation", "8/16/32-bit config access", "probe", "vendor/device search", "class search", "capability traversal", "BAR decode", "command flags"],
      example: "examples/os/pci_features.sura",
      limitations: ["caller serializes shared config ports", "no BAR sizing/resource allocation", "no MSI/MSI-X setup", "no device-specific driver"],
    },
    pcie_configuration_library: {
      discovery: "checksum-validated ACPI MCFG with complete caller-owned region storage",
      validation: ["reserved fields", "1-MiB base alignment", "segment and bus bounds", "physical and same-segment bus overlap rejection", "address overflow checks"],
      operations: ["segment-aware 4-KiB ECAM access", "8/16/32-bit reads and writes", "class enumeration", "standard capability traversal", "extended capability traversal", "BAR decode", "command flags"],
      library: "stdlib/freestanding/pcie.sura",
      example: "examples/os/pcie_features.sura",
      limitations: ["caller maps ECAM uncached and serializes writes", "no executed configuration-space MMIO proof", "no BAR sizing or allocation", "no bridge setup", "no MSI/MSI-X, SR-IOV, ACS/IOMMU policy, or hot-plug"],
    },
    block_device_library: {
      contract: "fixed-width synchronous logical-block callbacks with checked geometry, LBA range, transfer overflow, and buffer alignment",
      operations: ["read", "write", "flush"],
      adapters: ["caller-owned RAM disk", "boot-stage UEFI Block I/O"],
      uefi_checks: ["native structure size and offsets", "snapshotted media ID and block size", "media-change rejection", "read-only state"],
      library: "stdlib/freestanding/block.sura",
      example: "examples/os/block_features.sura",
      limitations: ["caller owns storage, contexts, buffers, and serialization", "bool public result does not preserve detailed callback status", "first matching UEFI protocol only", "UEFI adapter invalid after ExitBootServices", "the current OS executes the generic block ABI through AHCI; RAM and UEFI adapters remain feature-image verified"],
    },
    ahci_library: {
      model: "polling AHCI 1.x SATA with one command slot and one PRDT entry",
      operations: ["PCI class discovery and ABAR extraction", "BIOS/OS ownership handoff", "HBA reset", "active SATA port discovery", "command engine stop/start", "ATA IDENTIFY", "48-bit DMA read/write", "cache flush", "BlockDevice binding"],
      dma_contract: ["caller-owned stable virtual and physical addresses", "registered contiguous DMA window", "uncached ABAR mapping", "non-DMA caller buffers staged through the DMA window in bounded sector-aligned chunks", "32-bit controller address-range enforcement"],
      library: "stdlib/freestanding/ahci.sura",
      example: "examples/os/ahci_features.sura",
      executed_verification: "The QEMU q35 OS path discovers ICH9 AHCI, identifies the second SATA disk, DMA-reads its MBR and FAT32 sectors, DMA-writes NOTES/SETTINGS/DESKTOP records, flushes, and remounts the modified image on a second boot",
      limitations: ["polling only; no interrupts or NCQ", "one outstanding command", "one PRDT entry and one registered DMA scratch window", "no ATAPI or port multiplier", "no hot-plug, TRIM, COMRESET recovery, or power management", "no IOMMU mapping"],
    },
    nvme_library: {
      model: "polling NVMe NVM command set with admin queue and I/O queue pair 1",
      operations: ["PCI class discovery and BAR extraction", "controller enable/disable", "controller and namespace Identify", "I/O completion phase tracking", "create I/O completion/submission queues", "read", "write", "flush", "BlockDevice binding"],
      dma_contract: ["caller-owned contiguous page-aligned queues", "registered page-aligned data window", "PRP1 plus one direct PRP2 page", "arbitrary caller buffers staged through the DMA window in bounded chunks", "4-KiB controller minimum page", "queue depth at most 64"],
      library: "stdlib/freestanding/nvme.sura",
      examples: ["examples/os/nvme_features.sura", "examples/os/nvme_qemu_gate.sura"],
      executed_verification: "The direct QEMU gate initializes admin and I/O queues, identifies the controller and namespace, validates a known sector, writes and flushes 8192 bytes through PRP1/PRP2, verifies device readback, and checks the same bytes in the host image. The complete OS selects NVMe before AHCI and mounts its FAT32 settings plus SuraFS document partition through the NVMe BlockDevice.",
      verification: ["tools/sura_nvme_qemu_gate.ps1", "tools/sura_os_nvme_qemu_gate.ps1", "SURA_NVME_EXECUTED_OK", "SURA_OS_STORAGE_NVME_READY"],
      limitations: ["QEMU-only; no physical-hardware proof", "no MSI/MSI-X or multiple I/O queue pairs", "no concurrent commands", "no PRP lists or SGLs", "no namespace-list selection", "no controller shutdown or abort/reset recovery", "no asynchronous events, IOMMU mapping, or zoned namespaces", "rejects separate LBA metadata"],
    },
    xhci_library: {
      model: "polling xHCI controller with simultaneous directly attached boot-keyboard and boot-mouse interrupt-IN endpoints",
      operations: ["PCI class discovery and MMIO BAR extraction", "PCI memory and bus-master enablement", "capability parsing", "legacy BIOS/OS ownership handoff", "controller stop/reset/run", "DCBAA, command ring, event ring, and ERST configuration", "connected-port counting and ordinal selection", "port reset", "Enable Slot", "Address Device", "endpoint-0 Setup/Data/Status transfers", "Device and Configuration descriptor reads", "boot HID interface and interrupt-IN endpoint discovery", "Configure Endpoint", "SET_CONFIGURATION", "SET_PROTOCOL boot", "Normal TRB HID report receive", "nonblocking transfer-event polling", "USB usage-to-KeyEvent conversion", "boot-mouse-to-PointerEvent conversion", "Disable Slot"],
      dma_contract: ["one physically contiguous page-aligned 16-KiB controller region for DCBAA, command ring, event ring, and ERST", "one physically contiguous page-aligned 24-KiB region per device for output/input contexts, endpoint-0 ring, descriptor buffer, interrupt-IN ring, and report buffer", "the desktop reserves one 64-KiB identity-mapped arena for controller, keyboard, and mouse", "controller-coherent DMA mappings owned by the caller"],
      libraries: ["stdlib/freestanding/xhci.sura", "stdlib/freestanding/usb_hid.sura", "stdlib/freestanding/pointer_event.sura", "os/input.sura"],
      examples: ["examples/os/xhci_features.sura", "examples/os/usb_hid_features.sura", "examples/os/xhci_qemu_gate.sura", "examples/os/xhci_mouse_qemu_gate.sura", "examples/os/xhci_input_integration_features.sura"],
      executed_verification: "The keyboard gate enumerates QEMU usb-kbd, injects A, verifies 00 00 04 00 00 00 00 00 and its KeyEvent; the mouse gate selects the second port, enumerates usb-mouse, injects movement, and verifies its raw report and PointerEvent; the full OS gate enumerates both simultaneously, selects xHCI over PS/2, injects Shift and movement, and requires SURA_OS_XHCI_INPUT_READY, SURA_OS_KEYBOARD_OK, and SURA_OS_MOUSE_OK",
      limitations: ["no physical-hardware proof", "controllers requiring scratchpad buffers are rejected", "desktop device roles currently assume first connected root port is the boot keyboard and second is the boot mouse", "no hubs, route strings, arbitrary device order, arbitrary HID report-descriptor parser, or non-boot HID", "polling only; no IRQ/MSI", "no hot-plug recovery, isochronous transfers, or power management"],
    },
    virtio_gpu_library: {
      model: "polling modern VirtIO PCI 2D scanout with a caller-owned split control queue and identity-mapped backing",
      transport: ["PCI device 1af4:1050", "vendor-specific common/notify/ISR/device capability mapping", "VIRTIO_F_VERSION_1 negotiation", "queue-zero descriptor/available/used areas", "BAR-relative queue notification"],
      commands: ["GET_DISPLAY_INFO", "RESOURCE_CREATE_2D B8G8R8X8_UNORM", "RESOURCE_ATTACH_BACKING", "SET_SCANOUT", "TRANSFER_TO_HOST_2D", "RESOURCE_FLUSH", "RESOURCE_UNREF"],
      libraries: ["stdlib/freestanding/virtio_pci.sura", "stdlib/freestanding/virtio_gpu.sura", "stdlib/freestanding/framebuffer.sura", "os/sura_os.sura"],
      executed_verification: "The direct QEMU gate disables GOP, transfers a deterministic 640x480 four-color image, captures the VirtIO display, checks five exact RGB samples, and releases the resource. The full OS keeps GOP primary, mirrors full frames and cursor damage to VirtIO, requires SURA_OS_VIRTIO_GPU_READY, and separately boots without the device while requiring SURA_OS_VIRTIO_GPU_FALLBACK.",
      verification: ["examples/os/virtio_gpu_qemu_gate.sura", "tools/sura_virtio_gpu_qemu_gate.ps1", "tools/sura_os_vm.ps1"],
      limitations: ["QEMU-only; no physical-hardware proof", "polling only; no IRQ/MSI-X", "one scanout and no cursor queue", "no EDID or display hot-plug/config-change recovery", "no resource blobs, virgl, Venus, or 3D command submission", "no IOMMU mapping"],
    },
    hda_audio_library: {
      model: "polling Intel High Definition Audio PCM output with one caller-owned BDL and identity-mapped sample buffer",
      controller: ["PCI class 04/03/00 discovery", "BAR0 MMIO", "GCTL reset", "STATESTS codec discovery", "ICOI/ICII/ICIS immediate-command verbs", "output stream descriptor reset/run/stop", "LPIB progress polling"],
      codec: ["root-node enumeration", "Audio Function Group discovery", "Audio Output and Pin Complex widget discovery", "power state", "pin output enable", "converter stream/channel assignment", "converter format", "stereo output amplifier gain"],
      pcm_contract: ["48 kHz", "signed 16-bit little-endian", "stereo", "one 192000-byte BDL entry", "caller-owned contiguous 48-page DMA region", "stream tag 1"],
      libraries: ["stdlib/freestanding/hda.sura", "os/sura_os.sura"],
      executed_verification: "The direct QEMU gate generates a one-second bipolar square wave, transfers it through the emulated ICH9 HDA output stream, requires LPIB progress, captures the host WAV backend, and parses 48-kHz signed-16 stereo PCM with observed -12000 and +12000 samples. The full OS transfers a bounded startup slice, requires SURA_OS_HDA_AUDIO_READY, and separately boots without the device while requiring SURA_OS_HDA_AUDIO_UNAVAILABLE.",
      verification: ["examples/os/hda_qemu_gate.sura", "tools/sura_hda_qemu_gate.ps1", "tools/sura_os_vm.ps1"],
      limitations: ["QEMU-only; no physical-hardware proof", "polling only; no IRQ/MSI", "one fixed-format output stream and one BDL entry", "no PCM mixer, application audio service, input capture, jack sensing, unsolicited codec responses, CORB/RIRB command transport, power management, hot-plug recovery, or IOMMU mapping"],
    },
    acpi_power_library: {
      model: "conventional ACPI fixed-hardware S5 power-off and FADT reset for the freestanding x86-64 image",
      discovery: ["UEFI ACPI 2.0/1.0 RSDP", "checksum-valid FADT and DSDT", "X_DSDT with DSDT fallback", "bounded static _S5 Name(Package) parsing", "legacy or extended PM1a/PM1b control descriptions", "FADT RESET_REG and RESET_VALUE"],
      register_spaces: ["System I/O 8/16-bit access", "System Memory 8/16-bit access", "SMI_CMD ACPI_ENABLE transition when SCI_EN is clear"],
      os_integration: ["SURA_OS_ACPI_POWER_READY", "shutdown through PM1 SLP_TYP/SLP_EN", "reboot through RESET_REG", "SURA_OS_ACPI_POWER_OFF_ARMED", "SURA_OS_ACPI_RESET_ARMED", "isa-debug-exit fallback only when firmware power control is unavailable"],
      libraries: ["stdlib/freestanding/acpi.sura", "stdlib/freestanding/power.sura", "os/sura_os.sura", "os/user_terminal.sura"],
      executed_verification: "The direct shutdown gate discovers FADT and DSDT _S5, enables ACPI mode, writes PM1 control, and requires QEMU exit 0. The direct reset gate writes RESET_VALUE through RESET_REG and requires QEMU -no-reboot exit 0. The complete OS gate sends reboot through the persistent CPL-3 Terminal worker and requires SURA_OS_ACPI_RESET_ARMED; the normal VM sends shutdown and requires SURA_OS_ACPI_POWER_OFF_ARMED.",
      verification: ["examples/os/power_shutdown_qemu_gate.sura", "tools/sura_power_shutdown_qemu_gate.ps1", "examples/os/power_reset_qemu_gate.sura", "tools/sura_power_reset_qemu_gate.ps1", "tools/sura_os_reboot_qemu_gate.ps1", "tools/sura_os_vm.ps1"],
      limitations: ["QEMU/OVMF execution only; no physical-hardware proof", "no general AML interpreter or _PTS execution", "no hardware-reduced ACPI sleep-control register path", "no suspend states, wake-source management, battery telemetry, thermal policy, or CPU performance-state policy", "the static AML parser is serialized through fixed scratch storage"],
    },
    fat32_library: {
      mount_checks: ["boot signature", "matching sector geometry", "power-of-two cluster size", "FAT32 cluster-count range", "FAT version and root cluster", "overflow and volume bounds"],
      operations: ["cluster-chain traversal", "uppercase space-padded 8.3 directory lookup and listing", "checked VFAT LFN creation, lookup, listing, rename, and deletion", "strict UTF-8 to UTF-16 conversion", "255 UTF-16-code-unit long names", "collision-checked deterministic 8.3 aliases", "malformed-LFN short-alias fallback", "complete and partial regular-file read", "same-size existing-file overwrite", "mirrored FAT-entry update", "zeroed cluster allocation", "bounded chain free", "short-name file and directory creation", "regular-file resize with zeroed newly visible bytes", "partial regular-file write", "same-directory rename", "empty-directory and regular-file deletion", "flush"],
      library: "stdlib/freestanding/fat32.sura",
      example: "examples/os/fat32_features.sura",
      mutation_gate: ["examples/os/fat32_mutation_qemu_gate.sura", "tools/sura_fat32_mutation_qemu_gate.ps1", "SURA_FAT32_REMOUNT_OK", "SURA_FAT32_LFN_UTF8_OK", "SURA_FAT32_LFN_CORRUPT_OK", "SURA_FAT32_MUTATION_OK"],
      executed_verification: "A QEMU-booted UEFI image formats a minimum legal FAT32 RAM disk, checks both FAT copies during three-cluster growth, executes sector- and cluster-spanning partial I/O, shrink/regrow zero exposure checks including same-cluster-count truncation, short-name rename, nonempty-directory rejection and tree deletion, then remounts the same bytes through fresh volume state, reads persisted content, deletes the file, and verifies every chain cluster is free. It creates 문서/메모.txt, resolves alias collisions, renames it to 긴 한글 메모 파일.sura, remounts and reads the data, removes the Korean tree, creates and remounts a maximum 255-code-unit name across a newly allocated directory cluster, and verifies a deliberately corrupted LFN checksum is ignored in favor of the short alias. The graphical OS separately mounts its generated FAT32 data image, traverses root and DOCS short names, and still uses fixed-size NOTES/SETTINGS/DESKTOP overwrites.",
      limitations: ["no Unicode normalization or non-ASCII case folding", "no general string path parser; the OS supplies bounded click navigation", "no cross-directory move", "no FAT repair or FSInfo free-count update", "no timestamps or permissions", "no interrupted-metadata recovery, journaling, or locking", "allocation scans from cluster 2 and a failed write can leak space", "desktop mutation/LFN integration is pending"],
    },
    fat32_vfs: {
      library: "stdlib/freestanding/fat32_vfs.sura",
      model: "caller-owned mounted FAT32 volume, shared sector/LFN workspace, scratch entry, and fixed exclusive-open handle array",
      operations: ["canonical relative UTF-8 component resolution", "root/file/directory stat", "UTF-8 directory listing", "file and directory creation", "open-create/truncate/append", "partial read/write with automatic growth", "explicit resize", "flush and close", "same-directory rename", "regular-file and empty-directory removal", "filesystem sync"],
      executed_verification: ["examples/os/fat32_mutation_qemu_gate.sura", "tools/sura_fat32_mutation_qemu_gate.ps1", "/문서/메모.txt VFS create/write/stat/list", "exclusive duplicate-open rejection", "/문서/기록.sura rename and append", "nonempty-directory rejection", "fresh FAT32 remount and absence check", "SURA_FAT32_VFS_UTF8_OK", "SURA_FAT32_VFS_REMOUNT_OK"],
      limitations: ["synchronous shared workspace requires caller serialization", "one exclusive handle per directory entry", "no nonempty recursive deletion", "no cross-directory move", "no permissions, timestamps, asynchronous I/O, or cache coherence", "graphical desktop integration is pending"],
    },
    partition_libraries: {
      gpt_checks: ["primary or backup header CRC32", "usable-LBA and entry-array bounds", "nonzero disk GUID", "entry geometry", "complete partition-entry-array CRC32"],
      gpt_operations: ["indexed entry traversal", "sector-spanning entry copy", "type-GUID lookup"],
      mbr_operations: ["four primary entries", "type-code lookup", "boot flag and device-range validation"],
      unified_lookup: "EFI System Partition through authoritative GPT, with type-0xEF MBR fallback only when neither GPT validates",
      libraries: ["stdlib/freestanding/gpt.sura", "stdlib/freestanding/partition.sura"],
      examples: ["examples/os/gpt_features.sura", "examples/os/partition_features.sura"],
      limitations: ["no extended MBR/EBR chains", "no hybrid-disk reconciliation", "no GPT repair", "no partition create, resize, or delete", "compile and image verification only"],
    },
    virtual_filesystem_library: {
      model: "caller-owned fixed-capacity mount table and file objects",
      paths: "caller-owned canonical absolute UTF-8 byte buffers with explicit lengths; rejects NUL, repeated/trailing separators, and dot segments",
      operations: ["mount", "unmount", "longest-prefix resolution", "open", "read", "write", "whole-file replace when supplied by the backend", "append", "seek", "resize", "flush", "close", "stat", "create file or directory", "list directory", "same-mount rename", "bounded allocation-free file or complete directory-tree copy with rollback and descendant rejection", "nonrecursive or recursive remove", "sync"],
      library: "stdlib/freestanding/vfs.sura",
      example: "examples/os/vfs_features.sura",
      limitations: ["no allocation", "no internal synchronization", "no permissions enforcement or process ownership", "no path normalization, current directory, or symbolic links", "cross-mount rename is rejected", "the standalone mount-dispatch example has compile and image verification; the memfs backend has separate QEMU execution proof"],
    },
    writable_memory_filesystem: {
      model: "volatile fixed-capacity hierarchical filesystem with caller-owned node, UTF-8 name-slot, and file-data-slot storage",
      operations: ["create file or directory", "open", "read", "write", "atomic whole-file replace", "append", "resize", "flush", "close", "stat", "list", "rename across directories", "nonrecursive remove", "recursive remove"],
      utf8_validation: ["rejects NUL and slash inside components", "rejects dot and dot-dot components", "rejects truncated, overlong, surrogate, and out-of-range UTF-8"],
      libraries: ["stdlib/freestanding/vfs.sura", "stdlib/freestanding/memfs.sura"],
      example: "examples/os/memfs_features.sura",
      executed_verification: ["tools/sura_memfs_qemu_gate.ps1", "UEFI execution in QEMU", "Korean directory and file names", "exact write/read", "atomic whole-file replacement with one generation change", "resize", "directory listing", "rename", "append", "bounded complete directory-tree copy", "exact copied 21-byte file", "copy-into-descendant rejection", "open-create", "open-truncate", "nonrecursive rejection", "recursive deletion", "SURA_MEMFS_UTF8_OK", "SURA_MEMFS_COPY_TREE_OK", "SURA_MEMFS_MUTATION_OK"],
      limitations: ["volatile memory only when used without SuraFS", "fixed node count, name-slot size, and per-file data-slot size", "no allocation", "no sparse extents", "no symbolic links", "no permissions enforcement or process ownership", "no timestamps beyond monotonic mutation generations", "no internal synchronization, journaling, crash recovery, or persistent disk format by itself", "not exposed as a separate volatile graphical desktop mount"],
    },
    surafs_persistent_foundation: {
      model: "fixed-capacity persistent filesystem using alternating complete copy-on-write banks and one checksummed superblock per bank",
      commit_order: ["write inactive metadata/data bank", "flush block device", "publish inactive-bank superblock", "flush block device"],
      recovery: "mount validates both superblock and whole-bank CRC32 values, then selects the newest intact generation; a corrupt newest bank falls back to the prior generation",
      operations: ["UTF-8 file and directory creation", "read", "write", "atomic whole-file replace", "append", "resize", "stat", "directory listing", "rename", "nonrecursive remove", "recursive remove", "graphical File Explorer directory navigation and file opening", "graphical file and folder creation", "graphical inline UTF-8 rename", "graphical recycle-bin transfer and permanent deletion inside /휴지통", "graphical Ctrl+C/Ctrl+X/Ctrl+V file and complete directory-tree copy/move with bounded conflict suffixes", "graphical Text Editor UTF-8 cursor, selection highlight, Ctrl+A/C/X/V range editing, autosave, Save As, and .sura lexical highlighting"],
      graphical_layout: ["128 MiB AHCI data disk", "legacy FAT32 settings/desktop partition retained", "type-0x7f SuraFS partition at LBA 131072", "32 nodes", "64-byte UTF-8 name slots", "4096-byte per-file data slots", "/문서/메모.txt default document", "/휴지통 recycle-bin directory"],
      libraries: ["stdlib/freestanding/block.sura", "stdlib/freestanding/ahci.sura", "stdlib/freestanding/vfs.sura", "stdlib/freestanding/memfs.sura", "stdlib/freestanding/surafs.sura", "os/storage.sura", "os/sura_os.sura"],
      example: "examples/os/surafs_qemu_gate.sura",
      executed_verification: ["tools/sura_surafs_qemu_gate.ps1", "UEFI execution in QEMU", "Korean directory and file names", "alternating generations", "intentional newest-bank corruption", "previous-generation content recovery", "post-recovery append and deletion", "SURA_SURAFS_RECOVERY_OK", "SURA_SURAFS_MUTATION_OK", "tools/sura_surafs_gui_qemu_gate.ps1", "AHCI-backed graphical editor save through one atomic VFS replace per save", "graphical Korean directory navigation and file open", "graphical temp directory creation and /휴지통 transfer", "graphical agent.sura creation and renamed.sura persistence", "graphical 한글.sura creation through Korean composition", "more than six persisted directory entries with Home/End viewport scrolling", "Explorer Ctrl+C exact 43-byte UTF-8 file copy", "Explorer Ctrl+X cross-directory move into movebox", "recursive movebox - Copy directory-tree duplication with a second exact 43-byte file", "Text Editor Ctrl+A selection", "Text Editor Ctrl+C exact 43-byte clipboard copy", "Text Editor Ctrl+X CPL-3 range deletion", "Text Editor Ctrl+V bounded payload restoration", "exact /문서/copy.sura Save As duplicate after clipboard restoration", "bounded .sura syntax-color map for keywords, strings, comments, and numbers", "active editor path isolation from Explorer navigation scratch paths", "exact multiline ASCII-plus-Korean UTF-8 editor payload inspection", "remount-only second boot", "restored byte length and weighted checksum", "unchanged SuraFS generation and complete partition SHA-256", "SURA_OS_SURAFS_READY", "SURA_OS_SURAFS_CREATE_OK", "SURA_OS_SURAFS_RENAME_OK", "SURA_OS_SURAFS_TRASH_OK", "SURA_OS_FILES_COPY_READY", "SURA_OS_FILES_CUT_READY", "SURA_OS_FILES_PASTE_OK", "SURA_OS_EDITOR_SELECTION_OK", "SURA_OS_EDITOR_COPY_OK", "SURA_OS_EDITOR_CUT_OK", "SURA_OS_EDITOR_PASTE_OK", "SURA_OS_EDITOR_SYNTAX_OK", "SURA_OS_FILES_SCROLL_OK", "SURA_OS_SURAFS_SAVE_AS_OK", "SURA_OS_SURAFS_EDITOR_SAVE_OK", "SURA_OS_SURAFS_FILE_OPEN_OK", "SURA_OS_SURAFS_EDITOR_RESTORED bytes=43 checksum=103782"],
      limitations: ["whole-bank commits rather than extent-level copy-on-write", "fixed node count, UTF-8 name-slot size, and per-file data-slot size selected at format time", "no dynamic extent allocator or sparse files", "no permissions enforcement, ownership, links, internal locking, encryption, compression, online resize, bad-block handling, or background scrub", "CRC32 detects corruption but is not cryptographic authentication", "QEMU AHCI execution only; not installed or tested on a physical disk"],
    },
    png_decode_foundation: {
      model: "allocation-free bounded PNG-to-RGBA8 decoder with caller-owned output and workspace",
      supported: ["non-interlaced PNG", "8-bit grayscale", "8-bit grayscale-alpha", "8-bit RGB", "8-bit RGBA", "consecutive multiple IDAT chunks", "zlib-wrapped DEFLATE", "PNG filters 0 through 4", "unknown ancillary chunk skipping"],
      validation: ["PNG signature", "IHDR ordering and exact size", "bounded dimensions and arithmetic", "CRC32 for every chunk", "critical-chunk rejection", "IDAT ordering", "IEND placement", "zlib CMF/FLG", "Adler-32", "exact decompressed scanline size", "caller output and workspace bounds"],
      libraries: ["stdlib/freestanding/gzip.sura", "stdlib/freestanding/png.sura"],
      executed_verification: ["examples/os/png_qemu_gate.sura", "tools/sura_png_qemu_gate.ps1", "two consecutive IDAT chunks", "all PNG filter types 0 through 4", "exact 3x5 RGB-to-RGBA pixels", "CRC corruption rejection", "output-capacity rejection", "workspace-capacity rejection", "SURA_PNG_DECODE_OK after ExitBootServices"],
      limitations: ["no palette color type", "no sub-8-bit or 16-bit samples", "no Adam7 interlacing", "no color-profile or gamma processing", "the browser has a two-slot image path; the full QEMU gate executes a large live suralang.site PNG fetch, decode, layout, and paint"],
    },
    jpeg_decode_foundation: {
      model: "allocation-free bounded 8-bit baseline sequential Huffman JPEG-to-RGBA8 decoder with caller-owned output and 16-KiB workspace",
      supported: ["one-component grayscale", "three-component YCbCr", "1x1 through 2x2 sampling factors", "4:4:4", "4:2:2", "4:2:0", "8-bit quantization tables", "canonical DC and AC Huffman tables", "entropy 0xff/0x00 byte stuffing", "integer inverse DCT", "nearest-neighbor chroma upsampling", "YCbCr-to-RGB conversion"],
      validation: ["SOI and exact terminal EOI", "bounded segment lengths", "single baseline SOF0", "component identity and sampling bounds", "quantization-table presence", "Huffman-tree oversubscription", "scan component uniqueness and table presence", "baseline spectral-selection fields", "coefficient run and category bounds", "caller output and workspace capacity"],
      libraries: ["stdlib/freestanding/jpeg.sura"],
      executed_verification: ["examples/os/jpeg_qemu_gate.sura", "tools/sura_jpeg_qemu_gate.ps1", "AC-bearing 8x8 4:4:4 color gradient", "16x16 4:2:0 image with four luminance blocks", "8x8 grayscale image", "dimension and alpha checks", "bounded per-channel color checks", "malformed marker rejection", "output-capacity rejection", "workspace-capacity rejection", "SURA_JPEG_DECODE_OK after ExitBootServices"],
      limitations: ["no progressive JPEG", "no arithmetic coding", "nonzero restart intervals are rejected", "no CMYK or YCCK", "no 12-bit samples", "no ICC or color-profile processing", "the browser connects the decoder to its two-slot image path; live PNG painting is executed, while live JPEG painting is not a separate gate"],
    },
    html_dom_foundation: {
      model: "fixed-capacity allocation-free HTML element/text tree with caller-owned node and attribute arrays and immutable input slices",
      relationships: ["parent", "first child", "last child", "next sibling", "current open ancestor"],
      tags: ["html", "head", "body", "div", "p", "h1 through h6", "a", "span", "br", "img", "style", "script", "noscript", "meta", "link", "title", "ul", "ol", "li", "section", "header", "footer", "main", "nav", "form", "input", "button", "label", "unknown element preservation"],
      attributes: ["id", "class", "href", "src", "alt", "style", "width", "height", "type", "name", "value", "rel", "hidden", "action", "method", "placeholder", "checked", "disabled", "for", "unknown attribute preservation", "single-quoted", "double-quoted", "unquoted", "boolean"],
      parsing: ["ASCII-case-insensitive names", "UTF-8 text byte preservation", "raw script/style text through matching closing tags", "comments and declarations skipped", "self-closing and void elements", "bounded ancestor closing-tag recovery", "malformed and capacity failure rejection"],
      libraries: ["stdlib/freestanding/html_dom.sura"],
      executed_verification: ["examples/os/html_dom_qemu_gate.sura", "tools/sura_html_dom_qemu_gate.ps1", "exact parent/child/sibling tree", "Korean UTF-8 text slice", "href/src/class/alt attributes", "login-shaped form/label/input/button tree", "type/name/value/action/method/placeholder/for form attributes", "raw script text preserving a JavaScript less-than operator", "br, img, and input void elements", "comment skipping", "node-capacity rejection", "attribute-capacity rejection", "incomplete-comment rejection", "SURA_HTML_DOM_OK after ExitBootServices"],
      limitations: ["the graphical Text Browser uses a bounded 1024-node and 1024-attribute instance and falls back to its older flat tokenizer when DOM preparation fails", "this module recognizes form structure and attributes but does not own editable control state or submit forms", "no HTML5 adoption-agency/foster-parenting algorithm", "raw-text scanning is limited to script/style closing-tag recognition", "no entity decoding", "no CSS selector matching or box construction in this module"],
    },
    browser_form_foundation: {
      model: "allocation-free bounded browser form-control state with caller-owned mutable value slots and immutable DOM name/action/method slices",
      controls: ["text input", "password input", "checkbox", "submit input/button", "button", "disabled flag", "checked flag", "nearest ancestor form", "pointer focus", "Enter submission"],
      encoding: ["application/x-www-form-urlencoded", "ASCII alphanumeric and -._~ preserved", "space encoded as +", "other bytes encoded as uppercase %HH", "UTF-8 encoded byte by byte", "checked valueless checkbox defaults to on", "disabled, unchecked, unnamed, submit, and button controls excluded"],
      submission: ["default/GET action query", "HTTP/1.1 POST body", "current URL action", "absolute HTTP(S) action", "root-relative action", "301/302/303 continue as GET", "307/308 preserve POST body"],
      libraries: ["stdlib/freestanding/browser_form.sura"],
      integration: ["os/network.sura", "os/sura_os.sura"],
      executed_verification: ["examples/os/browser_form_qemu_gate.sura", "tools/sura_browser_form_qemu_gate.ps1", "initial and mutable text/password values", "UTF-8 Backspace", "checkbox toggle", "disabled focus rejection", "control-to-form ancestry", "exact uppercase percent encoding and + spaces", "successful-control filtering", "default on checkbox value", "serialization-capacity rejection", "SURA_BROWSER_FORM_OK after ExitBootServices", "SURA_OS_BROWSER_FORM_PAGE_OK", "SURA_OS_BROWSER_FORM_FOCUS_OK", "SURA_OS_BROWSER_FORM_INPUT_OK", "SURA_OS_BROWSER_FORM_SUBMIT_OK after the graphical browser builds POST /login with the serialized body", "build/os/SuraOS-browser-form.ppm", "build/os/SuraOS-browser-submitted.ppm"],
      limitations: ["32 controls with 128-byte value slots in the graphical browser", "serialized request is bounded by the 1024-byte HTTP request buffer", "no relative action without a leading slash", "no radio groups, textarea, select, file input, multipart/form-data, form validation, submitter name/value inclusion, cookies, sessions, autofill, or complete HTML successful-controls rules"],
    },
    browser_javascript_foundation: {
      model: "allocation-free bounded JavaScript source compiler and stack bytecode VM with caller-owned code, constant, binding, stack, global, and scratch storage",
      values: ["undefined", "null", "boolean", "signed 64-bit integer number subset", "immutable UTF-8 string slice"],
      bytecode: ["constants", "global read/write", "pop", "duplicate", "integer add/subtract/multiply/divide/modulo", "unary negate/not", "equality and ordering", "absolute jump", "conditional jump", "bounded indirect host call", "halt"],
      source_subset: ["integer and unescaped UTF-8 string literals", "true", "false", "null", "undefined", "let/const/var global declarations", "assignment to declared globals", "identifier reads", "unary ! and -", "+ - * / %", "== === != !== < <= > >=", "parentheses", "brace blocks", "if/else", "line and block comments", "exact document.getElementById(string).textContent = expression statement"],
      integration: ["stdlib/freestanding/browser_js.sura", "stdlib/freestanding/browser_js_source.sura", "stdlib/freestanding/browser_js_dom.sura", "stdlib/freestanding/html_dom.sura", "os/network.sura", "os/sura_os.sura"],
      graphical_bounds: ["16-KiB collected inline source", "512 instructions", "128 constants", "128 bindings/globals", "64-value operand stack", "8192 executed instructions", "32 DOM text-mutation overlays"],
      executed_verification: ["examples/os/browser_js_qemu_gate.sura", "tools/sura_browser_js_qemu_gate.ps1", "arithmetic, globals, taken branch, string equality, indirect host callback", "invalid opcode, stack underflow, constant/global/jump/argument rejection", "instruction-budget termination of an infinite jump", "SURA_BROWSER_JS_VM_OK after ExitBootServices", "examples/os/browser_js_source_qemu_gate.sura", "tools/sura_browser_js_source_qemu_gate.ps1", "source declarations, expressions, assignment, if/else, strict equality syntax", "unknown-name, string-escape, and code-capacity rejection", "SURA_BROWSER_JS_SOURCE_OK after ExitBootServices", "examples/os/browser_js_dom_qemu_gate.sura", "tools/sura_browser_js_dom_qemu_gate.ps1", "ID lookup, textContent mutation overlay, replacement and descendant suppression", "SURA_BROWSER_JS_DOM_OK after ExitBootServices", "SURA_OS_BROWSER_JS_OK after graphical inline script execution", "SURA_OS_BROWSER_JS_PAGE_OK after sura.local/javascript loads", "SURA_OS_BROWSER_JS_CLICK_OK after pointer hit-testing executes inline onclick and redraws changed text", "build/os/SuraOS-browser-javascript.ppm"],
      failure_isolation: "unsupported or over-capacity inline scripts record a bounded script error and do not abort HTML/CSS layout or page rendering",
      limitations: ["not a complete ECMAScript implementation", "integer-only number subset rather than IEEE-754 Number", "no string escapes or concatenation", "no arrays, objects, general properties, functions, closures, call stack, exceptions, promises, modules, eval, or garbage collector", "no external script fetch", "DOM writes are limited to exact ID-based textContent replacement", "events are limited to parsed inline onclick attributes", "onclick currently consumes the click before default link or form behavior", "no addEventListener, capture, bubbling, general Event object, timers, fetch, storage, or other Web APIs"],
    },
    browser_webassembly_foundation: {
      model: "allocation-free bounded WebAssembly MVP integer parser and interpreter with caller-owned module tables, operand stack, locals, and control stack",
      sections: ["custom", "type", "function", "export", "code"],
      values: ["i32", "i64"],
      execution: ["direct call", "block", "loop", "if/else", "br", "br_if", "return", "local.get", "local.set", "local.tee", "i32/i64 constants", "integer comparisons", "implemented i32/i64 arithmetic opcodes"],
      bounds: ["strict bounded LEB128", "fixed module-table capacity", "fixed operand/local/control capacity", "configurable call-depth limit", "configurable instruction limit"],
      integration: ["stdlib/freestanding/browser_wasm.sura", "os/network.sura", "HTTP or HTTPS response body magic-byte detection", "zero-argument exported run invocation", "i32/i64 result rendered as an HTML page", "sura.local/webassembly binary fixture returns 42"],
      executed_verification: ["examples/os/browser_wasm_qemu_gate.sura", "tools/sura_browser_wasm_qemu_gate.ps1", "arithmetic result 36", "direct call and both if branches", "infinite loop stopped at exactly 16 instructions", "bad magic rejection", "unsupported memory-section rejection", "malformed LEB128 rejection", "invalid branch-depth rejection", "insufficient table-capacity rejection", "SURA_BROWSER_WASM_OK after ExitBootServices", "SURA_OS_BROWSER_WASM_OK after graphical binary response execution", "SURA_OS_BROWSER_WASM_PAGE_OK after sura.local/webassembly renders result 42", "build/os/SuraOS-browser-webassembly.ppm"],
      validation: "bounded structural validation plus runtime type checks; not the complete WebAssembly specification validator",
      limitations: ["no imports, tables, linear memory, globals, start functions, elements, data, floating point, references, exceptions, threads, or SIMD", "no WASI", "no JIT", "no JavaScript WebAssembly object", "no fetch/instantiate integration"],
    },
    css_computed_style_foundation: {
      model: "fixed-capacity allocation-free CSS rule parser and DOM computed-style cascade with caller-owned rule, style, output, and scratch arrays",
      selectors: ["tag selector", ".class selector", "#id selector", "compound selectors such as div#hero and .foo.bar", "descendant selectors", "direct-child selectors", "inline style attribute", "tag/class/id specificity", "equal-specificity source-order override"],
      properties: ["display none/block/inline/inline-block/flex/inline-flex/grid/inline-grid", "color", "background and background-color", "width, height, min-width, max-width, min-height, and max-height", "integer px dimensions", "width percentages and viewport width", "viewport height", "bounded calc(<percent> - <px>) widths", "margin/padding/border-width longhands", "one-to-four-value margin/padding/border-width shorthands", "horizontal margin auto", "static/relative/absolute/fixed position", "signed px or auto top/right/bottom/left offsets", "width extraction from border and border-top/right/bottom/left compound shorthands", "font-size including bounded clamp(px,vw,px) geometry", "line-height including non-negative decimal multipliers", "box-sizing content-box/border-box", "justify-content", "align-items", "flex-wrap", "flex-direction row/column", "gap/row-gap/column-gap", "grid-template-columns explicit tokens or repeat(count,...)", "#RGB", "#RGBA", "#RRGGBB", "#RRGGBBAA with alpha discarded", "transparent", "up to 32 global :root custom properties resolved into var(--name) uses", "last supported hex color extracted from compound background values"],
      inheritance: ["color", "font-size", "line-height"],
      libraries: ["stdlib/freestanding/html_dom.sura", "stdlib/freestanding/css_style.sura"],
      executed_verification: ["examples/os/css_style_qemu_gate.sura", "tools/sura_css_style_qemu_gate.ps1", "exact body tag and class values", "ID specificity over a later tag rule", "compound, descendant, and direct-child matching", "inline style override", "inherited element and text colors", "display:none", "four-side margin/padding/border values", "compound border and directional border shorthand widths", "percentage, viewport, calc-minus-pixel, and min/max dimension resolution", "bounded clamp(px,vw,px) minimum/preferred/maximum resolution", "decimal line-height multiplier resolution", "relative/absolute/fixed position plus signed offsets", ":root custom-property collection and var() replacement", "4/8-digit hex color handling", "compound background final-color extraction", "flex/grid property parsing", "strict malformed-stylesheet rejection", "relaxed at-rule and unsupported-selector skipping", "supported selectors retained from comma lists", "SURA_CSS_STYLE_OK after ExitBootServices"],
      limitations: ["the graphical Text Browser uses the relaxed parser for inline CSS and one root-relative same-host external stylesheet", "custom properties are one global :root table without scoped cascade, fallback, or cycle handling", "hex alpha is discarded instead of composited", "no attribute, pseudo, or sibling selectors", "no !important, named colors, percentage heights, general calc(), em/rem, vertical auto-margin behavior, media queries, general variable-size web-font rendering, sticky position, z-index, or stacking contexts", "flex/grid parsing and layout remain bounded subsets"],
    },
    css_box_layout_foundation: {
      model: "fixed-capacity allocation-free DOM-to-box geometry pass consuming computed styles and emitting caller-owned CssBox records",
      geometry: ["border-box x/y/width/height", "content-box x/y/width/height", "outer width/height including margins", "content-box and border-box sizing", "automatic block width", "automatic or explicit height"],
      flow: ["vertical block flow", "inline element and text flow", "measured-width line wrapping", "display:none subtree removal", "relative boxes retaining normal flow", "absolute boxes using the nearest positioned ancestor", "fixed boxes using the viewport", "absolute/fixed removal from block/flex/grid flow", "bounded flex row/column direction, wrapping, pixel gaps, main-axis distribution, and cross-axis placement", "bounded equal-width grid with up to 16 explicit or repeat() columns and pixel gaps", "parent/first-child/last-child/next-sibling box links"],
      libraries: ["stdlib/freestanding/html_dom.sura", "stdlib/freestanding/css_style.sura", "stdlib/freestanding/css_box.sura"],
      executed_verification: ["examples/os/css_box_qemu_gate.sura", "tools/sura_css_box_qemu_gate.ps1", "exact root/html/body/div/text coordinates and dimensions", "margin/padding/border edge arithmetic", "fixed-height border-box content dimensions", "percentage/calc width plus min/max width and viewport-height clamping", "centered horizontal margin:auto distribution", "absolute right/top placement inside a relative containing block", "absolute box removal from following block flow", "hidden paragraph subtree omission", "box-capacity rejection", "40-pixel inline wrapping across text/span/text", "flex wrapping, gap, distribution, direction, and cross-axis cases", "grid explicit/repeat column placement with row and column gaps", "SURA_CSS_BOX_OK after ExitBootServices"],
      limitations: ["the graphical Text Browser paints at most 64 colored backgrounds, 32 text boxes, 256 complete UTF-8 bytes per text node, and 2048 UTF-8 text bytes per desktop frame", "text width approximates Unicode scalar count and font size instead of measuring glyphs", "no word breaking, shaping, bidirectional layout, baseline alignment, floats, overflow clipping, margin collapsing, tables, sticky positioning, z-index, stacking contexts, transforms, or complete containing-block behavior", "right/bottom placement needs known containing and positioned dimensions", "no flex grow/shrink/basis, ordering, reverse direction, align-content, per-item alignment, intrinsic grid tracks, named lines, auto-fit/auto-fill, or spanning", "one bounded global layout state makes the pass non-reentrant"],
    },
    virtio_network_and_browser: {
      libraries: ["stdlib/freestanding/virtio_net.sura", "stdlib/freestanding/net.sura", "stdlib/freestanding/http1.sura", "stdlib/freestanding/gzip.sura", "stdlib/freestanding/http_content.sura"],
      integration: ["stdlib/freestanding/browser_form.sura", "stdlib/freestanding/browser_js.sura", "stdlib/freestanding/browser_js_source.sura", "stdlib/freestanding/browser_js_dom.sura", "stdlib/freestanding/browser_wasm.sura", "os/user_browser.sura", "os/network.sura", "os/https.sura", "os/trust_store.sura", "os/sura_os.sura"],
      window_geometry: "work-area-sized Text Browser window; the earlier 720-by-420 default was removed",
      address_focus: "pointer click or F6 selects the address field; typing replaces the current address",
      input_interaction: ["clicking page content focuses the Browser window before hit testing", "mouse-wheel scrolling targets the Browser under the pointer", "a visible vertical scrollbar supports track clicks for page movement", "held Backspace redraws each repeated UTF-8 code-point deletion"],
      incremental_navigation: ["uncached address-bar DNS begin without a blocking receive loop", "bounded DNS receive polling from the desktop service loop", "dotted-decimal IPv4 literal parsing with DNS bypass", "TCP SYN begin plus bounded SYN-ACK polling from the desktop service loop", "ClientHello send plus bounded TLS outer-record and handshake polling from the desktop service loop", "one-shot consumption of a completed authenticated TLS connection by the HTTPS continuation", "keyboard and mouse dispatch while DNS, TCP connect, or TLS handshake is pending", "copied URL snapshot comparison", "F6 or address editing cancels a stale DNS, TCP, or TLS request", "scheme-less hostnames default to HTTPS while explicit http:// remains available", "response receive/decode and later stylesheet/image TLS connections remain synchronous cooperative paths", "F6 records a response-fetch cancel request and the next cooperative poll unwinds before restoring the old document and address focus", "other nested Browser address/content/request actions rejected while shared fetch buffers are active", "SURA_OS_BROWSER_NAV_ASYNC_BEGIN", "SURA_OS_BROWSER_NAV_ASYNC_INPUT_OK", "SURA_OS_BROWSER_NAV_ASYNC_DONE", "SURA_OS_BROWSER_NAV_TCP_BEGIN", "SURA_OS_BROWSER_NAV_TCP_INPUT_OK", "SURA_OS_BROWSER_NAV_TCP_DONE", "SURA_OS_BROWSER_NAV_TLS_BEGIN", "SURA_OS_BROWSER_NAV_TLS_DONE", "SURA_OS_BROWSER_NAV_FETCH_BEGIN", "SURA_OS_BROWSER_NAV_FETCH_INPUT_OK", "SURA_OS_BROWSER_NAV_FETCH_CANCEL_REQUESTED", "SURA_OS_BROWSER_NAV_FETCH_CANCELLED_OK"],
      ring3_request_boundary: ["persistent Browser worker with a distinct ProcessAddressSpace CR3", "private 384-byte mailbox", "copied address snapshot limited to 255 bytes", "bounded ASCII scheme/host/path validation", "external hosts receive network capability only", "sura.local receives no network capability", "storage and device capabilities remain denied", "initial requests, form actions, and redirect targets are re-authorized", "worker fault injection closes and reconstructs only the Browser process/window"],
      device: ["legacy-compatible VirtIO-net PCI discovery", "I/O BAR", "feature negotiation", "two 256-entry split virtqueues", "sixty-four posted 2048-byte RX buffers in a 160-KiB DMA region", "single polling TX descriptor"],
      protocols: ["Ethernet II", "DHCP Discover/Offer/Request/ACK", "leased IPv4 address, subnet, gateway, and DNS configuration", "ARP request/reply", "IPv4 header build and checksum validation", "dotted-decimal IPv4 address literals", "UDP", "DNS A query and compressed-name response traversal", "case-insensitive single-host boot-session DNS result cache", "TCP SYN/SYN-ACK/ACK", "TCP payload acknowledgement and FIN", "HTTP/1.1 GET", "HTTP/1.1 application/x-www-form-urlencoded POST", "Content-Length response framing", "Transfer-Encoding: chunked with extensions and trailers", "connection-close response framing", "gzip content coding with stored, fixed-Huffman, and dynamic-Huffman DEFLATE blocks", "TLS 1.3 TCP 443 stream", "TLS_AES_128_GCM_SHA256", "X25519", "RSA-PSS-RSAE-SHA256 CertificateVerify", "RSA/SHA-256 X.509 chains", "HTTP/1.1 ALPN", "up to five absolute, protocol-relative, or root-relative redirects"],
      browser: ["sixth managed TEXT BROWSER window", "keyboard-editable ASCII hostname, http:// URL, or https:// URL with path", "2-MiB response buffer", "shared fail-closed HTTP/HTTPS response framing", "in-place chunk decoding", "bounded gzip decoding with CRC32 and ISIZE validation", "Accept-Encoding: gzip, identity request", "two-set Korean composition in URL paths", "proportional antialiased printable-ASCII and modern-Hangul address/page rendering", "valid UTF-8 page-text preservation", "uppercase percent encoding of UTF-8 path bytes", "Enter-triggered DNS/TCP/HTTP or HTTPS navigation", "HTTP-to-HTTPS redirects", "certificate hostname, validity-time, chain, key-usage, basic-constraints, and explicit trust-anchor validation", "HTTP header removal", "bounded 1024-node and 1024-attribute DOM", "raw script/style text scanning", "inline style collection", "one root-relative same-host external stylesheet fetch", "relaxed supported-rule extraction", "32-entry :root custom-property resolution", "computed style and 1024-box layout with bounded percentage/calc/viewport/min-max dimensions, positioning, and flex/grid subsets", "bounded inline JavaScript source collection, compilation, and VM execution with failure isolation", "inline onclick hit-testing and execution", "32-entry getElementById(...).textContent mutation overlay", "bounded WebAssembly MVP integer response detection and zero-argument run export execution", "visible background, border, image, and UTF-8 text painting", "DOM link hit-testing with same-document fragment and root/absolute navigation", "wheel, arrow, Page Up/Down, Home, and End scrolling", "device-independent held-key repeat including UTF-8 Backspace", "text/password/checkbox/button form controls with pointer focus", "application/x-www-form-urlencoded successful-control serialization", "default/GET query submission", "HTTP/1.1 POST body submission", "Enter and submit-button activation", "POST-preserving 307/308 and GET-converting 301/302/303 redirects", "two-slot same-host PNG/baseline-JPEG fetch/decode/layout/paint", "older flat tokenizer fallback", "desktop icon and taskbar activation", "QMP framebuffer capture"],
      executed_verification: ["tools/sura_http1_qemu_gate.ps1", "SURA_HTTP1_FRAMING_OK after partial/complete Content-Length including large bodies, chunk-extension/trailer dechunk, connection-close, no-body, and invalid-framing cases", "tools/sura_gzip_qemu_gate.ps1", "SURA_GZIP_INFLATE_OK after stored, fixed-Huffman, dynamic-Huffman, CRC32, rejection, and complete HTTP gzip finalization cases", "tools/sura_browser_form_qemu_gate.ps1", "SURA_BROWSER_FORM_OK after exact successful-control serialization and bounds checks", "tools/sura_browser_js_qemu_gate.ps1", "SURA_BROWSER_JS_VM_OK after bytecode success and bounded failure paths", "tools/sura_browser_js_source_qemu_gate.ps1", "SURA_BROWSER_JS_SOURCE_OK after source compilation, execution, and rejection paths", "tools/sura_browser_js_dom_qemu_gate.ps1", "SURA_BROWSER_JS_DOM_OK after DOM ID lookup and textContent overlay checks", "tools/sura_browser_wasm_qemu_gate.ps1", "SURA_BROWSER_WASM_OK after bounded MVP integer execution and rejection paths", "SURA_OS_NETWORK_READY", "SURA_OS_DHCP_OK", "SURA_OS_ARP_OK", "SURA_OS_UDP_OK", "SURA_OS_DNS_OK", "SURA_OS_TCP_OK", "SURA_OS_HTTP_OK", "SURA_OS_BROWSER_APP_OK", "SURA_OS_BROWSER_CSS_OK", "SURA_OS_BROWSER_FOCUS_OK", "SURA_OS_BROWSER_URL_OK after live example.org navigation", "SURA_OS_BROWSER_HTTPS_OK after live suralang.site HTTP-to-HTTPS redirect, TLS 1.3 handshake, certificate validation, and encrypted response", "SURA_OS_BROWSER_EXTERNAL_CSS_OK after the live same-host stylesheet fetch", "SURA_OS_BROWSER_CSS_VARIABLES_OK after live :root var() resolution", "SURA_OS_BROWSER_CSS_POSITION_OK after live positioned-style computation", "SURA_OS_BROWSER_DOM_BOX_OK after live DOM/computed-style/box conversion", "SURA_OS_BROWSER_DOM_RENDER_OK after bounded background, border, image, and UTF-8 text painting", "SURA_OS_BROWSER_IMAGE_PNG_OK", "SURA_OS_BROWSER_IMAGE_RENDER_OK", "SURA_OS_BROWSER_LINK_OK after live anchor hit-testing", "SURA_OS_BROWSER_WHEEL_OK after QEMU xHCI USB mouse injection with PS/2 fallback available", "SURA_OS_BROWSER_SCROLL_OK after viewport movement", "SURA_OS_KEY_REPEAT_OK after held Page Down", "SURA_OS_BROWSER_BACKSPACE_REPEAT_OK after held Backspace deletes UTF-8 code points", "SURA_OS_BROWSER_JS_OK after the graphical built-in form page compiles and executes inline JavaScript", "SURA_OS_BROWSER_JS_PAGE_OK after sura.local/javascript loads", "SURA_OS_BROWSER_JS_CLICK_OK after QEMU clicks its inline onclick button and changes #status textContent", "SURA_OS_BROWSER_WASM_OK after graphical binary response execution", "SURA_OS_BROWSER_WASM_PAGE_OK after sura.local/webassembly renders result 42", "SURA_OS_BROWSER_FORM_PAGE_OK", "SURA_OS_BROWSER_FORM_FOCUS_OK", "SURA_OS_BROWSER_FORM_INPUT_OK", "SURA_OS_BROWSER_FORM_SUBMIT_OK after building POST /login with the serialized body", "SURA_OS_BROWSER_KOREAN_INPUT_OK", "build/os/SuraOS-browser.ppm", "build/os/SuraOS-browser-scrolled.ppm", "build/os/SuraOS-browser-form.ppm", "build/os/SuraOS-browser-submitted.ppm", "build/os/SuraOS-browser-javascript.ppm", "build/os/SuraOS-browser-webassembly.ppm", "build/os/SuraOS-browser-korean.ppm"],
      limitations: ["QEMU user networking is the executed environment", "polling only", "the Ring-3 Browser worker validates request metadata and capabilities, and address-bar DNS plus the first TCP connect are incremental desktop-loop states; TLS, response decoding, DOM construction, layout, painting, and link/form navigation remain a synchronous kernel call stack whose polling loops cooperatively service input; F6 can request a safe unwind, but other nested Browser edits/clicks/requests stay blocked until shared-buffer work returns", "no IPv6", "bounded connections without a complete retransmission timer, congestion control, or out-of-order reassembly", "HTTP content codings other than identity and gzip are rejected", "only one gzip member is accepted", "only exact Transfer-Encoding: chunked is accepted", "HTTPS has one algorithm profile and three pinned trust anchors: ISRG Root X1, DigiCert Global Root G2, and GlobalSign Root CA - R3; this is not a broad root store", "no certificate revocation checking, session resumption, broad cipher/signature negotiation, or arbitrary URL ports", "DNS host names remain ASCII because IDNA is not implemented", "DNS cache has one entry for the current boot and no TTL or negative caching", "UI glyph coverage is printable ASCII plus modern precomposed Hangul", "browser CSS supports bounded tag/class/ID compounds, descendant/direct-child matching, global :root variables, width percentages, min/max dimensions, viewport units, horizontal auto margins, relative/absolute/fixed positioning, and one root-relative same-host stylesheet", "the two image slots connect same-host PNG/baseline-JPEG fetch, decode, layout, and paint; the full live gate executes PNG, while live JPEG is not a separate gate", "forms are bounded to text/password/checkbox/button controls, URL-encoded GET/POST, current/absolute/root-relative actions, and a 1024-byte request buffer; no radio, textarea, select, file/multipart, validation, cookies, sessions, or complete HTML form model", "JavaScript is an integer/string/global/expression/if inline-source subset without arrays, general objects/properties, functions, external scripts, or general Web APIs; DOM mutation is one getElementById(...).textContent path and events are inline onclick only", "WebAssembly is a bounded MVP integer response runner without imports, memory, tables, globals, floating point, WASI, JIT, JavaScript integration, or complete specification validation", "onclick currently consumes a click before default link or form behavior", "no scoped CSS variables, percentage heights, general calc(), em/rem sizing, sticky position, z-index/stacking contexts, transforms, overflow clipping, complete flex/grid, cookies, HTTP object cache, history, or downloads"],
    },
    serial_and_vm_gate: {
      serial: ["16550 initialization", "bounded polling", "byte read/write", "buffer output"],
      gate: "tools/sura_qemu_boot_gate.ps1",
      source: "examples/os/qemu_boot_gate.sura",
      success_marker: "SURA_EXIT_BOOT_SERVICES_OK",
      limitations: ["QEMU and OVMF required for executed boot", "compile-only mode is not execution proof", "no ACPI UART discovery", "no interrupt-driven serial I/O"],
    },
    minimal_os_integration: {
      source: "os/sura_os.sura",
      ui_assets: ["os/icons.sura", "seven embedded 16x16 palette-index raster images", "generated printable-ASCII and modern-Hangul 2-bit antialiased font atlas", "desktop, window-title, Start-menu, taskbar, File Explorer, Text Editor, Calculator, and Text Browser rendering"],
      gate: "tools/sura_os_vm.ps1",
      output: ["build/os/SuraOS.efi", "build/os/SuraOS.img", "build/os/SuraData.img"],
      executed_path: ["UEFI entry and ExitBootServices", "GOP framebuffer with bounded backbuffer", "COM1 diagnostics", "physical-page self-check", "polling xHCI USB boot keyboard and mouse with PS/2 fallback", "SURA_OS_XHCI_INPUT_READY plus injected USB key and movement desktop dispatch", "recoverable xHCI-to-PS/2 and input-dispatch failure paths instead of kernel-loop termination", "geometric modern-Hangul UTF-8 rendering", "six managed application windows", "focus, z-order, drag, close, reopen, Start menu, taskbar, and RTC clock", "five persistent CPL-3 desktop processes under one UserProcessScheduler with distinct ProcessAddressSpace CR3 roots, event queues, kernel stacks, W^X code, NX mailboxes/event pages, and guarded user stacks", "SURA_OS_USER_SCHEDULER_READY", "SURA_OS_USER_PROCESSES_PERSISTENT_OK after all five workers preserve their saved frame, CR3 root, and kernel stack across real GUI requests", "faulted Calculator isolation, address-space reap, fresh worker/event queue, new process ID and CR3, real post-restart Calculator event, and continued Terminal status response", "non-yielding Calculator watchdog termination while mouse movement and a separately queued System Information process advance, second automatic reconstruction, real post-restart event, and continued Terminal status response", "SURA_OS_USER_PROCESS_RESTARTED", "SURA_OS_USER_PROCESS_RESTART_EVENT_OK", "SURA_OS_USER_PROCESS_HANG_INPUT_OK", "SURA_OS_USER_PROCESSES_CONCURRENT_OK", "SURA_OS_USER_PROCESS_BACKGROUND_OK", "SURA_OS_USER_PROCESS_HANG_RECOVERED", "ICH9 AHCI discovery and ATA IDENTIFY", "staged DMA reads and writes for caller-owned filesystem buffers", "legacy FAT32 settings and desktop-state compatibility", "type-0x7f SuraFS partition", "UTF-8 /문서/메모.txt File Explorer navigation and Text Editor save/open", "non-destructive 62-byte /문서/main.sura starter", "Explorer file/folder creation, inline rename, Korean filename composition, /휴지통 transfer, Ctrl+C copy, Ctrl+X move, and bounded recursive directory-tree paste", "4096-byte editor state with an 8288-byte three-page Ring-3 mailbox, UTF-8 cursor/selection boundaries, selection highlighting, Ctrl+A/C/X/V editing, Ctrl+F next match, bounded Ctrl+H replacement, .sura lexical colors, LF Enter, UTF-8 Backspace/Delete, cursor-following wrapping, Ctrl+S, and Save As", "calculator keyboard input, clickable 4x4 keypad, expression display, keyboard 50 - 31 = 19, and keypad 7 + 5 = 12", "SURA_OS_SURAFS_READY", "SURA_OS_SURAFS_CREATE_OK", "SURA_OS_SURAFS_RENAME_OK", "SURA_OS_SURAFS_TRASH_OK", "SURA_OS_FILES_COPY_READY", "SURA_OS_FILES_CUT_READY", "SURA_OS_FILES_PASTE_OK", "SURA_OS_EDITOR_SELECTION_OK", "SURA_OS_EDITOR_COPY_OK", "SURA_OS_EDITOR_CUT_OK", "SURA_OS_EDITOR_PASTE_OK", "SURA_OS_EDITOR_FIND_OK", "SURA_OS_EDITOR_REPLACE_OK", "SURA_OS_EDITOR_SYNTAX_OK", "SURA_OS_CALCULATOR_KEYPAD_OK", "SURA_OS_CALCULATOR_KEYPAD_RESULT_OK", "SURA_OS_SURAFS_SAVE_AS_OK", "SURA_OS_SURAFS_EDITOR_SAVE_OK", "SURA_OS_SURAFS_FILE_OPEN_OK", "SURA_OS_SURAFS_EDITOR_RESTORED bytes=43 checksum=103782", "exact graphical filesystem nodes, file/tree copies, editor clipboard restoration, Save As duplicate, and multiline ASCII-plus-Korean payload persistence across a remount-only second boot", "VirtIO-net split queues", "DHCP, ARP, IPv4, UDP, DNS, TCP, HTTP/1.1 Content-Length/chunked/connection-close framing, and the bounded TLS 1.3 HTTPS profile", "single-host boot-session DNS cache", "keyboard-editable browser URL", "live non-default-host example.org HTTP navigation", "live suralang.site HTTP-to-HTTPS redirect, certificate validation, encrypted response, and render through SURA_OS_BROWSER_HTTPS_OK", "bounded WebAssembly MVP integer response execution through zero-argument run, rendered result 42, SURA_OS_BROWSER_WASM_OK, and SURA_OS_BROWSER_WASM_PAGE_OK", "two-set Korean composition and geometric Hangul rendering in the address bar", "bounded h1/p/br/a HTML layout", "CSS body/div/h1/a selector colors", "64-entry kernel input-event queue round trip", "46x14 graphical terminal wrap, scroll, clear, shutdown, and reboot commands", "ACPI S5 power-off and FADT RESET_REG reboot"],
      desktop_ring3_workers: {
        system_information: ["process-owned W^X code", "writable/NX mailbox", "guarded stack", "distinct ProcessAddressSpace CR3", "CPL-3 bounded framebuffer/free-page/storage/network snapshot validation", "display-safe memory and pixel derivation", "ring-0 hardware gathering and rendering", "SURA_OS_SYSTEM_RING3_READY", "SURA_OS_SYSTEM_RING3_OK", "SURA_OS_SYSTEM_CR3_OK"],
        terminal: ["process-owned W^X code", "writable/NX mailbox", "guarded stack", "distinct ProcessAddressSpace CR3", "CPL-3 bounded line editing and command recognition", "ring-0 rendering and privileged command execution after return", "SURA_OS_TERMINAL_RING3_READY", "SURA_OS_TERMINAL_RING3_OK", "SURA_OS_TERMINAL_CR3_OK"],
        file_explorer: ["process-owned W^X code", "writable/NX mailbox", "guarded stack", "distinct ProcessAddressSpace CR3", "CPL-3 bounded selection validation", "ring-0 SuraFS VFS traversal, mutation, recycle-bin transfer, and UTF-8 rendering after return", "SURA_OS_FILES_RING3_READY", "SURA_OS_FILES_RING3_OK", "SURA_OS_FILES_CR3_OK"],
        text_editor: ["process-owned W^X code", "writable/NX three-page mailbox", "guarded stack", "distinct ProcessAddressSpace CR3", "CPL-3 packed UTF-8 insertion, selection replacement, range deletion, Backspace, Delete, clipboard-payload, and bounded replace state transitions", "ring-0 selection rendering, find-match selection, clipboard ownership, .sura lexical syntax map, SuraFS autosave, and Save As persistence after return", "SURA_OS_EDITOR_RING3_READY", "SURA_OS_EDITOR_RING3_OK", "SURA_OS_EDITOR_CR3_OK", "SURA_OS_EDITOR_FIND_OK", "SURA_OS_EDITOR_REPLACE_OK", "SURA_OS_EDITOR_SYNTAX_OK"],
        calculator: ["process-owned W^X code", "writable/NX mailbox", "guarded stack", "distinct ProcessAddressSpace CR3", "CPL-3 state transitions for keyboard 50 - 31 = 19 and graphical-keypad 7 + 5 = 12", "SURA_OS_CALCULATOR_RING3_READY", "SURA_OS_CALCULATOR_RING3_OK", "SURA_OS_CALCULATOR_CR3_OK", "SURA_OS_CALCULATOR_KEYPAD_RESULT_OK"],
      },
      shell_commands: ["help", "status", "mem", "about", "clear", "shutdown", "reboot"],
      hidden_diagnostic_commands: ["faultapp", "hangapp"],
      shell_gate_execution: "the non-interactive QEMU gate sends help, status, mem, about, clear, and shutdown through COM1 and requires ACPI S5 power-off; the complete-OS reboot gate sends reboot through the persistent CPL-3 Terminal worker and requires the FADT reset path; the graphical screenshot gate separately sends faultapp, requires Calculator page-fault isolation, automatic reconstruction with a new process, and one real post-restart event, then sends status; it next sends hangapp, injects mouse movement during the non-yielding user loop, requires watchdog isolation, another reconstruction and real event, then requires another live status before shutdown",
      interactive_command: ".\\tools\\sura_os_vm.ps1 -Engine .\\build\\SuraLanguage_os_next.exe -Interactive",
      screenshot_command: ".\\tools\\sura_os_screenshot.ps1 -Engine .\\build\\SuraLanguage_os_next.exe",
      screenshots: ["build/os/SuraOS-windows.ppm after drag", "build/os/SuraOS-start-menu.ppm with reopened System Information", "build/os/SuraOS-korean-input.ppm after rendered and persisted Korean input", "build/os/SuraOS-apps.ppm with File Explorer, antialiased Text Editor, and the clickable Calculator keypad", "build/os/SuraOS-browser.ppm with the fetched suralang.site HTTPS body", "build/os/SuraOS-browser-scrolled.ppm after live-page scrolling", "build/os/SuraOS-browser-form.ppm after pointer focus and mutable form input", "build/os/SuraOS-browser-submitted.ppm after URL-encoded POST construction and submission", "build/os/SuraOS-browser-javascript.ppm after inline onclick changes #status textContent", "build/os/SuraOS-browser-webassembly.ppm after run() returns 42", "build/os/SuraOS-browser-korean.ppm after Korean URL-path input", "build/os/SuraOS-desktop.ppm after Terminal activation"],
      execution_environment: "QEMU x86-64 TCG with EDK2/OVMF; interactive COM1 uses an ephemeral 127.0.0.1-only TCP bridge; no host boot or firmware-variable changes",
      limitations: ["graphical desktop milestone, not a complete desktop OS", "xHCI, PS/2, AHCI, VirtIO-net, HDA, and COM1 device access remains polling; the APIC kernel-slice path polls input between long user slices", "xHCI desktop enumeration assumes fixed root-port ordering; no hub, hot-plug, arbitrary HID, or physical-hardware proof", "window composition and bounded Window Server requests exist, but full app-owned presentation and a user-facing background-job API are not complete", "uncached address-bar DNS is incremental and cancelable; the later synchronous TCP/TLS/decode/layout call stack cooperatively services desktop input and accepts F6 as a safe fetch-cancel request, but other Browser address edits, content activation, and nested requests wait until shared-buffer work returns or cancellation finishes", "fault recovery rebuilds a fresh built-in app worker rather than restoring arbitrary process memory and instruction state", "Browser request validation runs in a dedicated Ring-3 worker, but networking, TLS, DOM, layout, rendering, hardware snapshot gathering, privileged terminal actions, FAT32 traversal, and storage persistence remain kernel-owned", "fixed kernel-owned Start/taskbar entries", "networking has no IPv6, complete TCP retransmission timer, congestion control, or out-of-order reassembly; HTTPS has one TLS 1.3 algorithm profile, three pinned trust anchors rather than a broad root store, no revocation check, and no session resumption", "browser HTML/CSS/JavaScript/WebAssembly support is bounded: compound/descendant/direct-child selectors, same-host external CSS, PNG/baseline-JPEG image slots, URL-encoded GET/POST form controls, flex/grid subsets, an inline integer/string/global/expression/if script subset, inline onclick, one getElementById(...).textContent mutation path, and a bounded MVP integer WebAssembly response runner exist; multipart/file forms, broad properties, complete layout, JavaScript objects/functions, general events, Web APIs, WebAssembly memory/imports/WASI, and complete specification validation do not", "does not execute every compile-verified freestanding subsystem", "HDA is one fixed-format output stream with no application mixer, input capture, or physical-hardware proof"],
    },
    acpi_madt: {
      library: "stdlib/freestanding/acpi.sura",
      example: "examples/os/acpi_features.sura",
      discovery: ["UEFI ACPI 2.0/1.0 GUID", "RSDP checksum", "XSDT with RSDT fallback", "SDT checksum", "MADT"],
      records: ["local APIC/x2APIC processors", "I/O APICs", "interrupt source overrides", "local APIC address override"],
      limitations: ["caller-owned fixed buffers", "firmware tables must stay mapped", "no interrupt routing", "does not itself allocate per-CPU state or start processors"],
    },
    ioapic_library: {
      discovery_input: "checked ACPI MADT I/O APIC and interrupt-source-override records",
      operations: ["I/O APIC identity/version validation", "GSI ownership lookup", "ISA polarity and trigger resolution", "fixed physical-destination redirection build", "masked-first route programming", "route mask update"],
      library: "stdlib/freestanding/ioapic.sura",
      example: "examples/os/ioapic_features.sura",
      limitations: ["caller maps MMIO uncached and serializes IOREGSEL/IOWIN", "8-bit physical APIC destinations only", "no logical or lowest-priority delivery", "no x2APIC interrupt remapping", "no NMI routes or automatic vector allocation", "no executed MMIO proof"],
    },
    boot_disk: {
      cli: "sura --target uefi-x86_64 --out BOOTX64.EFI --disk-image sura-os.img source.sura",
      layout: ["protective MBR", "primary and backup GPT", "FAT32 EFI System Partition"],
      payload: "EFI/BOOT/BOOTX64.EFI",
      reproducible: true,
    },
    atomics: ["load", "store", "exchange", "compare_exchange", "fetch_add", "fetch_sub", "fences"],
    interrupts: {
      declarations: ["func name(frame: ptr[Frame]) interrupt do", "func name(frame: ptr[Frame]) interrupt_error do"],
      saved_state: "all x86-64 general-purpose registers plus normalized error code and hardware RIP/CS/RFLAGS frame",
      return: "IRETQ after register restoration",
      gate_helper: "cpu.idt_set_gate(table, vector, addr_of(handler), selector, ist, attributes)",
      compile_time_checks: ["direct interrupt-function address", "vector error-code ABI", "vector/selector/IST/attributes ranges"],
    },
    surafs_verification_files: ["stdlib/freestanding/surafs.sura", "examples/os/surafs_qemu_gate.sura", "tools/sura_surafs_qemu_gate.ps1", "os/storage.sura", "os/sura_os.sura", "tools/sura_surafs_gui_qemu_gate.ps1", "tools/sura_os_data_disk.ps1"],
    surafs_gui_verification: ["fresh 128 MiB data image", "AHCI-backed first boot", "graphical editor mutation", "graphical UTF-8 directory navigation", "graphical UTF-8 file open", "temp directory creation and recycle-bin transfer", "agent.sura creation and renamed.sura rename", "한글.sura creation through graphical Korean composition", "more than six entries with Home/End viewport scrolling", "copy.sura Save As with exact 43-byte payload and lexical syntax-color activation", "active editor path isolation from Explorer navigation scratch paths", "direct active-bank parent/name/content inspection", "exact 43-byte multiline ASCII-plus-Korean on-disk payload inspection", "persisted second boot", "restored bytes=43 and checksum=103782 marker", "unchanged generation", "unchanged complete SuraFS partition SHA-256"],
    fat32_mutation_verification_files: ["stdlib/freestanding/fat32.sura", "stdlib/freestanding/fat32_vfs.sura", "examples/os/fat32_mutation_qemu_gate.sura", "tools/sura_fat32_mutation_qemu_gate.ps1"],
    hda_verification_files: ["stdlib/freestanding/hda.sura", "examples/os/hda_qemu_gate.sura", "tools/sura_hda_qemu_gate.ps1", "os/sura_os.sura", "tools/sura_os_vm.ps1"],
    power_verification_files: ["stdlib/freestanding/power.sura", "examples/os/power_shutdown_qemu_gate.sura", "examples/os/power_reset_qemu_gate.sura", "tools/sura_power_shutdown_qemu_gate.ps1", "tools/sura_power_reset_qemu_gate.ps1", "tools/sura_os_reboot_qemu_gate.ps1", "os/sura_os.sura", "os/user_terminal.sura", "tools/sura_os_vm.ps1"],
    persistent_desktop_verification_files: ["os/user_worker.sura", "os/user_calculator.sura", "os/user_text_editor.sura", "os/user_file_explorer.sura", "os/user_terminal.sura", "os/user_system_info.sura", "examples/os/persistent_calculator_qemu_gate.sura", "tools/sura_persistent_calculator_qemu_gate.ps1", "examples/os/persistent_desktop_apps_qemu_gate.sura", "tools/sura_persistent_desktop_apps_qemu_gate.ps1"],
    verification: ["tests/os_target_unit.cpp", "tests/freestanding_import_unit.cpp", "tools/sura_uefi_target_smoke.ps1", "tools/sura_ap_startup_smoke.ps1", "tools/sura_qemu_boot_gate.ps1", "tools/sura_integer_semantics_qemu_gate.ps1", "tools/sura_sha384_qemu_gate.ps1", "tools/sura_tls_crypto_qemu_gate.ps1", "tools/sura_trust_store_qemu_gate.ps1", "tools/sura_ring3_qemu_gate.ps1", "tools/sura_ipc_qemu_gate.ps1", "tools/sura_process_syscall_qemu_gate.ps1", "tools/sura_process_wait_qemu_gate.ps1", "tools/sura_user_process_qemu_gate.ps1", "tools/sura_user_process_kernel_slice_qemu_gate.ps1", "tools/sura_persistent_calculator_qemu_gate.ps1", "tools/sura_text_input_qemu_gate.ps1", "tools/sura_memfs_qemu_gate.ps1", "tools/sura_fat32_mutation_qemu_gate.ps1", "tools/sura_input_layout_qemu_gate.ps1", "tools/sura_xhci_qemu_gate.ps1", "tools/sura_xhci_mouse_qemu_gate.ps1", "tools/sura_virtio_gpu_qemu_gate.ps1", "tools/sura_os_foundation_verify.ps1", "tools/sura_os_vm.ps1", "tools/sura_os_screenshot.ps1", "tools/sura_os_data_disk.ps1", "os/sura_os.sura", "os/input.sura", "os/user_worker.sura", "os/user_system_info.sura", "os/user_terminal.sura", "os/user_file_explorer.sura", "os/user_text_editor.sura", "os/user_calculator.sura", "os/storage.sura", "os/network.sura", "os/https.sura", "os/trust_store.sura", "stdlib/freestanding/sha256.sura", "stdlib/freestanding/sha384.sura", "stdlib/freestanding/hkdf_sha256.sura", "stdlib/freestanding/aes128_gcm.sura", "stdlib/freestanding/x25519.sura", "stdlib/freestanding/tls13_record.sura", "stdlib/freestanding/tls13_handshake.sura", "stdlib/freestanding/tls13_hello.sura", "stdlib/freestanding/tls13_messages.sura", "stdlib/freestanding/tls13_verify.sura", "stdlib/freestanding/rsa_public.sura", "stdlib/freestanding/rsa_sha256.sura", "stdlib/freestanding/rsa_sha384.sura", "stdlib/freestanding/der.sura", "stdlib/freestanding/x509.sura", "stdlib/freestanding/x509_chain.sura", "stdlib/freestanding/process_memory.sura", "stdlib/freestanding/ipc.sura", "stdlib/freestanding/process_syscall.sura", "stdlib/freestanding/key_event.sura", "stdlib/freestanding/pointer_event.sura", "stdlib/freestanding/usb_hid.sura", "stdlib/freestanding/text_input.sura", "stdlib/freestanding/memfs.sura", "stdlib/freestanding/fat32.sura", "stdlib/freestanding/fat32_vfs.sura", "stdlib/freestanding/virtio_net.sura", "stdlib/freestanding/virtio_pci.sura", "stdlib/freestanding/virtio_gpu.sura", "stdlib/freestanding/net.sura", "stdlib/freestanding/framebuffer.sura", "stdlib/freestanding/font5x7.sura", "stdlib/freestanding/font_hangul.sura", "stdlib/freestanding/text_terminal.sura", "stdlib/freestanding/window_manager.sura", "stdlib/freestanding/desktop_shell.sura", "stdlib/freestanding/desktop_apps.sura", "stdlib/freestanding/rtc.sura", "stdlib/freestanding/ps2.sura", "stdlib/freestanding/xhci.sura", "examples/os/sha384_qemu_gate.sura", "examples/os/trust_store_qemu_gate.sura", "examples/os/tls_crypto_qemu_gate.sura", "examples/os/integer_semantics_qemu_gate.sura", "examples/os/virtio_gpu_qemu_gate.sura", "examples/os/framebuffer_features.sura", "examples/os/text_terminal_features.sura", "examples/os/window_manager_features.sura", "examples/os/desktop_shell_features.sura", "examples/os/desktop_apps_features.sura", "examples/os/rtc_features.sura", "examples/os/ps2_features.sura", "examples/os/usb_hid_features.sura", "examples/os/xhci_input_integration_features.sura", "examples/os/xhci_mouse_qemu_gate.sura", "examples/os/text_input_features.sura", "examples/os/memfs_features.sura", "examples/os/fat32_mutation_qemu_gate.sura", "examples/os/freestanding_features.sura", "examples/os/memory_kernel.sura", "examples/os/process_elf_features.sura", "examples/os/user_process_features.sura", "examples/os/user_process_qemu_gate.sura", "examples/os/user_process_kernel_slice_qemu_gate.sura", "examples/os/persistent_calculator_qemu_gate.sura", "examples/os/ipc_features.sura", "examples/os/process_syscall_features.sura", "examples/os/process_wait_qemu_gate.sura", "examples/os/ring3_qemu_gate.sura", "examples/os/scheduler_features.sura", "examples/os/preemptive_timer_features.sura", "examples/os/syscall_features.sura", "examples/os/user_mode_features.sura", "examples/os/pci_features.sura", "examples/os/pcie_features.sura", "examples/os/block_features.sura", "examples/os/ahci_features.sura", "examples/os/nvme_features.sura", "examples/os/xhci_features.sura", "examples/os/xhci_qemu_gate.sura", "examples/os/gpt_features.sura", "examples/os/partition_features.sura", "examples/os/fat32_features.sura", "examples/os/vfs_features.sura", "examples/os/qemu_boot_gate.sura", "examples/os/acpi_features.sura", "examples/os/ioapic_features.sura", "examples/os/ap_startup_features.sura"],
    not_implemented: ["executed AP-startup coverage and complete per-AP initialization lifecycle", "automatic per-CPU TSS/IST allocation", "FPU/SIMD process context-switch policy", "ET_DYN/PIE, relocation, interpreter, dynamic-linking, and TLS executable loading", "demand paging, copy-on-write, shared memory, and memory-mapped files", "signals and fast-syscall blocking/resume conversion", "KPTI, NMI-safe entry, and comprehensive speculative-entry hardening", "synchronized/NUMA physical-memory policy", "complete virtual address-space policy", "PCID and remote TLB shootdown", "SMP run queues and load balancing", "PCI/PCIe resource allocation, bridge setup, and MSI/MSI-X", "production network drivers, general USB hub/hot-plug/non-boot-HID drivers, accelerated graphics, a general application audio mixer/input service, physical audio-driver proof, and other device-specific drivers", "IRQ-driven input queues, scheduled-process input delivery, and complete resize/minimize/maximize/application window lifecycle", "extended MBR chains, GPT repair, and partition create/resize/delete", "scalable persistent extent allocation, permissions enforcement, links, locking, encryption, online resize, and physical-disk deployment", "ARM64 freestanding backend", "source-level freestanding debugger", "executed CI VM boot coverage"],
  },
  interop: {
    ffi_abi: "1.2.0",
    plugin_abi: "1.1.0",
    ffi_vm_calls: "serialized because the GC heap is process-global",
    ffi_handle_lifetime: "monotonic opaque tokens with per-call leases; close is deferred while a call is active",
    ffi_same_handle_reentry: "a different host thread receives SURA_ERR_BUSY while sura_run owns the handle",
    ffi_global_commit: "fallible map and GC-root construction completes before the visible globals are swapped",
    ffi_cross_run_values: {
      persistent: ["nil", "bool", "number", "string", "tensor", "array/dict whose reachable values are persistent"],
      run_bound: ["function/closure", "upvalue", "class instance", "array/dict that reaches a run-bound value"],
      successful_run: "top-level globals whose reachable graph is run-bound are omitted from persistent globals",
      failed_run: "if a failed run inserts a run-bound value into an existing persistent container, that handle clears its persistent globals",
      graph_scan_limit_objects: 1000000,
    },
    ffi_return_strings: "per-host-thread OS TLS/FLS buffer; valid until the next string/error getter on that thread",
    ffi_c_boundary: "C-compatible header; every exported function is noexcept and converts native exceptions to status/default values",
    optional_integrations: {
      ffmpeg: "local media/video decoding; select with SURA_FFMPEG or --ffmpeg where supported",
      curl: "native HTTP, async HTTP, and HTTP-backed LLM requests",
      nodejs: "JavaScript target and VS Code tooling; required by http.serve_routes and preferred by http.serve_static",
      python: "Python bridge and http.serve_static fallback",
      c_cpp_toolchain: "native FFI, plugin, embedding, and generated binding builds",
      cuda_driver_and_optional_cublas: "CUDA tensor operations",
    },
  },
  verification: {
    public_report: `/downloads/verification-${version}.json`,
    ...verificationManifest,
    latest_engineering_records: {
      release_evidence: {
        generated_utc: releaseEvidence.generated_utc,
        passed: releaseEvidence.passed,
        checks: `${releaseEvidence.passed_count}/${releaseEvidence.required_count}`,
        source: "/records/release-evidence.json",
      },
      goal_audit: {
        generated_utc: goalAudit.generated_utc,
        status: goalAudit.status,
        progress_percent: goalAudit.progress_percent,
        checks: `${goalAudit.passed_count}/${goalAudit.required_count}`,
        blockers: goalAudit.blocker_count,
        source: "/records/goal-audit.json",
      },
      target_lowering: {
        generated_utc: targetAudit.generated_utc,
        status: targetAudit.status,
        js: targetAudit.js,
        wasm: targetAudit.wasm,
        source: "/records/target-lowering-audit.json",
      },
    },
  },
  release: releaseManifest,
  performance_records: {
    current_native_baseline: {
      status: "verified fair-scope record",
      applies_to_version: version,
      source: "/records/native-performance.json",
      generated_utc: nativePerformance.generated_utc,
      engine: nativePerformance.engine,
      host: nativePerformance.host,
      compiler: nativePerformance.compiler_version,
      compiler_flags: nativePerformance.compiler_flags,
      measurement: "100,000-step inner physics loop; 5 Sura runs and 5 native C++ runs",
      vec2: { sura_jit_ms: currentVec2Perf.sura_jit_ms, cpp_o3_ms: currentVec2Perf.native_ms, ratio: currentVec2Perf.sura_native_ratio },
      vec3: { sura_jit_ms: currentVec3Perf.sura_jit_ms, cpp_o3_ms: currentVec3Perf.native_ms, ratio: currentVec3Perf.sura_native_ratio },
      fair_scope_passed: currentVec2Perf.fair_scope_passed && currentVec3Perf.fair_scope_passed,
    },
    latest_published_native_baseline: {
      status: performanceRecord.status,
      applies_to_version: performanceRecord.measured_language_version,
      current_release_measured: performanceRecord.measured_language_version === version,
      source: `/downloads/verification-${version}.json#performance_record`,
      archived_source: performanceRecord.source,
      archived_source_sha256: performanceSha256,
      generated_utc: performanceReport.generated_utc,
      engine: performanceReport.engine,
      host: performanceReport.host,
      compiler: performanceReport.compiler_version,
      compiler_flags: performanceReport.compiler_flags,
      measurement: "100,000-step inner loop; 30 Sura runs and 30 native runs",
      vec2: { sura_jit_ms: vec2Perf.sura_jit_ms, cpp_o3_ms: vec2Perf.native_ms, ratio: vec2Perf.sura_native_ratio },
      vec3: { sura_jit_ms: vec3Perf.sura_jit_ms, cpp_o3_ms: vec3Perf.native_ms, ratio: vec3Perf.sura_native_ratio },
      target_ratio: performanceRecord.target_ratio,
      target_met: performanceRecord.target_met,
    },
    native_baseline_2026_07_12: {
      environment: "Windows x64; Intel Core i5-12400F; g++ 15.2.0; 100,000-step inner loop",
      vec2: { sura_jit_ms: 17.225, cpp_o3_ms: 0.053, ratio: 324.09 },
      vec3: { sura_jit_ms: 62.254, cpp_o3_ms: 0.051, ratio: 1221.77 },
    },
    historical_gpu_record: {
      language_version: "1.8",
      engine_sha256_prefix: "3270a9",
      hardware: "RTX 4060",
      source: "Guide/GPU_AND_SCALE.md",
    },
    current_jit_gaps: ["full System V opcode/helper coverage", "broader ARM64 opcode/helper coverage", "macOS x86-64 native backend", "general function and method inlining", "general escape analysis beyond guarded 2/3-field no-alias record updates", "general register allocation", "loop-invariant code motion", "loop-carried XMM SSA"],
  },
  documentation_limits: {
    api_entries: `${apiSymbolCount} module entries contain canonical names, call signatures, and source locations; shared runtime limits and module-specific contracts are recorded in the prose and structured sections`,
    global_registry: `${globalBuiltinNames.length} case-sensitive direct-call names and aliases are enumerated`,
  },
  cli_help: runtimeHelp,
  package_manager_help: packageHelp,
};

machineFacts.freestanding.minimal_os_integration.executed_path =
  machineFacts.freestanding.minimal_os_integration.executed_path.map((item) =>
    item
      .replace("geometric modern-Hangul UTF-8 rendering", "proportional antialiased lowercase-ASCII and modern-Hangul UTF-8 rendering")
      .replace("two-set Korean composition and geometric Hangul rendering in the address bar", "two-set Korean composition and proportional antialiased rendering in the address bar and UTF-8 page body")
      .replace("focus, z-order, drag, close, reopen, Start menu, taskbar, and RTC clock", "focus, z-order, drag, bottom-right resize, minimize/taskbar restore, maximize/restore, fullscreen/restore, close, reopen, Start menu, taskbar, and RTC clock")
      .replace("five persistent CPL-3 desktop processes", "seven persistent CPL-3 desktop processes")
      .replace("all five workers", "all seven workers")
  );
machineFacts.freestanding.minimal_os_integration.executed_path.push(
  "NVMe-first data-disk selection with AHCI fallback, FAT32 settings, and SuraFS document mount",
  "dedicated persistent Ring-3 Window Server with its own CR3, event queue, kernel stack, and bounded two-surface damage compositor",
  "real GOP backbuffer pages mapped into the Window Server ProcessAddressSpace, followed by a verified pixel read/write and restoration",
  "SURA_OS_WINDOW_SERVER_RING3_READY, SURA_OS_WINDOW_SERVER_CR3_OK, SURA_OS_WINDOW_SERVER_SHARED_BUFFER_OK, and SURA_OS_WINDOW_SERVER_RING3_OK",
  "dedicated persistent Ring-3 Browser request validator with its own CR3, event queue, kernel stack, and private 384-byte URL/capability mailbox",
  "SURA_OS_BROWSER_RING3_READY, SURA_OS_BROWSER_RING3_OK, SURA_OS_BROWSER_CR3_OK, and SURA_OS_BROWSER_PROCESS_ISOLATED_OK",
);
machineFacts.freestanding.desktop_applications.executed_verification =
  machineFacts.freestanding.desktop_applications.executed_verification
    .replace("five-process graphical scheduler", "seven-process graphical scheduler")
    .replace("five distinct worker CR3 roots", "seven distinct worker CR3 roots") +
  "; it also executes the persistent Window Server worker, checks exact bounded two-surface composition, maps the real GOP backbuffer into that worker, verifies one pixel write, and restores the original pixel; the Browser request worker validates copied request metadata and capabilities under its distinct CR3, and faultbrowser verifies isolated reconstruction plus a real post-restart authorization";
machineFacts.freestanding.minimal_os_integration.desktop_ring3_workers.window_server = [
  "process-owned W^X code",
  "writable/NX mailbox",
  "guarded stack",
  "distinct ProcessAddressSpace CR3",
  "bounded 16x16 two-surface damage composition",
  "mapped real GOP backbuffer pages",
  "verified pixel read, write, and original-value restoration",
  "SURA_OS_WINDOW_SERVER_RING3_READY",
  "SURA_OS_WINDOW_SERVER_CR3_OK",
  "SURA_OS_WINDOW_SERVER_SHARED_BUFFER_OK",
  "SURA_OS_WINDOW_SERVER_RING3_OK",
];
machineFacts.freestanding.minimal_os_integration.desktop_ring3_workers.browser = [
  "process-owned W^X code",
  "private writable/NX 384-byte mailbox",
  "guarded stack",
  "distinct ProcessAddressSpace CR3",
  "copied address, scheme, host, and path validation",
  "network capability for external hosts and no network capability for sura.local",
  "storage and device capability denial",
  "initial, form-action, and redirect authorization",
  "isolated fault recovery with a new process ID and CR3",
  "SURA_OS_BROWSER_RING3_READY",
  "SURA_OS_BROWSER_RING3_OK",
  "SURA_OS_BROWSER_CR3_OK",
  "SURA_OS_BROWSER_PROCESS_ISOLATED_OK",
];
machineFacts.freestanding.virtio_network_and_browser.executed_verification.push(
  "SURA_OS_BROWSER_RING3_READY",
  "SURA_OS_BROWSER_RING3_OK after copied address and capability validation",
  "SURA_OS_BROWSER_CR3_OK",
  "SURA_OS_BROWSER_PROCESS_ISOLATED_OK after faultbrowser reconstructs the worker and a real request is re-authorized",
  "SURA_OS_BROWSER_NAV_TCP_BEGIN after the RFC 5737 TEST-NET literal starts a SYN attempt without DNS",
  "SURA_OS_BROWSER_NAV_TCP_INPUT_OK after F6 is dispatched while SYN-ACK is pending",
  "SURA_OS_BROWSER_NAV_TCP_DONE after the replacement live connection completes",
  "SURA_OS_BROWSER_NAV_TLS_BEGIN after ClientHello is sent on the live suralang.site connection",
  "SURA_OS_BROWSER_NAV_TLS_DONE after the authenticated handshake completes incrementally",
  "SURA_OS_BROWSER_NAV_FETCH_BEGIN during live suralang.site response/resource work",
  "SURA_OS_BROWSER_NAV_FETCH_INPUT_OK after QMP pointer movement is dispatched before that fetch completes",
  "SURA_OS_BROWSER_NAV_FETCH_CANCEL_REQUESTED after F6 is dispatched during live response work",
  "SURA_OS_BROWSER_NAV_FETCH_CANCELLED_OK after the synchronous network stack unwinds and the Browser restores its old document",
);
machineFacts.freestanding.virtio_network_and_browser.executed_verification =
  machineFacts.freestanding.virtio_network_and_browser.executed_verification.map((item) =>
    item
      .replace(
        "SURA_OS_BROWSER_URL_OK after live example.org navigation",
        "SURA_OS_BROWSER_URL_OK after live explicit-http example.org navigation",
      )
      .replace(
        "SURA_OS_BROWSER_HTTPS_OK after live suralang.site HTTP-to-HTTPS redirect, TLS 1.3 handshake, certificate validation, and encrypted response",
        "SURA_OS_BROWSER_HTTPS_OK after live scheme-less suralang.site selects HTTPS, completes incremental TLS 1.3, validates its certificate, and receives an encrypted response",
      )
  );
machineFacts.freestanding.minimal_os_integration.hidden_diagnostic_commands.push("faultbrowser");
machineFacts.freestanding.virtio_network_and_browser.browser.push(
  "scheme-less hostnames select HTTPS by default; explicit http:// selects plain HTTP",
);
machineFacts.freestanding.minimal_os_integration.executed_path =
  machineFacts.freestanding.minimal_os_integration.executed_path.map((item) =>
    item
      .replace(
        "live non-default-host example.org HTTP navigation",
        "live non-default-host explicit-http example.org navigation",
      )
      .replace(
        "live suralang.site HTTP-to-HTTPS redirect, certificate validation, encrypted response, and render through SURA_OS_BROWSER_HTTPS_OK",
        "live scheme-less suralang.site HTTPS selection, incremental TLS handshake, certificate validation, encrypted response, and render through SURA_OS_BROWSER_HTTPS_OK",
      )
  );
machineFacts.freestanding.minimal_os_integration.limitations =
  machineFacts.freestanding.minimal_os_integration.limitations.map((item) =>
    item
      .replace(
        "no resize, minimize, maximize, user-facing background-job API, or user-space window server",
        "no user-facing background-job API, full-size Ring-3 app-surface migration, complete user-space compositor, animations, drag-and-drop, real multi-output presentation, or complete accessibility service",
      )
      .replace(
        "Browser logic, window rendering, hardware snapshot gathering",
        "Browser logic, built-in application content rendering, hardware snapshot gathering",
      )
      .replace(
        "uncached address-bar DNS is incremental and cancelable; the later synchronous TCP/TLS/decode/layout call stack",
        "uncached address-bar DNS, the first TCP connect, and the first TLS handshake are incremental and cancelable; the later synchronous response/decode/layout call stack",
      )
  );
machineFacts.freestanding.tls13_crypto.limitations =
  machineFacts.freestanding.tls13_crypto.limitations.map((item) =>
    item.replace(
      "four pinned built-in trust anchors",
      "six pinned built-in trust anchors",
    )
  );
machineFacts.freestanding.virtio_network_and_browser.limitations =
  machineFacts.freestanding.virtio_network_and_browser.limitations.map((item) =>
    item
      .replace(
        "the Ring-3 Browser worker validates request metadata and capabilities, and address-bar DNS plus the first TCP connect are incremental desktop-loop states; TLS, response decoding, DOM construction, layout, painting, and link/form navigation remain a synchronous kernel call stack",
        "the Ring-3 Browser worker validates request metadata and capabilities; address-bar DNS, the first TCP connect, and the first TLS handshake are incremental desktop-loop states; response decoding, later resource TLS connections, DOM construction, layout, painting, and link/form navigation remain a synchronous kernel call stack",
      )
      .replace(
        "three pinned trust anchors: ISRG Root X1, DigiCert Global Root G2, and GlobalSign Root CA - R3",
        "six pinned trust anchors: ISRG Root X1, DigiCert Global Root G2, GlobalSign Root CA - R3, Amazon Root CA 1, USERTrust RSA Certification Authority, and Microsoft RSA Root Certificate Authority 2017",
      )
      .replace(
        "four pinned trust anchors: ISRG Root X1, DigiCert Global Root G2, GlobalSign Root CA - R3, and Amazon Root CA 1",
        "six pinned trust anchors: ISRG Root X1, DigiCert Global Root G2, GlobalSign Root CA - R3, Amazon Root CA 1, USERTrust RSA Certification Authority, and Microsoft RSA Root Certificate Authority 2017",
      )
  );
machineFacts.freestanding.minimal_os_integration.limitations =
  machineFacts.freestanding.minimal_os_integration.limitations.map((item) =>
    item
      .replace(
        "three pinned trust anchors rather than a broad root store",
        "six pinned trust anchors rather than a broad root store",
      )
      .replace(
        "four pinned trust anchors rather than a broad root store",
        "six pinned trust anchors rather than a broad root store",
      )
  );
machineFacts.freestanding.not_implemented =
  machineFacts.freestanding.not_implemented.map((item) =>
    item
      .replace(
        "demand paging, copy-on-write, shared memory, and memory-mapped files",
        "demand paging, copy-on-write, general user-facing shared-memory APIs, and memory-mapped files",
      )
      .replace(
        "IRQ-driven input queues, scheduled-process input delivery, and complete resize/minimize/maximize/application window lifecycle",
        "IRQ-driven input queues, scheduled-process input delivery, full-size app-owned Ring-3 surfaces, complete user-space composition, window animations, drag-and-drop, real multi-output presentation, and complete accessibility service",
      )
  );
machineFacts.freestanding.verification.push(
  "stdlib/freestanding/font_ui.sura",
  "stdlib/freestanding/font_ui_atlas.sura",
  "stdlib/freestanding/power.sura",
  "stdlib/freestanding/window_server.sura",
  "stdlib/freestanding/ui.sura",
  "os/user_window_server.sura",
  "examples/os/window_server_features.sura",
  "tools/sura_window_server_qemu_gate.ps1",
  "tools/sura_ui_font_generate.ps1",
  "tools/sura_power_shutdown_qemu_gate.ps1",
  "tools/sura_power_reset_qemu_gate.ps1",
  "tools/sura_os_reboot_qemu_gate.ps1",
  "tools/sura_nvme_qemu_gate.ps1",
  "tools/sura_os_nvme_qemu_gate.ps1",
  "examples/os/power_shutdown_qemu_gate.sura",
  "examples/os/power_reset_qemu_gate.sura",
  "examples/os/nvme_qemu_gate.sura",
  "third_party/noto-cjk/OFL.txt",
  "third_party/noto-cjk/NOTICE.md",
);

const navItems = [
  ["overview", "문서 개요"],
  ["compatibility", "호환성과 지원 등급"],
  ["project", "범위와 기여"],
  ["install", "설치와 실행"],
  ["lexical", "값과 기본 문법"],
  ["types", "타입과 연산"],
  ["control", "제어 흐름"],
  ["functions", "함수와 스코프"],
  ["objects", "객체와 컬렉션"],
  ["errors", "오류와 진단"],
  ["cli", "실행기 CLI"],
  ["stdlib", "표준 라이브러리"],
  ["async", "동시성"],
  ["ai", "Tensor·AI·CUDA"],
  ["media", "영상→문자 프레임"],
  ["interop", "FFI·Plugin·Python"],
  ["packages", "패키지·개발 도구"],
  ["targets", "JS·WASM 타깃"],
  ["freestanding", "OS 개발 기능"],
  ["release", "빌드와 배포"],
  ["performance", "성능과 검증 기록"],
  ["machine", "구조화 데이터"],
];

const catalogRows = catalog.entries.map((entry) => [
  code(entry.name),
  escapeHtml(entry.path),
  modules.some((module) => module.name === entry.name) ? "API signature 제공" : "source module entry",
]);

const moduleCards = modules.map((module) => {
  const rows = module.symbols.map((symbol) => [
    code(symbol.name),
    code(symbol.signature),
  ]);
  return "<details class='module-card' id='module-" + escapeHtml(module.name) + "'>" +
    "<summary><span><code>" + escapeHtml(module.name) + "</code> — " +
    escapeHtml(moduleDescriptions[module.name] || "") + "</span><b>" +
    module.symbol_count + " signatures</b></summary>" +
    table(["이름", "호출 서명"], rows) + "</details>";
}).join("");

const sections = [];

sections.push(section("overview", "문서 개요",
  paragraph("이 문서는 " + code("Sura Language " + version) + "의 공개 레퍼런스입니다. 문법, 런타임 의미, 명령행, 표준 라이브러리 API, 동시성, AI/CUDA, 미디어, 외부 연동, 패키지 관리, 타깃 상태와 검증 기록을 한 파일에 담습니다.") +
  "<div class='status-grid'>" +
    "<div class='status-card status-editor'><strong>실행 파이프라인</strong><span>lexer → parser/AST → strict typecheck → register bytecode → register VM. Windows x64는 부분 JIT, Linux x86-64는 helper 없는 straight-line baseline을 사용합니다.</span></div>" +
    "<div class='status-card status-targets'><strong>API 인벤토리</strong><span>runtime module namespace 35개, catalog module 34개, module signature " + apiSymbolCount + "개, global builtin name·alias " + globalBuiltinNames.length + "개입니다.</span></div>" +
    "<div class='status-card status-release'><strong>검증 기준</strong><span>최종 " + version + " engine에서 stable suite는 VM " + escapeHtml(verificationManifest.results.stable_vm) + "와 JIT " + escapeHtml(verificationManifest.results.stable_jit) + "를 통과했습니다.</span></div>" +
  "</div>" +
  paragraph("Sura source 확장자는 " + code(".sura") + "입니다. 값은 NaN-boxed dynamic Value로 표현하고, register VM과 mark-sweep GC가 실행합니다. GC heap은 프로세스 전역입니다.") +
  paragraph("기본 진단 언어는 영어입니다. 한국어 진단은 " + code("--lang ko") + " 또는 " + code("SURA_LANG=ko") + "로 선택합니다.")
));

sections.push(section("compatibility", "호환성과 지원 등급",
  paragraph("공개 호환성 보장은 " + code(compatibilityContract.source.guarantee_starts_at) + "부터 적용합니다. 패치 릴리스의 공개 호환성 파괴는 금지하며, 마이너 릴리스에서 기존 기능을 없애거나 의미를 바꾸려면 최소 " + compatibilityContract.source.minor_release_deprecation_period + "번의 마이너 릴리스 동안 지원 중단 절차를 거칩니다. 1.10 검사는 보관된 실제 런타임과 현재 런타임의 역사 검증 자료이며 1.10 지원을 소급 보장하지 않습니다.") +
  table(["영역", "현재 계약"], [
    ["소스", "1.11 호환 fixture " + compatibilityContract.source.fixtures.length + "개를 strict check, VM, JIT 또는 fallback, bytecode load에서 검사"],
    ["역사 검증", compatibilityContract.source.historical_probes[0].runtime_version + " 공개 ZIP·엔진 해시를 확인하고 1.10 fixture " + compatibilityContract.source.historical_probes[0].fixtures.length + "개를 이전·현재 VM/JIT fallback에서 실행"],
    ["바이트코드", "형식 " + compatibilityContract.bytecode.current + " 생성, 형식 " + compatibilityContract.bytecode.accepted.join("·") + " 읽기"],
    ["보호 릴리스 패키지", "형식 " + compatibilityContract.release_package.current + " 생성, 형식 " + compatibilityContract.release_package.accepted.join("·") + " 읽기"],
    ["Plugin ABI", compatibilityContract.plugin_abi],
    ["Embedding FFI ABI", compatibilityContract.ffi_abi],
  ]) +
  "<h3>안정</h3>" + list(compatibilityContract.support_tiers.stable.map(escapeHtml)) +
  "<h3>플랫폼 한정</h3>" + list(compatibilityContract.support_tiers.platform_limited.map(escapeHtml)) +
  "<h3>실험</h3>" + list(compatibilityContract.support_tiers.experimental.map(escapeHtml)) +
  paragraph("플랫폼 한정 기능은 요구 조건이 맞지 않을 때 문서화된 VM fallback 또는 명시적인 오류를 사용합니다. 실험 기능은 마이너 버전에서 바뀔 수 있으며, 안정 등급으로 올리기 전에 지원 플랫폼, 실패 동작, 호환 fixture와 CI 검사를 추가합니다. 전체 정책과 검증 명령은 " + code("COMPATIBILITY.md") + "에 기록되어 있습니다.")
));

sections.push(section("project", "범위와 기여",
  paragraph("Sura의 핵심 범위는 native syntax, register bytecode와 VM, core language constructs, practical stdlib, package project workflow, C FFI·Plugin ABI와 실제 구현에서 생성되는 도구 문서입니다. 플랫폼·외부 프로그램 조건이 있는 JIT, CUDA, media, installer와 bridge는 조건부 범위이고 JavaScript·WebAssembly 변환, Transformer 확장과 외부 registry 운영은 실험 범위입니다.") +
  paragraph("새 public syntax, builtin, module, CLI option과 file format은 처음에는 실험으로 분류합니다. 지원 플랫폼, 실패·fallback test, 실제 문법 예제와 reference, 호환성 영향, 적용 가능한 CI 검사가 모두 있어야 안정 또는 플랫폼 한정 등급으로 올릴 수 있습니다.") +
  table(["문서", "용도"], [
    [code("SCOPE.md"), "핵심·조건부·실험 범위와 안정 승격 조건"],
    [code("CONTRIBUTING.md"), "실제 Sura 문법, build·test·package 기여 절차"],
    [code(".github/ISSUE_TEMPLATE/feature_request.yml"), "지원 등급, fallback, 호환성과 검증 계획을 요구하는 기능 제안"],
    [code(".github/ISSUE_TEMPLATE/package_submission.yml"), "test·quality·publish dry-run 증거를 요구하는 package 제안"],
  ])
));

sections.push(section("install", "설치와 실행",
  "<h3>Windows x64 공개 파일</h3>" +
  table(["파일", "용도"], [
    ["<a href='" + escapeHtml(releaseManifest.store.url) + "'>Microsoft Store</a>", "Microsoft 인증 기본 설치; 게시자 " + escapeHtml(releaseManifest.store.publisher) + ", 무료"],
    ["<a href='/downloads/SuraLanguageSetup-" + version + ".exe' download>SuraLanguageSetup-" + version + ".exe</a>", "단일 설치 파일"],
    ["<a href='/downloads/SuraLanguage-" + version + "-windows-x64.zip' download>SuraLanguage-" + version + "-windows-x64.zip</a>", "설치 kit와 payload 검증"],
    ["<a href='/downloads/SuraLanguage-VSCode-" + version + ".vsix' download>SuraLanguage-VSCode-" + version + ".vsix</a>", "VS Code 언어 확장"],
  ]) +
  paragraph("기본 설치 경로는 Microsoft Store입니다. 직접 다운로드 설치기는 Windows x64, .NET Framework CLR, Windows PowerShell 또는 PowerShell을 사용합니다. 설치된 runtime은 " + code("%LOCALAPPDATA%\\Programs\\Sura\\bin") + "에 위치하고 " + code("sura") + "와 " + code("surapkg") + " 명령을 만듭니다.") +
  paragraph("직접 다운로드 설치기의 Authenticode 상태는 " + code(releaseManifest.signing.authenticode) + "입니다. 설치 대상 실행 파일이 사용 중이면 1.11.1 설치기는 덮어쓰기 전에 중단하고 Sura REPL·터미널·VS Code를 닫으라는 안내와 " + code("%LOCALAPPDATA%\\Sura\\Logs") + "의 로그 경로를 표시합니다.") +
  pre("sura --version\nsurapkg examples\nsurapkg example algorithms/word_frequency my_example\nsurapkg new my_app\ncd my_app\nsurapkg run\nsurapkg test") +
  "<h3>소스 checkout 빌드</h3>" +
  pre(".\\build.bat portable\n.\\SuraLanguage.exe --version\n.\\SuraLanguage.exe .\\app.sura") +
  paragraph("실행 기능별 외부 요구사항: 영상 decoding은 FFmpeg를 사용합니다. native HTTP·async HTTP·HTTP 기반 LLM 요청은 curl을 실행합니다. http.serve_routes는 Node.js를 사용하고 http.serve_static은 Node.js를 먼저 사용한 뒤 Python을 사용할 수 있습니다. Python bridge는 Python, JS target과 VS Code 도구 검증은 Node.js, native FFI/plugin/embedding build는 C/C++ toolchain을 사용합니다.")
));

sections.push(section("lexical", "값과 기본 문법",
  paragraph("source는 UTF-8이며 UTF-8 BOM도 받습니다. 한 줄 주석은 " + code("#") + "와 " + code("//") + "를 사용합니다. 문자열은 큰따옴표로 작성하고 " + code("\\n \\r \\t \\\" \\\\") + " escape와 " + code("{expression}") + " interpolation을 지원합니다.") +
  pre('# comment\n// comment\nname is "Sura"\ncount: number is 3\nenabled is true\nmissing is nil\nitems is [1, 2, 3]\nprofile is {name: "Ada", level: 7}\nprint("hello {name}, next={count + 1}")') +
  table(["형태", "문법"], [
    ["선언·재대입", code("name is value")],
    ["타입 표기", code("name: type is value")],
    ["복합 대입", code("+=  -=  *=  /=  %=")],
    ["field/index 대입", code("object.field is value") + ", " + code("array[index] is value")],
    ["optional access", code("value?.field")],
    ["nil coalescing", code("left ?? right")],
    ["삼항식", code("condition ? then_value : else_value")],
    ["source import", code('import "./lib.sura"')],
    ["stdlib module", code("use json")],
  ]) +
  paragraph(code("let") + ", " + code("var") + ", " + code("const") + ", Python 들여쓰기 block, C/JavaScript 중괄호 statement block은 Sura parser 문법에 포함되지 않습니다.")
));

sections.push(section("types", "타입과 연산",
  paragraph("Sura " + version + "의 기본 검사는 strict-by-default 점진적 타입 검사입니다. type annotation이 알려진 지점은 검사하고 builtin·property·index·method 반환의 상당수는 " + code("any") + "로 추론합니다.") +
  table(["annotation", "의미"], [
    [code("number"), "IEEE-754 double"],
    [code("int / float / double"), code("number") + "와 같은 runtime 숫자 표현의 annotation alias"],
    [code("string"), "UTF-8 문자열"],
    [code("bool"), code("true") + ", " + code("false")],
    [code("nil"), "값 없음"],
    [code("array"), "순서 있는 Value 목록"],
    [code("dict"), "문자열 key 기반 Value map"],
    [code("any"), "모든 값과 호환"],
    ["class name", "해당 class instance와 상속 관계"],
  ]) +
  paragraph(code("숫자/문자열/불리언/논리/배열/사전/없음/아무") + " 한국어 alias도 지원합니다. " + code("type(value)") + "의 runtime 이름에는 " + code("number, string, bool, nil, array, dict, tensor, function, instance, object") + "가 포함됩니다. " + code("function/object/tensor") + "를 annotation에 쓰면 현재 parser는 class name으로 보존합니다.") +
  paragraph("모든 annotation은 " + code("nil") + "을 허용합니다. named function parameter·return과 class extends 관계는 검사합니다. class method annotation은 parser가 받으며 현재 typechecker가 강제하지 않습니다.") +
  paragraph("strict type checking이 기본입니다. type error는 본문 실행 전에 중단됩니다. " + code("--legacy-types") + "는 source 실행에서 warning 후 진행하며 " + code("--compile") + "과 " + code("--release") + "에는 적용되지 않습니다.") +
  "<h3>Truthiness</h3>" +
  table(["false", "true"], [
    [code("nil, false, 0"), "0이 아닌 number"],
    ["빈 string·array·dict", "비어 있지 않은 string·array·dict"],
    ["", "function·instance·tensor와 나머지 object value"],
  ]) +
  "<h3>연산자</h3>" +
  table(["그룹", "연산자와 의미"], [
    ["산술", code("+ - * / %") + "; string이 한쪽에 있으면 " + code("+") + "가 다른 값을 문자열로 변환; array + array는 새 배열로 연결"],
    ["비교", code("== != < <= > >=") + "; ordering 네 연산은 number operand만 허용"],
    ["논리", code("and or not") + "; runtime truthiness 사용"],
    ["포함", code("value in collection")],
    ["비트", code("& | ^ ~ << >>")],
    ["선택", code("condition ? a : b") + ", " + code("left ?? right")],
  ]) +
  "<h3>우선순위</h3>" +
  paragraph("아래 표는 낮은 우선순위에서 높은 우선순위 순서입니다. 비교 연산은 괄호 없는 한 식에서 하나만 파싱합니다.") +
  table(["순서", "연산자", "결합"], [
    ["1", code("??"), "오른쪽; left가 nil일 때만 right 평가"],
    ["2", code("? :"), "오른쪽; 선택된 branch만 평가"],
    ["3", code("or"), "왼쪽; short-circuit"],
    ["4", code("and"), "왼쪽; short-circuit"],
    ["5", code("not"), "prefix"],
    ["6", code("== != < <= > >= in"), "비교 하나"],
    ["7–9", code("|  ^  &"), "각 단계 왼쪽"],
    ["10", code("<< >>"), "왼쪽"],
    ["11", code("+ -"), "왼쪽"],
    ["12", code("* / %"), "왼쪽"],
    ["13", code("unary -  ~"), "prefix"],
    ["14", code("call  index  .  ?."), "postfix"],
  ]) +
  "<h3>연산자 우선순위: 낮음 → 높음</h3>" +
  table(["단계", "연산자", "결합"], [
    ["1", code("??"), "오른쪽"],
    ["2", code("? :"), "오른쪽"],
    ["3–4", code("or") + ", " + code("and"), "왼쪽; short-circuit"],
    ["5", code("not"), "prefix"],
    ["6", code("== != < <= > >= in"), "괄호 없는 식에서 비교 1회"],
    ["7–9", code("| ^ &"), "각 단계 왼쪽"],
    ["10", code("<< >>"), "왼쪽"],
    ["11", code("+ -"), "왼쪽"],
    ["12", code("* / %"), "왼쪽"],
    ["13", code("unary - ~"), "prefix"],
    ["14", code("call index . ?."), "postfix"],
  ]) +
  paragraph(code("and/or") + "는 short-circuit하고 선택된 operand 값을 반환합니다. 현재 typechecker의 결과 추론은 bool입니다. array + array는 VM, strict checker, JS target에서 연결 연산으로 처리합니다. WASM target은 정적 배열끼리, 같은 ABI의 동적 배열끼리, 원소 metadata가 증명된 raw 배열과 tagged 동적 배열의 혼합을 연결합니다. WASM의 직접 원소 타입 증거는 배열 연산과 조건식의 AST를 최대 8단계까지만 따라가며, 함수 본문 전체를 추론하지 않습니다. 이 범위를 넘어 원소 metadata가 없는 raw Value와 tagged 동적 배열의 혼합은 아직 nil을 반환합니다.") +
  paragraph("비트 operand는 유한 정수이고 절댓값이 " + code("2^53 - 1") + " 이하여야 합니다. shift count는 0..63입니다. left shift 결과는 safe integer 범위 안에 있어야 하며 right shift는 arithmetic shift로 정의됩니다.")
));

sections.push(section("control", "제어 흐름",
  "<div class='grid-2'><div class='doc-panel'><h3>조건</h3>" +
  pre('if score >= 90 then\n  print("A")\nelif score >= 80 then\n  print("B")\nelse\n  print("C")\nend\n\nif score > 0 then print("positive") else print("zero")') +
  "</div><div class='doc-panel'><h3>반복</h3>" +
  pre("while ready do\n  break\nend\n\nrepeat 3 do\n  continue\nend\n\nfor n in 1 to 5 step 2 do\n  print(n)\nend") +
  "</div></div>" +
  paragraph("range " + code("for") + "는 정방향·역방향 step과 " + code("1 ~ 3") + " 형태를 지원합니다. array, dict, string 순회는 " + code("for value in collection") + "과 " + code("for key, value in collection") + " 형태를 사용합니다.") +
  "<h3>match와 when</h3>" +
  pre('match status\n  when "ready" then\n    print("go")\n  when _ then\n    print("wait")\nend\n\nwhen score do\nin 1 ~ 100 then\n  print("range")\nelse then\n  print("outside")\nend')
));

sections.push(section("functions", "함수와 스코프",
  pre("func add(value: number, delta: number is 1) -> number do\n  return value + delta\nend\n\ndouble is func(value) do\n  return value * 2\nend\n\nscale is |value| value * 4\nready is || true") +
  paragraph("함수 선언, anonymous block function, expression lambda를 함수 값으로 저장할 수 있습니다. 함수는 lexical closure를 만들며 바깥 local을 캡처합니다.") +
  pre("func make_counter() do\n  count is 0\n  func next() do\n    count += 1\n    return count\n  end\n  return next\nend") +
  paragraph("함수 안에서 top-level variable을 명시적으로 갱신할 때 " + code("global name") + "을 선언합니다. parameter default는 " + code("name is value") + " 또는 " + code("name: type is value") + "로 작성합니다.") +
  paragraph("기본값 표현식은 positional argument가 생략된 호출에서만 왼쪽부터 평가합니다. 명시적으로 전달한 " + code("nil") + "은 유지됩니다. 뒤쪽 기본값은 앞쪽 parameter를 참조할 수 있고 method default는 " + code("self") + "를 참조할 수 있습니다. named function, anonymous function, lambda와 method가 같은 규칙을 사용합니다.")
));

sections.push(section("objects", "객체와 컬렉션",
  "<h3>class와 상속</h3>" +
  pre('class Animal do\n  aliases is []\n  func init(name) do\n    self.name is name\n  end\n  func speak() do\n    return "..."\n  end\nend\n\nclass Dog extends Animal do\n  func init(name) do\n    super.init(name)\n  end\n  func speak() do\n    return self.name + ": bark"\n  end\nend\n\ndog is new Dog("Badu")') +
  paragraph("class field 기본값은 새 instance마다 부모 class에서 자식 class 순서, 각 class의 선언 순서로 평가합니다. array와 dict 결과도 instance별 값이므로 한 instance의 field 변경이 다른 instance의 기본 field를 변경하지 않습니다. 자동 struct constructor의 field 기본값은 해당 argument를 생략한 생성에서 평가하고, 명시한 argument의 기본식은 실행하지 않습니다.") +
  "<h3>struct와 enum</h3>" +
  pre("struct Vec2 do\n  x\n  y\n  func length2() do\n    return self.x * self.x + self.y * self.y\n  end\nend\n\nenum Code do\n  OK is 200\n  FAIL is 500\nend") +
  paragraph("array는 " + code("items[index]") + ", dict는 " + code("value.key") + "와 " + code('value["key"]') + "로 접근합니다. 문자열은 " + code("trim/lower/upper/split/contains/starts_with/ends_with/repeat/pad_left/pad_right") + " 등의 method를 제공합니다. array는 " + code("len/push/insert/remove/join/contains") + " 등의 method를 제공합니다.")
  + paragraph("array는 음수 index를 지원하고 범위를 벗어나면 오류를 냅니다. dict의 없는 key는 nil입니다. string index의 단위는 UTF-8 byte이며 범위를 벗어나면 nil입니다. array·dict·function·instance·tensor equality는 identity 비교입니다.")
));

sections.push(section("errors", "오류와 진단",
  pre('try\n  throw "failed"\ncatch error\n  print(error)\nfinally do\n  print("cleanup")\nend') +
  paragraph("예외 block은 " + code("try") + ", " + code("catch name") + ", 선택형 " + code("finally do") + ", " + code("end") + " 순서입니다.") +
  table(["코드", "범위"], [
    [code("E100 / E101"), "정의되지 않은 변수·함수"],
    [code("E200 / E201"), "대입·operand type, numeric ordering"],
    [code("E202"), "0 나눗셈·나머지, 범위 초과 등 runtime value 오류"],
    [code("E203 / E204"), "인자 개수·인자 type과 safe integer/shift 계약"],
    [code("E205 / E206"), "return type·default parameter type"],
    [code("E207 / E208"), "반복 범위·iterable type"],
    [code("E209 / E210"), "복합 대입·match pattern type"],
    [code("E300"), "runtime 호출 인자 개수"],
    [code("E500"), "runtime 내부 한도·불가능한 opcode"],
  ]) +
  paragraph(code("sura --check path") + "는 실행 없이 parse와 typecheck를 수행합니다. parser diagnostics는 줄 경계에서 복구해 한 파일의 후속 syntax error도 보고합니다.")
));

sections.push(section("cli", "실행기 CLI",
  paragraph("아래 목록은 " + code("SuraLanguage.exe --help") + "의 " + version + " 출력입니다.") +
  pre(runtimeHelp)
));

sections.push(section("stdlib", "표준 라이브러리",
  paragraph("VM의 runtime module namespace는 35개입니다. " + code("surapkg docs") + "와 " + code("surapkg info") + "가 signature metadata를 제공하는 module은 34개·" + apiSymbolCount + " signature이며, " + code("console") + "이 별도 runtime module입니다. global builtin registry에는 case-sensitive 직접 호출 이름과 alias " + globalBuiltinNames.length + "개가 있습니다.") +
  paragraph(code("surapkg list --json") + "의 package inventory는 39 entry입니다. " + code("data/time/web") + "은 각각 " + code("json/datetime/http") + " alias로 canonicalize됩니다. " + code("game/system") + " source file은 package inventory에 나타나며 VM의 " + code("use") + " module namespace에는 포함되지 않습니다.") +
  table(["이름", "runtime/source 경로", "공개 metadata"], catalogRows) +
  "<h3>34개 내장 module API</h3>" + moduleCards +
  "<h3>console runtime module</h3>" +
  paragraph(code("use console") + "은 runtime module object를 만듭니다. 이 module은 현재 surapkg 34-module API metadata 집계에 포함되지 않습니다.") +
  paragraph(consoleMethods.map((name) => code("console." + name)).join(" ")) +
  "<h3>" + globalBuiltinNames.length + "개 global builtin 이름과 alias</h3>" +
  "<details class='machine-preview'><summary>global builtin 목록 보기</summary>" +
  paragraph(globalBuiltinNames.map((name) => code(name)).join(" ")) + "</details>"
));

sections.push(section("async", "동시성",
  paragraph(code("async") + " module은 thread-per-task 대신 bounded worker pool을 사용합니다. 기본 worker 수는 감지한 hardware concurrency를 1..8로 제한하고 감지 실패 시 4를 사용합니다. pending queue 상한 기본값은 1024입니다. " + code("async.limits()") + "가 현재 값을 반환하고 live task와 scope가 없을 때 " + code("async.configure(max_workers, max_queue)") + "로 교체합니다. 설정 범위는 worker 1..256, queue 1..1,000,000입니다.") +
  table(["항목", "계약"], [
    ["task 종류", code("async.cmd") + ", " + code("async.http_get/http_request") + ", " + code("async.sleep") + ", " + code("async.sura")],
    ["상태", code("queued, running, succeeded, failed, cancelled")],
    ["조회", code("status, ready, pending, cancelled")],
    ["대기", code("await, await_timeout, any, all, all_timeout")],
    ["수명", code("forget, cleanup")],
    ["취소", code("cancel") + "; timer checkpoint, command process tree, curl request 종료"],
    ["scope", code("scope, scope_attach, scope_cancel, scope_status, scope_join, scope_close")],
  ]) +
  paragraph(code("scope_cancel") + "은 runtime mutex 아래에서 새 child admission을 닫은 뒤 기존 child snapshot에 cancellation을 요청합니다. " + code("scope_join") + "은 기존 child를 기다리고 " + code("scope_close") + "는 cancellation을 먼저 요청합니다. join/close는 child handle을 정리한 뒤 보존된 child failure를 전파합니다. timed join/close가 끝나지 않으면 " + code("closed: false") + "를 반환하고 해당 scope는 새 child를 받지 않습니다.") +
  paragraph("완료 결과와 오류는 await·forget·cleanup 또는 scope 수집까지 보관됩니다. await는 terminal handle을 소비합니다. status의 " + code("running") + " field는 queued task에도 true이므로 세부 상태는 " + code("state") + "와 " + code("queued") + " field로 구분합니다.") +
  paragraph(code("async.sura({program, input, timeout_ms?}, [scope_id])") + "는 .sura, .sura.bc/.bc, .sura.srp/.srp program을 격리된 자식 Sura process에서 실행합니다. 제출 시 program과 input을 snapshot하며 program·input 각각 64 MiB, JSON input 1,000,000 node·depth 128을 상한으로 둡니다. input은 nil·bool·finite number·string·array·dict만 허용하며 closure·instance·tensor·native resource·cycle을 거부하고 alias identity는 보존하지 않습니다. 기본 timeout은 30,000 ms, 범위는 1..3,600,000 ms입니다. 자식의 " + code("argv()[0]") + "이 input JSON을 받고 stdout이 await 결과가 됩니다. nonzero exit는 task failure이며 timeout·cancel은 process tree를 종료합니다. child process 안의 재귀 async.sura는 비활성화됩니다.") +
  paragraph("임의 Sura closure를 같은 process worker에 spawn하는 API는 없습니다. any/all_timeout은 completion condition variable을 기다립니다. queued cancellation은 FIFO queue에서 즉시 제거됩니다.") +
  paragraph("file:// async read는 일반 파일만 허용하고 64 KiB chunk마다 cancellation을 확인하며 task 결과를 64 MiB로 제한합니다. command와 curl capture도 64 MiB를 넘으면 process tree를 종료하고 task를 failed 상태로 게시합니다. terminal task output의 runtime 전체 retained budget은 256 MiB이고 retained error는 task당 64 KiB입니다. " + code("async.limits()") + "가 max_retained_bytes와 retained_bytes를 반환하며 await·forget·cleanup·scope close가 예산을 반환합니다. HTTP body 임시 파일은 process id와 단조 sequence를 사용해 exclusive-create하며 정상·예외·취소 경로에서 삭제합니다.") +
  pre("use async\nscope is async.scope()\nfirst is async.sleep(50, scope)\nsecond is async.cmd(\"echo sura\", scope)\nresult is async.scope_join(scope, 1000)\nprint(result)")
));

sections.push(section("ai", "Tensor·AI·CUDA",
  table(["영역", "구현 범위"], [
    [code("nn"), "CPU dense MLP, forward/predict/train/evaluate, Adam·momentum SGD, JSON save/load"],
    [code("autograd CPU"), "row-major typed Tensor, CPU 기본 float64, reverse-mode graph, optimizer와 checkpoint"],
    [code("autograd CUDA"), "CUDA 기본 float32 resident Tensor, f16/bf16 2-byte visible storage, typed matmul, f32 output·gradient·master optimizer"],
    ["Transformer 구성", "reshape, ND-left×rank-2 matmul, transpose, GELU, LayerNorm, embedding, causal_attention, cross_entropy_ids"],
    ["optimizer", "SGD·Adam, weight decay, momentum, transactional finite check, loss scaling"],
    ["데이터", "UTF-8 raw-byte tokenizer, bounded byte-level BPE, byte-token uint32 shard, seek 기반 batch loader"],
    ["model 교환", "Safetensors read/write, PyTorch bridge through Safetensors, ONNX initializer read/write and bounded CPU graph execution"],
    ["checkpoint", "v3 visible weight, f32 master, SGD velocity, Adam moments/step/beta product"],
  ]) +
  paragraph("resident CUDA graph의 GELU, LayerNorm, embedding, transpose, attention, sparse cross entropy와 output/gradient는 float32입니다. f16/bf16 storage를 직접 소비하는 low-precision 연산은 typed matmul입니다.") +
  paragraph("현재 CUDA 범위: single-device resident graph, optional dynamically loaded cuBLAS SGEMM/GemmEx, reference PTX fallback, deterministic no-O(T²)-workspace causal-attention backward. tokenizer.train_bpe는 bounded deterministic byte-level BPE 학습·왕복·저장과 chunk-boundary 보존 dataset pack을 제공한다. autograd.run_onnx는 CPU에서 IR 3~10·opset 7~18의 15개 operator subset만 실행하며 전체 축 순열 Transpose, Flatten, bounded INT64-shape Reshape를 포함한다. 현재 미구현 범위: general broadcasting, batched-right matmul, dense softmax/cross-entropy, low activation/output, mixed backward 가속, FlashAttention, KV cache, AdamW, NCCL, 임의 ONNX graph·Conv·동적 shape/control-flow·GPU 실행, 외부 tokenizer 호환, mmap/prefetch.") +
  paragraph("기본 한도는 Tensor rank 8, Tensor당 10,000,000 elements, graph 1,000,000 nodes, live host buffer 512 MiB, causal-attention score 50,000,000개입니다.") +
  pre("use autograd\nx is autograd.tensor([[1, 2]], {device: \"cuda\"})\nw is autograd.parameter([[0.1], [0.2]], {device: \"cuda\"})\nautograd.zero_grad([w])\ny is autograd.linear(x, w)\nloss is autograd.mean(y)\nautograd.backward(loss)\nautograd.adam([w], 0.001)")
));

sections.push(section("media", "영상 → 문자 프레임",
  paragraph(code("media.frame_to_text") + "는 grayscale/RGB/RGBA pixel matrix를 FFmpeg 없이 변환합니다. " + code("media.ascii_frames") + ", " + code("media.video_to_text") + ", " + code("media.video_text_frames") + "는 로컬 영상 decoding에 FFmpeg를 사용합니다.") +
  pre('use media\noptions is {width: 80, fps: 8, max_frames: 300, charset: " .:-=+*#%@", dither: true}\nclip is media.ascii_frames("clip.mp4", options)\nprint(clip.frames[0])') +
  table(["옵션", "기본값·범위"], [
    [code("width"), "80, 1..512"],
    [code("height"), "자동; 지정값 1..512"],
    [code("char_aspect"), "0.5, 0.1..2.0; 자동 높이에 사용"],
    [code("fps"), "8, 0.1..60"],
    [code("max_frames"), "300, 1..10000"],
    [code("start / duration"), "초 단위 decode 구간"],
    [code("charset"), code('" .:-=+*#%@"') + "; 어두움→밝음 순 UTF-8 glyph"],
    [code("gamma"), "1, 0.1..5.0"],
    [code("invert / dither"), "false / false"],
    [code("timeout_ms"), "120000, 1000..600000"],
  ]) +
  paragraph("결과 schema는 " + code("sura.text-video.v1") + "이며 " + code("frames, timestamps, width, height, fps, frame_count, sampled_duration, truncated, charset, gamma, inverted, dithered, backend, decoder, source") + " field를 포함합니다.") +
  paragraph("hard cap은 frame당 4 Mi pixels, 최종 output 64 MiB, decoded stream 512 MiB, path 8192 bytes입니다. charset은 2..256개 유효 UTF-8 glyph입니다. start 범위는 0..86400초, duration은 0.001..86400초입니다.") +
  paragraph("FFmpeg 탐색 순서는 option " + code("ffmpeg") + ", 환경 변수 " + code("SURA_FFMPEG") + ", " + code("PATH") + "입니다. 입력은 로컬 일반 파일이며 network URL은 거부됩니다. decoder process는 shell을 거치지 않고 argument array로 실행되고 protocol whitelist는 file과 pipe입니다. video API는 native runtime 경로입니다.") +
  pre('$env:SURA_FFMPEG = "C:\\ffmpeg\\bin\\ffmpeg.exe"\nsura video_text.sura\n\n# 또는 Sura options에 ffmpeg 경로 전달\nclip is media.ascii_frames("clip.mp4", {ffmpeg: "C:\\\\ffmpeg\\\\bin\\\\ffmpeg.exe"})')
));

sections.push(section("interop", "FFI·Plugin·Python",
  table(["경계", "계약"], [
    ["C embedding FFI", "ABI 1.2.0; 검증된 opaque token handle, set globals, run source, read globals, last_error"],
    ["FFI result", code("SURA_OK, SURA_ERR_LEX, SURA_ERR_PARSE, SURA_ERR_RUNTIME, SURA_ERR_TYPE, SURA_ERR_BUSY, SURA_ERR_INTERNAL")],
    ["FFI concurrency", "호출 lease와 지연 close; 같은 handle을 다른 host thread가 실행 중 재진입하면 BUSY; process-global heap 때문에 VM call은 전역 lock으로 직렬화"],
    ["dynamic C call", code("ffi.load(path)") + ", " + code("ffi.call(lib, symbol, signature, ...args)")],
    ["runtime FFI call", "library를 호출마다 open/symbol lookup/close; 최대 4 argument; integer-like 또는 float/double 또는 char pointer shape"],
    ["Plugin", "ABI 1.1.0; nil/number/bool/string value, host log/alloc/free/cancel, lifecycle, export table, manifest allowlist·SHA-256·quota"],
    ["Python", code("python.available/executable/eval/call/call_json") + "; 설치된 Python interpreter를 선택적으로 사용"],
  ]) +
  paragraph("embedding FFI의 globals 갱신은 새 map과 persistent GC root를 모두 만든 뒤 swap합니다. exported call은 C header에서 사용할 수 있고 native exception을 C ABI 밖으로 내보내지 않습니다. string/error getter는 host thread별 OS TLS/FLS buffer를 사용하며 같은 thread의 다음 string/error getter까지 유효합니다.") +
  paragraph("같은 handle의 다음 " + code("sura_run") + "까지 보존되는 값은 nil·bool·number·string·tensor와 이 값들만 도달 가능한 array/dict입니다. function/closure·upvalue·class instance 또는 그 값에 도달하는 array/dict는 한 execution image에 종속됩니다. 성공한 실행에서는 해당 top-level global을 보존 목록에서 제외합니다. 실패한 실행이 기존 persistent container에 run-bound 값을 넣었다면 그 handle의 persistent globals를 비웁니다. graph 검사는 cycle-safe worklist를 사용하고 1,000,000 object에서 제한합니다.") +
  paragraph("runtime FFI는 string argument와 floating-point argument를 한 호출에 혼합하지 않습니다. C++ object ABI, struct by-value, callback은 일반 지원 범위에 포함되지 않습니다.") +
  paragraph("plugin timeout cancellation은 plugin이 host의 " + code("should_cancel") + "을 확인하는 cooperative 방식입니다. memory quota는 host allocator를 통한 allocation을 집계합니다.") +
  paragraph(code("surapkg bind-c") + "는 단순 C ABI header에서 0..4개 integer 또는 char pointer argument wrapper와 지원되는 numeric/string/void return wrapper를 생성합니다.")
));

sections.push(section("packages", "패키지·개발 도구",
  paragraph("package manifest는 " + code("sura.pkg.json") + ", lockfile은 " + code("sura.lock.json") + ", 설치 directory는 " + code("packages/<name>") + "입니다. 기본 local registry는 " + code("./registry") + "이고 " + code("SURA_REGISTRY") + " 또는 " + code("SURA_REGISTRY_URL") + "로 변경합니다.") +
  paragraph("dependency spec은 exact version과 " + code("^1.2") + ", " + code("~1.2") + ", " + code(">=1.0 <2.0") + " 형식을 지원합니다. resolve는 yanked version을 제외합니다.") +
  table(["도구", "기능"], [
    ["VS Code extension", "설치된 Sura engine을 " + code("--lsp") + "로 시작해 diagnostics, completion, hover, definition/reference, rename, format, semantic tokens, code action을 사용하며 서버를 시작할 수 없으면 기본 completion/hover/signature provider로 전환"],
    [code("sura --lsp"), "incremental LSP, completion, hover, signature, symbols, diagnostics, definition/reference, format, rename, code action"],
    [code("--debug-protocol"), "VS Code DAP line breakpoint, step, stack, locals, watch, exception stop"],
    [code("--format / --format-check"), "source formatting"],
    [code("--lint"), "static lint와 risky API 진단"],
    [code("--profile / --profile-json"), "call·arithmetic·branch·JIT feedback"],
    [code("--gc-stats / --gc-stats-json"), "최종 full collection을 포함한 collection 수, 회수 object 수, total/max/average pause, 다음 object threshold"],
    [code("--test / --test-report"), "test discovery와 JSON report"],
    [code("surapkg docs"), "HTML, api.json, search-index.json 생성"],
    [code("surapkg ci / release"), "docs, tests, benchmark gate, audit, signature/quality workflow"],
    [code("version.json"), "runtime·웹·VS Code·설치기·MSIX·레퍼런스의 단일 버전 원본"],
    [code("sura_version_sync.ps1"), "버전, 공개 manifest, 웹 메타데이터와 실제 다운로드 파일 크기·SHA-256 일치 검사"],
    [code("sura_windows_signature_gate.ps1"), "직접 배포 실행 파일의 Authenticode 상태·서명자·타임스탬프 기록; 엄격 모드는 unsigned 파일 거부"],
  ]) +
  "<h3>surapkg CLI 전체 목록</h3>" + pre(packageHelp)
));

sections.push(section("targets", "JavaScript·WebAssembly 타깃",
  table(["타깃", "2026-07-17 AST lowering 분류"], [
    ["Native VM/JIT", "register VM이 주 실행 경로; Win64 x64 partial JIT, Linux x86-64 System V baseline, little-endian Windows/Linux/macOS ARM64 AAPCS64 baseline"],
    ["JavaScript", targetAudit.ast_node_count + "개 node 중 full " + targetAudit.js.full + ", ignored " + targetAudit.js.ignored + ", partial " + targetAudit.js.partial],
    ["WebAssembly", targetAudit.ast_node_count + "개 node 중 full " + targetAudit.wasm.full + ", partial " + targetAudit.wasm.partial + ", ignored " + targetAudit.wasm.ignored],
  ]) +
  paragraph("AST JSON은 type annotation의 원래 표기를 source_name에 보존합니다. WASM에서 매개변수와 반환 타입을 모두 float 또는 double로 명시한 최상위 함수는 이 표기를 기준으로 기존 Sura Value ABI export와 함께 함수명__f64 동반 export를 생성합니다. 동반 export는 WebAssembly 호스트가 f64 매개변수와 f64 결과를 직접 사용하며, 원래 함수 이름의 export 계약은 바꾸지 않습니다.") +
  paragraph("JavaScript의 full 41·ignored 2는 43개 AST node의 lowering 분류입니다. JavaScript의 Python·FFI·plugin·async.cmd는 stub 또는 미지원 경로이며, autograd·media·nn·dataset·tokenizer 등 일부 module은 JavaScript runtime object 목록에 없습니다.") +
  paragraph("WASM target은 AST JSON frontend와 WAT output을 사용합니다. numeric subset, 일부 tagged Value, array/dict/function/class/exception hint lowering을 포함하며 general dynamic Value/function/class/exception semantics " + targetAudit.wasm.partial + "개 node가 partial 상태입니다. 전체 audit status는 " + code(targetAudit.status) + "입니다.")
));

sections.push(section("freestanding", "OS 개발용 freestanding 기능",
  paragraph(code("uefi-x86_64") + "는 Sura VM, GC, Windows API, C runtime, 외부 assembler·linker 없이 PE32+ EFI application을 직접 만드는 실험 타깃입니다. " + code("os/sura_os.sura") + "는 QEMU/OVMF에서 UEFI 진입, GOP framebuffer와 backbuffer 준비, COM1 초기화, ExitBootServices, 픽셀·도형·아이콘·5x7 고정폭 글꼴·소문자 ASCII와 현대 한글용 2-bit 안티앨리어싱 글꼴 렌더링, double-buffer present, physical-page self-check, polling xHCI USB boot keyboard·mouse와 PS/2 fallback, 46x14 graphical terminal, ICH9 AHCI 데이터 디스크, VirtIO-net, Intel HDA 48 kHz signed-16 stereo DMA 출력을 실행합니다. FAT32는 설정·창 상태 호환 영역으로 유지하고, 그래픽 File Explorer와 Text Editor의 문서는 두 번째 SuraFS 파티션에 저장합니다. 한글 폴더·파일 열기, 편집 저장, 재부팅 후 동일 세대·내용 복원, DHCP·ARP·IPv4·UDP·DNS·TCP·HTTP, 입력 가능한 주소창, 유효한 UTF-8 페이지 텍스트, 제한된 HTML/CSS 그래픽 레이아웃까지 QEMU에서 검증됐지만 완성 OS는 아닙니다.") +
  paragraph("전원 경로는 부팅 중 FADT와 DSDT를 검증해 정적 _S5 패키지, PM1 제어 레지스터, RESET_REG/RESET_VALUE를 기록합니다. shutdown은 ACPI S5, reboot는 FADT reset을 사용하며 두 경로 모두 QEMU에서 실제 exit 0으로 검증됩니다. 일반 AML 실행, suspend, 배터리·열 관리와 물리 하드웨어 검증은 아직 없습니다.") +
  pre(".\\SuraLanguage.exe --target uefi-x86_64 --out FEATURES.EFI examples\\os\\freestanding_features.sura") +
  table(["영역", "현재 구현"], [
    ["정수·포인터", code("i8/u8/i16/u16/i32/u32/i64/u64/isize/usize/ptr") + ", " + code("ptr[StructName]")],
    ["정적 데이터", code("static.zero/bytes/u8/u16/u32/u64/utf8/utf16/struct")],
    ["모듈", "importing-file 기준 nested relative import, normalized-path 중복 제거, cycle 검사"],
    ["메모리 레이아웃", "typed struct field, natural 또는 packed layout, " + code("sizeof/alignof/offset_of")],
    ["포인터", code("ptr.add/index/field/align_up/align_down/is_aligned") + "와 width-correct field load/store"],
    ["저수준 CPU", "raw memory, port I/O, CR0/2/3/4, MSR, GDT, IDT, INVLPG, CPUID, RDTSC/RDTSCP, XGETBV/XSETBV"],
    ["원자 연산", "8/16/32/64-bit load, store, exchange, compare-exchange, fetch-add/sub, fence"],
    ["인터럽트 ABI", code("interrupt/interrupt_error") + " 함수, general-purpose register frame, error-code normalization, saved-CS conditional SWAPGS/LFENCE, checked IDT gate, " + code("iretq")],
    ["CPU별 상태", "TSS RSP/IST, checked TSS descriptor, LGDT/LIDT, segment reload, LTR/STR, FXSAVE/XSAVE, XSETBV, SWAPGS"],
    ["SMP primitive", "GS-relative per-CPU storage, runtime xAPIC/x2APIC access, EOI, ICR, IPI, INIT, SIPI"],
    ["페이징 primitive", "4-level page-table index, entry read/write/map/clear, CR3 root activation, INVLPG, local TLB flush"],
    ["메모리 라이브러리", "UEFI memory-map bitmap physical allocator, aligned contiguous allocation, conflict-checked 4-level walk/map/unmap/protect/translate"],
    ["Context primitive", "72-byte aligned task frame, nonvolatile GP register/RSP switch, entry(argument), exit_handler(result)"],
    ["스케줄러 라이브러리", "single-CPU cooperative round-robin, create/yield/sleep/block/wake/join/reap"],
    ["선점형 스케줄러·타이머", "checked 152-byte ring-0 interrupt frames, CLI/IRETQ resume, APIC periodic/one-shot timer, bounded PIT calibration, TSC deadline"],
    ["소프트웨어 인터럽트 syscall", "indirect Win64 calls, INT vector invocation, fixed handler table and five-argument dispatch"],
    ["Ring 3·빠른 syscall", "checked IRETQ user entry/resume, 168-byte user frame, SYSCALL/SYSRETQ, STAR/LSTAR/FMASK, GS-relative kernel stack, validated return address and sanitized RFLAGS"],
    ["사용자 프로세스", "fixed-capacity single-CPU round-robin, per-process CR3/TSS RSP0/user GS, timer/yield, blocking IPC wait, Ring-0 service slice, exit, page-fault termination, reap; QEMU에서 선점·CR3·IPC·fault 격리, 무한 루프 중 입력 루프 복귀, 다섯 실제 데스크톱 작업자의 지속 프로세스 실행 검증"],
    ["프로세스 IPC syscall", code("getpid/send/receive/yield/exit/wait") + ", checked user copy, authenticated sender/target, blocking event wait; wait/wake/receive/exit QEMU 실행 검증"],
    ["TLS 1.3·X.509 기반", code("sha256.sura") + "·" + code("sha384.sura") + "·" + code("hkdf_sha256.sura") + "·" + code("aes128_gcm.sura") + "·" + code("x25519.sura") + "·" + code("tls13_record.sura") + "·" + code("tls13_handshake.sura") + "·" + code("tls13_messages.sura") + "·" + code("tls13_verify.sura") + "·" + code("rsa_sha256.sura") + "·" + code("rsa_sha384.sura") + "·" + code("der.sura") + "·" + code("x509_chain.sura") + "; ExitBootServices 뒤 RFC/NIST·OpenSSL 및 고정된 공식 루트 벡터로 키 스케줄, record, RDRAND, RSA-PSS/PKCS#1 SHA-256·SHA-384, DER, 인증서 체인·호스트명·시간 검증을 QEMU 실행 검증"],
    ["PCI 기반 라이브러리", "legacy 0xCF8/0xCFC config access, BDF search, capability traversal, BAR decode, command flags"],
    ["블록 장치 기반", "checked synchronous block ABI, RAM disk, boot-stage UEFI Block I/O adapter and media snapshot"],
    ["ACPI MADT", "checked RSDP/XSDT/RSDT discovery, processor and I/O APIC records, interrupt overrides"],
    ["ACPI 전원", code("power.sura") + "의 checksum-valid FADT/DSDT 탐색, 정적 _S5 package, PM1 SLP_TYP/SLP_EN 종료와 RESET_REG 재시작. 직접 gate와 전체 OS Ring-3 reboot gate가 QEMU exit 0을 실행 검증"],
    ["NVMe 저장장치", code("nvme.sura") + "의 admin/I/O queue, controller·namespace Identify, PRP1/PRP2 read/write/flush, staged BlockDevice I/O. 직접 gate가 8192-byte 장치·호스트 readback을 확인하고 전체 OS가 NVMe FAT32+SuraFS mount를 실행 검증"],
    ["AP 시작", "16-bit real-mode to 64-bit trampoline, bounded INIT/SIPI sequence, atomic ready handshake"],
    ["직렬/VM 부팅 게이트", "16550 bounded polling, post-ExitBootServices COM1 marker, QEMU/OVMF gate and compile-only mode"],
    ["그래픽 OS 통합", code("os/sura_os.sura") + "와 " + code("tools/sura_os_vm.ps1") + "; QEMU TCG에서 double-buffer desktop·AHCI/FAT32 설정 호환·SuraFS 문서 영속 저장·VirtIO TCP/IP/HTTP와 제한된 TLS 1.3 HTTPS·memory self-check·COM1 shell 검증"],
    ["Framebuffer·글꼴", code("framebuffer.sura") + "의 pixel/line/rectangle/full present/cursor damage copy, " + code("font5x7.sura") + "의 terminal용 고정폭 bitmap text, " + code("font_ui.sura") + "의 비례폭 16-pixel 소문자 ASCII·현대 한글 2-bit 안티앨리어싱; QMP screenshot gate로 실제 픽셀 캡처"],
    ["VirtIO GPU 2D", code("virtio_pci.sura") + "·" + code("virtio_gpu.sura") + "의 modern PCI capability, VIRTIO_F_VERSION_1 split control queue, 2D resource/backing/scanout/transfer/flush. 전용 QEMU gate가 640x480 화면의 정확한 RGB 픽셀을 검사하고, 전체 OS는 GOP를 유지한 채 미러링과 무장치 fallback을 실행 검증"],
    ["Intel HDA PCM 출력", code("hda.sura") + "의 PCI/MMIO reset, codec verb, Audio Function Group·output widget 탐색, 48 kHz signed-16 stereo BDL DMA. 전용 QEMU gate가 실제 WAV 형식과 -12000/+12000 샘플을 검사하고, 전체 OS는 bounded startup transfer와 무장치 fallback을 실행 검증"],
    ["그래픽 터미널", code("text_terminal.sura") + "의 Unicode code-point cells, checked UTF-8 output, wrap, scroll, clear, number output와 ASCII·현대 한글 UI-font draw; 별도 CR3의 CPL 3 UTF-8 줄 편집·명령 인식과 ring 0 특권 명령 실행 검증"],
    ["창 관리자·데스크톱 셸", code("window_manager.sura") + "의 focus, z-order, hit test, drag, close/show, bounds와 Start·taskbar activation; QEMU PS/2 입력으로 실행 검증"],
    ["기본 데스크톱 앱", code("desktop_apps.sura") + "의 File Explorer·Text Editor·Calculator state와 System Information snapshot; QEMU에서 각각 별도 CR3를 사용하는 CPL 3 시스템 정보 검증, 폴더 선택, editor 입력·저장, 계산 경로 50-31=19 실행 검증"],
    ["영속 데이터 디스크", code("ahci.sura") + "·" + code("fat32.sura") + "·" + code("vfs.sura") + "·" + code("surafs.sura") + "로 128 MiB MBR 디스크의 FAT32 호환 파티션과 type-0x7f SuraFS 문서 파티션을 사용. 그래픽 탐색기·편집기의 " + code("/문서/메모.txt") + " 저장, 디스크 구조 검사, remount-only 두 번째 부팅의 동일 generation·partition SHA-256을 QEMU에서 검증"],
    ["VirtIO 네트워크·브라우저", code("virtio_net.sura") + "·" + code("net.sura") + "·" + code("http1.sura") + "·" + code("gzip.sura") + "·" + code("http_content.sura") + "·" + code("os/network.sura") + "·" + code("os/https.sura") + "로 split queue, DHCP, ARP, IPv4, UDP DNS, TCP, HTTP/1.1 Content-Length·chunked·connection-close framing, CRC32·ISIZE를 검사하는 bounded gzip/DEFLATE 해제, 제한된 TLS 1.3 HTTPS, 주소 입력, h1/p/a HTML 레이아웃과 CSS 색상 하위 집합을 실행 검증. 주소창의 uncached DNS, 첫 TCP 연결, 첫 TLS handshake는 데스크톱 루프에서 단계적으로 poll되어 대기 중 입력과 취소를 처리하며, " + code("suralang.site") + " 실접속에서 직접 HTTPS 선택·인증서 검증·암호화 응답·렌더링을 확인"],
    ["PNG 디코더와 브라우저 연결", code("png.sura") + "의 non-interlaced 8-bit grayscale·grayscale-alpha·RGB·RGBA → RGBA8, multiple IDAT, zlib/Adler-32, chunk CRC32, filter 0~4 처리. 전용 QEMU gate가 모든 필터의 정확한 픽셀과 손상·용량 거부를 실행 검증하고, 전체 OS gate가 live suralang.site의 대용량 PNG를 HTTPS로 받아 두 슬롯 img 경로에서 decode·layout·paint합니다."],
    ["JPEG 디코더와 브라우저 연결", code("jpeg.sura") + "의 8-bit baseline sequential Huffman grayscale·YCbCr → RGBA8, 4:4:4·4:2:2·4:2:0 sampling, dequantization, integer IDCT, chroma upsampling. 전용 QEMU gate가 AC color gradient·4:2:0 MCU·grayscale과 오류 거부를 실행 검증하고 브라우저의 두 슬롯 img 경로가 이를 호출합니다. progressive·CMYK는 없으며 live JPEG painting은 별도 gate가 아닙니다."],
    ["HTML DOM 기반", code("html_dom.sura") + "의 caller-owned fixed node/attribute tree, parent·child·sibling 관계, UTF-8 text slice, raw script/style text, quoted/unquoted attribute, comment/declaration skip, void element와 bounded ancestor recovery. 전용 QEMU gate와 graphical browser가 DOM box painting, link ancestor hit-test, fragment 이동을 실행 검증"],
    ["브라우저 JavaScript 기반", code("browser_js.sura") + "·" + code("browser_js_source.sura") + "·" + code("browser_js_dom.sura") + "의 caller-owned 고정 용량 bytecode VM·source compiler·DOM host. 정수·문자열·boolean·null·undefined, 전역 선언/대입, 산술·비교, if/else, instruction budget과 host-call ABI를 전용 QEMU gate에서 실행 검증하며, graphical browser는 inline script, inline onclick, getElementById(...).textContent 변경을 " + code("SURA_OS_BROWSER_JS_OK") + "·" + code("SURA_OS_BROWSER_JS_CLICK_OK") + "로 검증. 객체·함수·일반 이벤트 모델·다른 Web API는 아직 없음"],
    ["브라우저 WebAssembly 기반", code("browser_wasm.sura") + "의 caller-owned 고정 용량 MVP 정수 parser/interpreter. custom·type·function·export·code section, i32/i64, direct call, block/loop/if/branch/local/정수 연산을 전용 QEMU gate에서 실행 검증. graphical browser는 WebAssembly magic으로 시작하는 응답의 0인자 run export를 실행하고 결과 42를 " + code("SURA_OS_BROWSER_WASM_OK") + "·" + code("SURA_OS_BROWSER_WASM_PAGE_OK") + "로 검증. import·memory·table·global·float·WASI·JIT·JavaScript WebAssembly API·완전한 명세 검증은 아직 없음"],
    ["CSS 계산 스타일 기반", code("css_style.sura") + "의 caller-owned fixed rule/style arrays, tag·class·ID·inline specificity/source-order cascade, color·font inheritance, display·px와 제한된 상대 크기·min/max·margin/padding/border-width·box-sizing·relative/absolute/fixed position 하위 집합. 전용 QEMU gate와 graphical browser가 inline CSS, 실사이트 same-host 외부 CSS, 계산 스타일을 실행 검증"],
    ["CSS 박스 레이아웃 기반", code("css_box.sura") + "의 content/border/outer geometry, 세로 block flow, 기본 inline flow·폭 기반 줄바꿈, display:none subtree 제거, relative 이동과 absolute/fixed out-of-flow 배치. 전용 QEMU gate와 graphical browser가 box painting, scrolling, link hit-testing을 실행 검증"],
    ["RTC·작업 표시줄 시계", code("rtc.sura") + "의 CMOS 안정 샘플·BCD/12시간 변환과 HH:MM 표시; QEMU에서 " + code("SURA_OS_RTC_OK") + " 실행 검증"],
    ["PS/2 fallback 입력", code("ps2.sura") + "의 translated Set-1 keyboard, 기본 three-byte mouse, IntelliMouse sample-rate 협상과 four-byte wheel packet polling; xHCI 두 장치 열거가 실패하면 데스크톱 fallback으로 사용"],
    ["xHCI·USB boot keyboard/mouse", code("xhci.sura") + "·" + code("usb_hid.sura") + "·" + code("os/input.sura") + "의 PCI/MMIO, reset/run, 두 root-port 장치 Address Device, endpoint-0 descriptor 전송, Configure Endpoint, interrupt-IN ring, KeyEvent·PointerEvent 변환과 데스크톱 dispatch. 별도 keyboard/mouse gate와 전체 OS gate가 QMP 입력을 실행 검증"],
    ["Unicode·한글 입력", code("key_event.sura") + "의 물리 키·누름/뗌/반복·Shift/Ctrl/Alt/Caps, " + code("text_input.sura") + "의 두벌식 조합·조합 중 Backspace·UTF-8, " + code("font_ui.sura") + "의 소문자 ASCII·현대 한글 atlas; 직접 입력 게이트와 Terminal에서 " + code("dkssudgktpdy") + " → 안녕하세요 QEMU 실행 검증"],
    ["부팅 디스크", code("--disk-image") + "로 protective MBR, GPT, FAT32 ESP와 " + code("EFI/BOOT/BOOTX64.EFI") + " 생성"],
    ["UEFI", "console, memory services, protocol lookup, ExitBootServices, GOP framebuffer"],
  ]) +
  paragraph("top-level " + code("name is value") + "는 freestanding 정적 선언입니다. 함수에서 mutable scalar global을 바꾸려면 기존 " + code("global name") + " 문법을 사용합니다. " + code("struct Name packed do") + "는 padding 없는 하드웨어 레이아웃을 만들고, 일반 typed struct는 필드 폭에 맞춰 자연 정렬합니다.") +
  pre("struct Device packed do\n  vendor: u16\n  command: u16\nend\n\ndevice_storage is static.struct(Device)\ncount: u64 is 0\n\nfunc probe() -> u64 do\n  global count\n  device: ptr[Device] is device_storage\n  device.command is 7\n  previous is atomic.fetch_add64(addr_of(count), 1)\n  return device.vendor\nend") +
  paragraph(code("func timer(frame: ptr[Frame]) interrupt do") + "는 error code가 없는 vector용이고 " + code("interrupt_error") + "는 CPU가 error code를 푸시하는 vector용입니다. " + code("cpu.idt_set_gate") + "는 direct handler address와 vector의 error-code ABI를 컴파일 때 검사합니다. 생성된 wrapper는 saved CS가 ring 3일 때 SWAPGS를 실행하고 LFENCE로 결정을 직렬화합니다. TSS RSP/IST와 descriptor 생성, LTR, FXSAVE/XSAVE, XSETBV도 지원하지만 실제 per-CPU 할당, FPU/SIMD 저장, NMI-safe entry 정책은 kernel이 정해야 합니다.") +
  paragraph("Sura freestanding 기반에는 메모리·프로세스·syscall·IPC, SHA-256·SHA-384·AES-128-GCM·X25519·RSA/X.509·TLS 1.3, PCI·ACPI·AHCI·NVMe·xHCI·VirtIO·HDA, SuraFS·FAT32·VFS, IPv4·DHCP·DNS·TCP·HTTP, PNG·JPEG·HTML·CSS·제한된 JavaScript/WebAssembly, framebuffer·창 관리자와 기본 데스크톱 앱이 구현되어 있습니다. 전체 QEMU 데스크톱 gate는 " + code("suralang.site") + "의 직접 HTTPS 선택, 인증서 체인·호스트명·시간과 여섯 개의 pinned root(ISRG Root X1, DigiCert Global Root G2, GlobalSign Root CA - R3, Amazon Root CA 1, USERTrust RSA Certification Authority, Microsoft RSA Root Certificate Authority 2017) 중 일치하는 신뢰점 검증, 암호화 응답, 외부 CSS, DOM/box painting과 PNG 처리를 실행합니다. 주소창 DNS, 첫 TCP 연결, 첫 TLS handshake는 데스크톱 루프에서 점진적으로 진행되며 입력과 취소를 처리합니다. 이후 응답 수신·decode, 추가 리소스 TLS 연결, DOM·layout·rendering은 아직 동기식 ring 0 경로지만 입력을 협력적으로 처리하고 F6 취소로 안전하게 풀립니다. IPv6, 완전한 TCP 재전송·혼잡 제어·out-of-order reassembly, 범용 TLS 알고리즘과 운영체제급 루트 저장소, 인증서 폐기 확인·세션 재개, 완전한 CSS·JavaScript·Web API, GPU 가속, SMP load balancing, complete per-AP lifecycle, FPU/SIMD process state와 ARM64 backend는 아직 없습니다. 자세한 compile-only·executed 구분은 " + code("Guide/OS_DEVELOPMENT.md") + "와 machine-readable data에 있습니다.")
));

sections.push(section("release", "빌드와 배포",
  table(["출력", "내용"], [
    [code(".sura.bc"), code("--compile") + "이 만드는 SURB v3 register bytecode; " + version + " runtime은 v2와 v3를 읽음"],
    [code(".sura.srp"), "release container v5: register bytecode·literal·constant payload, randomized nonce, integrity seal"],
    ["protected metadata", "line/local/function/parameter debug name 제거; optional release id·expiry"],
    ["runtime symbol", "class name, method map key, global name, constant은 실행에 필요한 형태로 package에 남음"],
    ["접근 값", code("--release-key") + ", " + code("--release-license") + "와 실행 시 대응 load option 또는 environment value"],
    ["launcher", code("surapkg protect --exe") + "가 embedded package와 engine locator를 포함한 Windows launcher 생성; 실행 시 옆의 Sura engine 또는 " + code("SURA_ENGINE") + " 경로 사용"],
    ["검사", "package/launcher에서 source text, source line, string literal, key/license byte pattern scan"],
  ]) +
  paragraph("payload transform과 seal은 Sura release container의 자체 형식입니다. 동일 source도 randomized nonce 때문에 package byte가 달라지고 seal 검사는 변조된 payload를 실행 전에 거부합니다.") +
  paragraph("leak scan은 16..65536 byte source 전체, 16 byte 이상 non-comment source line, 4 byte 이상 string literal, 4 byte 이상 key/license byte를 package와 launcher에서 검색합니다. " + code("protect-verify") + "가 report target과 hash를 재검증합니다.") +
  pre("sura --compile app.sura --out app.sura.bc\nsura --release app.sura --out app.sura.srp\nsura --load-release app.sura.srp\nsurapkg protect . --exe\nsurapkg protect-verify dist/protect-report.json --json verify.json")
));

sections.push(section("performance", "성능과 검증 기록",
  table(["검증", "결과"], [
    ["core suite", verificationManifest.results.core_vm + "; " + verificationManifest.results.core_jit],
    ["release evidence", releaseEvidence.passed_count + "/" + releaseEvidence.required_count + (releaseEvidence.passed ? " PASS" : " · incomplete")],
    ["goal audit", goalAudit.passed_count + "/" + goalAudit.required_count + " · " + goalAudit.progress_percent + "% · " + goalAudit.status],
    ["target lowering", "JS full " + targetAudit.js.full + ", ignored " + targetAudit.js.ignored + "; WASM full " + targetAudit.wasm.full + ", partial " + targetAudit.wasm.partial + ", ignored " + targetAudit.wasm.ignored],
    ["bytecode validation", verificationManifest.results.bytecode_validation],
    ["JIT safety", "stable suite의 frame·upvalue·exception regression cases PASS"],
    ["FFI safety", verificationManifest.results.ffi_safety],
    ["async runtime concurrency", verificationManifest.results.async_runtime_concurrency],
    ["release payload", verificationManifest.results.release_payload_identity],
  ]) +
  paragraph("검증 파일의 " + code(verificationManifest.engine_sha256) + " 해시는 현재 " + code(verificationManifest.engine_file) + " " + verificationManifest.engine_bytes + " bytes를 식별합니다. 생성된 installer kit와 single-file 설치 흐름은 installer smoke를 통과했습니다. " + code(verificationManifest.package_helper.file) + "는 " + verificationManifest.package_helper.bytes + " bytes, SHA-256 " + code(verificationManifest.package_helper.sha256) + "입니다.") +
  paragraph("최신 기록은 2026-07-17 Windows x64, " + escapeHtml(nativePerformance.host.cpu) + ", " + escapeHtml(nativePerformance.compiler_version) + "에서 같은 100,000-step inner physics loop를 Sura JIT와 C++ O3로 각각 5회 측정했습니다. Vec2는 Sura JIT " + currentVec2Perf.sura_jit_ms.toFixed(3) + " ms, C++ O3 " + currentVec2Perf.native_ms.toFixed(3) + " ms, 비율 " + currentVec2Perf.sura_native_ratio.toFixed(2) + "×였습니다. Vec3는 Sura JIT " + currentVec3Perf.sura_jit_ms.toFixed(3) + " ms, C++ O3 " + currentVec3Perf.native_ms.toFixed(3) + " ms, 비율 " + currentVec3Perf.sura_native_ratio.toFixed(2) + "×였습니다. 두 fair-scope 검사를 모두 통과했으며, 이 수치는 특정 물리 루프 기록이지 언어 전체 성능을 대표하지 않습니다. 측정 engine SHA-256은 " + code(nativePerformance.engine.sha256) + "입니다.") +
  paragraph("2026-07-13의 이전 공개 기록은 Vec2 38.33×, Vec3 105.62×였고, 2026-07-12의 더 이전 기록은 Vec2 17.225 ms, Vec3 62.254 ms였습니다. 비교 범위와 엔진이 다른 기록은 최신 수치와 섞어 해석하지 않습니다.") +
  paragraph("Guide/GPU_AND_SCALE.md에 기록된 RTX 4060 결과는 Sura 1.8, engine SHA-256 prefix 3270a9의 역사적 측정입니다. direct causal-attention forward+backward 50회 중앙값은 B1/H4/T64/D32에서 Sura 1.5076 ms, PyTorch 0.4649 ms, 비율 3.242×였고 B1/H4/T128/D64에서는 Sura 5.6562 ms, PyTorch 0.4282 ms, 비율 13.209×였습니다.") +
  paragraph("native emitter는 Win64 x64 partial backend, Linux x86-64 System V baseline, little-endian Windows/Linux/macOS ARM64 AAPCS64 baseline을 구현합니다. 두 baseline은 helper를 호출하지 않는 상수·이동·정적으로 숫자임이 증명된 덧셈·뺄셈·곱셈·단항 음수·반환만 처리하고, 나머지는 register VM에서 실행합니다. macOS x86-64에는 native backend가 없습니다. Vec2/Vec3 benchmark의 strict counted-loop shortcut은 인식된 top-level loop, closure identity, 정확한 field-copy constructor, Vec2/Vec3 layout, add/scale/cross와 step/step3 bytecode graph, numeric input, escape·non-alias 조건을 모두 증명한 경우에만 사용합니다. 하나라도 맞지 않으면 원래 VM/native bytecode path로 실행합니다. 한 번 이상 반복한 shortcut은 원래 position을 변경하지 않고 fresh result instance를 저장하므로 원래 객체를 가리키는 alias의 field가 바뀌지 않으며, 0회 반복은 원래 identity를 유지합니다. 이 경로는 일반 사용자 loop 최적화가 아닙니다. 일반 control-flow deoptimization, 범용 escape analysis, register allocation, loop-invariant code motion은 구현되지 않았습니다. Windows x64 native helper frame은 UNWIND_INFO를 등록합니다.")
));

const machineJson = JSON.stringify(machineFacts, null, 2).replaceAll("<", "\\u003c");
sections.push(section("machine", "구조화 데이터",
  paragraph("이 HTML의 " + code('script#sura-reference-data[type="application/json"]') + "에 version, compatibility contract와 support tier, runtime contract, 기본값 평가, syntax, truthiness, stdlib catalog, 34개 module의 " + apiSymbolCount + "개 signature, async, target 차이, external dependency, release artifact hash·signing·license, performance record, verification과 CLI help를 JSON으로 포함합니다.") +
  paragraph("각 module API entry는 이름, 호출 signature와 source 위치를 제공합니다. 공통 runtime 제한과 async·FFI·release·target 계약은 이 문서의 prose와 같은 JSON object에 기록합니다.") +
  paragraph("optional parameter는 signature에서 " + code("[name]") + ", variadic parameter는 " + code("...") + "로 표시합니다. 호출 이름과 argument 수는 각 signature를 기준으로 사용합니다.") +
  "<details class='machine-preview'><summary>구조화 데이터 보기</summary>" + pre(JSON.stringify(machineFacts, null, 2)) + "</details>"
));

let css = fs.readFileSync(cssPath, "utf8");
css = css.replace(/^@import[^\n]*\n/, "");

const navigation = navItems.map(([id, label]) => "<a href='#" + id + "'>" + escapeHtml(label) + "</a>").join("");
const clientScript = `(function(){
  var input = document.getElementById('filter');
  var status = document.getElementById('filter-status');
  var sections = Array.from(document.querySelectorAll('main section'));
  var navLinks = Array.from(document.querySelectorAll('.nav-section a'));

  function updateFilter() {
    var q = input.value.trim().toLowerCase();
    var visible = 0;
    sections.forEach(function(item) {
      var hidden = Boolean(q) && !item.textContent.toLowerCase().includes(q) && !item.id.toLowerCase().includes(q);
      item.classList.toggle('filtered-out', hidden);
      var link = document.querySelector('.nav-section a[href="#' + item.id + '"]');
      if (link) link.classList.toggle('filtered-out', hidden);
      if (!hidden) visible += 1;
    });
    status.textContent = q ? visible + '개 섹션 검색됨' : '전체 ' + sections.length + '개 섹션';
  }

  input.addEventListener('input', updateFilter);
  updateFilter();

  function fallbackCopy(value) {
    var area = document.createElement('textarea');
    area.value = value;
    area.setAttribute('readonly', '');
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.appendChild(area);
    area.select();
    document.execCommand('copy');
    area.remove();
  }

  document.querySelectorAll('pre').forEach(function(block) {
    var codeBlock = block.querySelector('code');
    if (!codeBlock) return;
    var value = codeBlock.textContent;
    var button = document.createElement('button');
    button.type = 'button';
    button.className = 'copy-code';
    button.textContent = '복사';
    button.setAttribute('aria-label', '코드 복사');
    button.addEventListener('click', function() {
      var complete = function() {
        button.textContent = '복사됨';
        window.setTimeout(function() { button.textContent = '복사'; }, 1200);
      };
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(value).then(complete).catch(function() { fallbackCopy(value); complete(); });
      } else {
        fallbackCopy(value);
        complete();
      }
    });
    block.appendChild(button);
  });

  sections.forEach(function(item) {
    var heading = item.querySelector(':scope > h2');
    if (!heading) return;
    var anchor = document.createElement('a');
    anchor.className = 'heading-anchor';
    anchor.href = '#' + item.id;
    anchor.textContent = '#';
    anchor.setAttribute('aria-label', heading.textContent + ' 링크');
    heading.appendChild(anchor);
  });

  if ('IntersectionObserver' in window) {
    var observer = new IntersectionObserver(function(entries) {
      entries.forEach(function(entry) {
        if (!entry.isIntersecting) return;
        navLinks.forEach(function(link) { link.classList.remove('active'); });
        var active = document.querySelector('.nav-section a[href="#' + entry.target.id + '"]');
        if (active) active.classList.add('active');
      });
    }, { rootMargin: '-10% 0px -78% 0px' });
    sections.forEach(function(item) { observer.observe(item); });
  }
})();`;

let html = "<!DOCTYPE html>\n<html lang='ko'>\n<head>\n<meta charset='UTF-8'>\n" +
  "<meta name='viewport' content='width=device-width,initial-scale=1'>\n" +
  "<meta name='description' content='Sura " + version + "의 문법, 런타임, CLI, 표준 라이브러리, 타깃 제한과 검증 기록을 한 문서에서 확인하는 공식 레퍼런스'>\n" +
  "<title>Sura 언어 레퍼런스 " + version + "</title>\n<style>" + css + "</style>\n</head>\n<body>\n" +
  "<a class='skip-link' href='#main-content'>본문 바로가기</a>\n<nav aria-label='레퍼런스 목차'>" +
  "<div class='logo'><a class='logo-home' href='/' aria-label='Sura 홈페이지'><span>Sura</span><strong>Reference</strong></a><span class='nav-version'>v" + version + "</span></div>" +
  "<div class='nav-search'><label class='sr-only' for='filter'>문서 검색</label><input id='filter' type='search' placeholder='문법, 명령, API 검색' autocomplete='off'></div>" +
  "<p id='filter-status' class='search-status' aria-live='polite'>전체 문서</p>" +
  "<div class='nav-section'>" + navigation + "</div></nav>\n" +
  "<main id='main-content'><header class='official-hero'><span class='doc-shell-label'>DOCUMENTATION / VERSION " + version + "</span>" +
  "<h1 class='official-title'>Sura 언어<br>레퍼런스</h1>" +
  "<p class='official-subtitle'>문법과 실행 모델부터 CLI, 표준 라이브러리, 플랫폼별 제한, 배포 파일의 검증 근거까지 한 HTML 문서에 정리했습니다. 다른 도구가 읽을 수 있는 구조화 JSON도 같은 문서에 포함합니다.</p>" +
  "<div class='doc-index'><a href='#install'>설치와 실행</a><a href='#lexical'>문법부터 읽기</a><a href='#stdlib'>API 찾기</a><a href='#machine'>구조화 데이터</a></div>" +
  "<p class='doc-meta'>" + version + " · 2026-07-19 · UTF-8 · " + apiSymbolCount + " API signatures</p></header>" +
  sections.join("\n") + "</main>\n" +
  "<script id='sura-reference-data' type='application/json'>" + machineJson + "</script>\n" +
  "<script>" + clientScript + "</script>\n" +
  "</body>\n</html>\n";

html = html
  .replace(
    "다섯 데스크톱 작업자는 persistent UserProcessScheduler 프로세스이지만 커널 dispatcher가 요청된 작업자 하나를 깨우고 다시 block할 때까지 기다립니다.",
    "여섯 번째 persistent UserProcessScheduler 프로세스인 Window Server도 별도 CR3와 이벤트 큐를 사용하며, 실제 GOP backbuffer를 공유 매핑해 제한된 합성 작업과 픽셀 쓰기·복원을 실행합니다. 일반 UI 요청은 요청된 작업자가 다시 block할 때까지 기다립니다.",
  )
  .replace(
    "하드웨어 스냅샷 수집, 터미널 렌더링·특권 명령, SuraFS VFS 조작과 모든 창 렌더링은 ring 0에 남아 있습니다.",
    "하드웨어 스냅샷 수집, 터미널 렌더링·특권 명령, SuraFS VFS 조작과 기본 앱 콘텐츠 렌더링은 ring 0에 남아 있습니다.",
  )
  .replace(
    "resize/minimize/maximize와 독립 user-space window server를 포함한 완전한 interactive window system",
    "전체 크기 앱 소유 Ring 3 surface와 완전한 user-space composition, 창 애니메이션·drag-and-drop·실제 multi-output·완전한 accessibility service",
  );

html = html.replace(
  "브라우저 URL·capability 검증은 전용 Ring 3 worker를 통과하고 주소창의 uncached DNS는 데스크톱 루프에서 점진적으로 poll되어 대기 중 키보드·마우스와 취소를 처리합니다. TCP connect·TLS·decode·DOM·layout·rendering 및 링크·폼 탐색은 아직 동기식 ring 0 경로입니다.",
  "브라우저 URL·capability 검증은 전용 Ring 3 worker를 통과합니다. 주소창의 uncached DNS, 첫 TCP connect, 첫 TLS handshake는 데스크톱 루프에서 단계적으로 poll되어 대기 중 키보드·마우스와 F6 취소를 처리합니다. 응답 decode, 추가 리소스 TLS, DOM·layout·rendering 및 링크·폼 탐색은 아직 동기식 ring 0 경로입니다.",
);

const rootReference = path.join(root, "reference.html");
const publicReference = path.join(siteRoot, "public", "reference.html");
const expectedBytes = Buffer.from(html, "utf8");
if (checkOnly) {
  for (const candidate of [rootReference, publicReference]) {
    if (!fs.existsSync(candidate) || !fs.readFileSync(candidate).equals(expectedBytes)) {
      throw new Error(`stale generated reference: ${candidate}; run node tools/sura_reference_generate.mjs`);
    }
  }
} else {
  fs.writeFileSync(rootReference, expectedBytes);
  fs.writeFileSync(publicReference, expectedBytes);
}
const rootBytes = fs.readFileSync(rootReference);
const publicBytes = fs.readFileSync(publicReference);
if (!rootBytes.equals(publicBytes)) throw new Error("root and public reference files differ");

console.log(checkOnly ? "reference freshness: PASS" : "reference generated:", rootReference);
console.log(checkOnly ? "public reference freshness: PASS" : "public copy:", publicReference);
console.log("version:", version);
console.log("stdlib:", catalog.stdlib_count, "inventory entries,", 35, "runtime modules,", modules.length, "catalog modules,", apiSymbolCount, "module signatures,", globalBuiltinNames.length, "global names");
