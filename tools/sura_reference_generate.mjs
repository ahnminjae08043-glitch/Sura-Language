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
      strings: "double quoted; supported escapes are \\n, \\t, \\\", and \\\\",
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
    status: "experimental compiler target; not a complete operating system",
    target: "uefi-x86_64",
    output: "position-independent PE32+ EFI application",
    hosted_runtime_dependencies: [],
    entry_order: ["efi_main", "kernel_main", "main", "synthetic top-level entry"],
    scalar_types: ["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "isize", "usize", "ptr", "ptr[StructName]"],
    static_data: ["static.zero", "static.bytes", "static.u8", "static.u16", "static.u32", "static.u64", "static.utf8", "static.utf16", "static.struct"],
    layout: ["typed struct fields", "natural alignment", "struct Name packed", "sizeof", "alignof", "offset_of", "typed pointer field load/store"],
    low_level: ["raw memory 8/16/32/64", "port I/O 8/16/32", "control registers", "MSR", "GDT", "IDT", "INVLPG", "CPUID", "RDTSC/RDTSCP", "XGETBV"],
    atomics: ["load", "store", "exchange", "compare_exchange", "fetch_add", "fetch_sub", "fences"],
    verification: ["tests/os_target_unit.cpp", "tools/sura_uefi_target_smoke.ps1", "examples/os/freestanding_features.sura"],
    not_implemented: ["interrupt-function ABI", "application-processor startup", "kernel libraries", "drivers", "filesystem", "boot-image builder", "ARM64 freestanding backend"],
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
  paragraph("source는 UTF-8이며 UTF-8 BOM도 받습니다. 한 줄 주석은 " + code("#") + "와 " + code("//") + "를 사용합니다. 문자열은 큰따옴표로 작성하고 " + code("\\n \\t \\\" \\\\") + " escape와 " + code("{expression}") + " interpolation을 지원합니다.") +
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
  paragraph(code("uefi-x86_64") + "는 Sura VM, GC, Windows API, C runtime, 외부 assembler·linker 없이 PE32+ EFI application을 직접 만드는 실험 타깃입니다. 현재 결과물은 OS가 아니라 앞으로 커널과 드라이버를 작성하기 위한 컴파일러 기능입니다. 이 기능은 현재 저장소의 개발 소스에 있으며 기존 공개 설치 파일에는 아직 포함되지 않았습니다.") +
  pre(".\\SuraLanguage.exe --target uefi-x86_64 --out FEATURES.EFI examples\\os\\freestanding_features.sura") +
  table(["영역", "현재 구현"], [
    ["정수·포인터", code("i8/u8/i16/u16/i32/u32/i64/u64/isize/usize/ptr") + ", " + code("ptr[StructName]")],
    ["정적 데이터", code("static.zero/bytes/u8/u16/u32/u64/utf8/utf16/struct")],
    ["메모리 레이아웃", "typed struct field, natural 또는 packed layout, " + code("sizeof/alignof/offset_of")],
    ["포인터", code("ptr.add/index/field/align_up/align_down/is_aligned") + "와 width-correct field load/store"],
    ["저수준 CPU", "raw memory, port I/O, CR0/2/3/4, MSR, GDT, IDT, INVLPG, CPUID, RDTSC/RDTSCP, XGETBV"],
    ["원자 연산", "8/16/32/64-bit load, store, exchange, compare-exchange, fetch-add/sub, fence"],
    ["UEFI", "console, memory services, protocol lookup, ExitBootServices, GOP framebuffer"],
  ]) +
  paragraph("top-level " + code("name is value") + "는 freestanding 정적 선언입니다. 함수에서 mutable scalar global을 바꾸려면 기존 " + code("global name") + " 문법을 사용합니다. " + code("struct Name packed do") + "는 padding 없는 하드웨어 레이아웃을 만들고, 일반 typed struct는 필드 폭에 맞춰 자연 정렬합니다.") +
  pre("struct Device packed do\n  vendor: u16\n  command: u16\nend\n\ndevice_storage is static.struct(Device)\ncount: u64 is 0\n\nfunc probe() -> u64 do\n  global count\n  device: ptr[Device] is device_storage\n  device.command is 7\n  previous is atomic.fetch_add64(addr_of(count), 1)\n  return device.vendor\nend") +
  paragraph("아직 없는 기능은 전용 interrupt 함수 ABI와 saved-register frame, application-processor 시작, page table·allocator·scheduler·syscall 라이브러리, 장치 드라이버, filesystem, boot-image builder, ARM64 freestanding backend입니다. " + code("examples/os/freestanding_features.sura") + "는 기능 시험이며 완성 OS가 아닙니다. 자세한 계약은 " + code("Guide/OS_DEVELOPMENT.md") + "에 있습니다.")
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

const html = "<!DOCTYPE html>\n<html lang='ko'>\n<head>\n<meta charset='UTF-8'>\n" +
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
  "<p class='doc-meta'>" + version + " · 2026-07-17 · UTF-8 · " + apiSymbolCount + " API signatures</p></header>" +
  sections.join("\n") + "</main>\n" +
  "<script id='sura-reference-data' type='application/json'>" + machineJson + "</script>\n" +
  "<script>" + clientScript + "</script>\n" +
  "</body>\n</html>\n";

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
