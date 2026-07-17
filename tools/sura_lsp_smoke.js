const cp = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const engine = process.argv[2] || ".\\SuraLanguage.exe";
const child = cp.spawn(engine, ["--lsp"], { cwd: process.cwd(), stdio: ["pipe", "pipe", "pipe"] });
child.stdin.on("error", (error) => {
  if (error && error.code === "EPIPE") return;
  throw error;
});

let buffer = Buffer.alloc(0);
let nextId = 1;
const pending = new Map();
const diagnosticsByUri = new Map();
const diagnosticWaiters = [];

function send(message) {
  if (child.stdin.destroyed || child.killed) return;
  const body = Buffer.from(JSON.stringify(message), "utf8");
  child.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  child.stdin.write(body);
}

function request(method, params) {
  const id = nextId++;
  send({ jsonrpc: "2.0", id, method, params });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      if (pending.has(id)) {
        pending.delete(id);
        reject(new Error(`timeout waiting for ${method}`));
      }
    }, 8000);
    pending.set(id, { resolve, reject, method, timer });
  });
}

function notify(method, params) {
  send({ jsonrpc: "2.0", method, params });
}

function finishRequest(message) {
  if (!Object.prototype.hasOwnProperty.call(message, "id")) return;
  const item = pending.get(message.id);
  if (!item) return;
  pending.delete(message.id);
  clearTimeout(item.timer);
  item.resolve(message);
}

function finishNotification(message) {
  if (message.method !== "textDocument/publishDiagnostics") return;
  const uri = message.params && message.params.uri;
  if (!uri) return;
  const diagnostics = message.params.diagnostics || [];
  diagnosticsByUri.set(uri, diagnostics);
  for (let i = diagnosticWaiters.length - 1; i >= 0; i--) {
    const waiter = diagnosticWaiters[i];
    if (waiter.uri === uri && waiter.predicate(diagnostics)) {
      diagnosticWaiters.splice(i, 1);
      clearTimeout(waiter.timer);
      waiter.resolve(diagnostics);
    }
  }
}

function waitForDiagnostics(uri, predicate, label) {
  const current = diagnosticsByUri.get(uri);
  if (current && predicate(current)) return Promise.resolve(current);
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      const latest = diagnosticsByUri.get(uri) || [];
      const sources = latest.map((diagnostic) => diagnostic.source).join(", ");
      reject(new Error(`timeout waiting for ${label}; latest sources: ${sources}`));
    }, 8000);
    diagnosticWaiters.push({ uri, predicate, resolve, reject, timer });
  });
}

child.stdout.on("data", (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);
  while (true) {
    const marker = buffer.indexOf(Buffer.from("\r\n\r\n"));
    if (marker < 0) return;
    const header = buffer.slice(0, marker).toString("ascii");
    const match = header.match(/Content-Length:\s*(\d+)/i);
    if (!match) throw new Error(`bad LSP header: ${header}`);
    const length = Number(match[1]);
    const start = marker + 4;
    if (buffer.length < start + length) return;
    const body = buffer.slice(start, start + length).toString("utf8");
    buffer = buffer.slice(start + length);
    const message = JSON.parse(body);
    finishNotification(message);
    finishRequest(message);
  }
});

child.stderr.on("data", (chunk) => process.stderr.write(chunk));

function fileUri(filePath) {
  const absolute = path.resolve(filePath).replace(/\\/g, "/");
  return `file:///${encodeURI(absolute)}`;
}

async function main() {
  const projectRoot = fs.mkdtempSync(path.join(os.tmpdir(), "sura-lsp-project-"));
  const diskDefPath = path.join(projectRoot, "disk_defs.sura");
  fs.writeFileSync(
    diskDefPath,
    ["func disk_score(x) do", "return x * 2", "end", ""].join("\n"),
    "utf8",
  );
  fs.mkdirSync(path.join(projectRoot, "node_modules"));
  fs.writeFileSync(
    path.join(projectRoot, "node_modules", "ignored.sura"),
    ["func ignored_score(x) do", "return x", "end", ""].join("\n"),
    "utf8",
  );
  const diskDefUri = fileUri(diskDefPath);

  const init = await request("initialize", {
    processId: process.pid,
    rootUri: fileUri(projectRoot),
    capabilities: {},
  });
  const caps = (init.result && init.result.capabilities) || {};
  if (caps.workspaceSymbolProvider !== true) throw new Error("workspaceSymbolProvider missing");
  const sync = caps.textDocumentSync || {};
  if (sync.change !== 2) throw new Error(`incremental text sync missing: ${JSON.stringify(caps.textDocumentSync)}`);
  if (!caps.semanticTokensProvider || !caps.semanticTokensProvider.full) {
    throw new Error("semanticTokensProvider missing");
  }
  if (caps.documentFormattingProvider !== true) throw new Error("documentFormattingProvider missing");
  if (caps.renameProvider !== true) throw new Error("renameProvider missing");
  if (!caps.codeActionProvider) throw new Error("codeActionProvider missing");
  if (!caps.signatureHelpProvider || !caps.signatureHelpProvider.triggerCharacters.includes("(")) {
    throw new Error("signatureHelpProvider missing");
  }
  notify("initialized", {});

  const uriA = "file:///C:/tmp/sura_lsp_a.sura";
  const uriB = "file:///C:/tmp/sura_lsp_b.sura";
  const constantsUri = "file:///C:/tmp/sura_lsp_constants.sura";
  const typedUri = "file:///C:/tmp/sura_lsp_typed_hover.sura";
  const sensitiveHeadersUri = "file:///C:/tmp/sura_lsp_sensitive_headers.sura";
  const toolPolicyUri = "file:///C:/tmp/sura_lsp_tool_policy.sura";
  const legacySyntaxUri = "file:///C:/tmp/sura_lsp_legacy_syntax.sura";
  const textA = [
    "func shared_score(x) do",
    "return x + 1",
    "end",
    "",
    "struct AgentScore do",
    "value",
    "end",
    "",
  ].join("\n");
  const textBLines = [
    "value is shared_score(41)",
    "disk_value is disk_score(5)",
    'payload is http_post("https://example.com", "body", "text/plain")',
    'page is http_get("https://example.com/page")',
    'jsonPage is http_json("https://example.com/data.json")',
    'getTask is async_http_get("https://example.com/status")',
    'requestTask is async_http_request({url: "https://example.com/status", timeout: 10})',
    'pyResult is python_call("math", "sqrt", [9])',
    'lib is ffi_load("native.dll")',
    'ffiResult is ffi_call(lib, "add", "int(int,int)", 1, 2)',
    'plug is plugin_load_manifest("native/plugin.sura-plugin.json")',
    'pluginResult is plugin_call(plug, "native_add", 1, 2)',
    "func local_use() do",
    "return shared_score(value)",
    "end",
    "",
  ];
  const textB = textBLines.join("\n");
  const httpPostLine = textBLines.findIndex((line) => line.includes("http_post("));
  const httpGetLine = textBLines.findIndex((line) => line.includes("http_get("));
  const httpJsonLine = textBLines.findIndex((line) => line.includes("http_json("));
  const asyncHttpGetLine = textBLines.findIndex((line) => line.includes("async_http_get("));
  const asyncHttpRequestLine = textBLines.findIndex((line) => line.includes("async_http_request("));
  const pythonCallLine = textBLines.findIndex((line) => line.includes("python_call("));
  const ffiLoadLine = textBLines.findIndex((line) => line.includes("ffi_load("));
  const ffiCallLine = textBLines.findIndex((line) => line.includes("ffi_call("));
  const pluginLoadManifestLine = textBLines.findIndex((line) => line.includes("plugin_load_manifest("));
  const pluginCallLine = textBLines.findIndex((line) => line.includes("plugin_call("));
  const localReturnLine = textBLines.findIndex((line) => line.includes("return shared_score"));
  const constantsLines = [
    "BUILD_LIMIT is 42",
    "func use_limit() do",
    "local_constant is 7",
    "return BUILD_LIMIT",
    "end",
    "",
  ];
  const constantsText = constantsLines.join("\n");
  const typedLines = [
    "func local_types() do",
    "count is 42",
    'count is "forty-two"',
    'name is "Ari"',
    "flags is [true, false]",
    'config is {mode: "fast", retries: 2}',
    "inner is nil",
    "return inner",
    "end",
    "",
  ];
  const typedText = typedLines.join("\n");
  const sensitiveHeadersLines = [
    'headers is http.headers_merge(http.auth_bearer("token"), {"X-Api-Key": "secret"})',
    "print headers",
    'log.info("request", headers)',
    "safe_headers is headers_redact(headers)",
    "print safe_headers",
    "",
  ];
  const sensitiveHeadersText = sensitiveHeadersLines.join("\n");
  const sensitivePrintLine = sensitiveHeadersLines.findIndex((line) => line === "print headers");
  const sensitiveLogLine = sensitiveHeadersLines.findIndex((line) => line.includes("log.info"));
  const toolPolicyLines = [
    "use tool",
    'spec is tool.spec("http_get", {url: "file:///tmp/data.json"})',
    "result is tool_call(spec)",
    "module_result is tool.call(spec)",
    "wide_policy is {}",
    "weak_result is tool_call_policy(spec, wide_policy)",
    "weak_module_result is tool.call_policy(spec, {})",
    'prefixless_policy is {tools: ["http_get"]}',
    "prefixless_result is tool_call_policy(spec, prefixless_policy)",
    'prefixless_module_result is tool.call_policy(spec, {tools: ["http_get"]})',
    'safe_result is tool_call_policy(spec, {tools: ["http_get"], url_prefixes: ["file://"]})',
    'safe_module_result is tool.call_policy(spec, {tools: ["http_get"], url_prefixes: ["file://"]})',
    "",
  ];
  const toolPolicyText = toolPolicyLines.join("\n");
  const directToolCallLine = toolPolicyLines.findIndex((line) => line.includes("tool_call("));
  const moduleToolCallLine = toolPolicyLines.findIndex((line) => line.includes("tool.call("));
  const widePolicyLine = toolPolicyLines.findIndex((line) => line === "wide_policy is {}");
  const weakToolPolicyLine = toolPolicyLines.findIndex((line) => line.includes("weak_result"));
  const weakModuleToolPolicyLine = toolPolicyLines.findIndex((line) => line.includes("weak_module_result"));
  const prefixlessPolicyLine = toolPolicyLines.findIndex((line) => line.includes("prefixless_policy"));
  const prefixlessToolPolicyLine = toolPolicyLines.findIndex((line) => line.includes("prefixless_result"));
  const prefixlessModuleToolPolicyLine = toolPolicyLines.findIndex((line) => line.includes("prefixless_module_result"));
  const starterToolPolicy = '{tools: ["http_get"], url_prefixes: ["file://"], http_methods: ["GET"], allow_shell: false}';
  const legacySyntaxText = ['print "legacy"', ""].join("\n");

  notify("textDocument/didOpen", {
    textDocument: { uri: uriA, languageId: "sura", version: 1, text: textA },
  });
  notify("textDocument/didOpen", {
    textDocument: { uri: uriB, languageId: "sura", version: 1, text: textB },
  });
  notify("textDocument/didOpen", {
    textDocument: { uri: constantsUri, languageId: "sura", version: 1, text: constantsText },
  });
  notify("textDocument/didOpen", {
    textDocument: { uri: typedUri, languageId: "sura", version: 1, text: typedText },
  });
  notify("textDocument/didOpen", {
    textDocument: { uri: sensitiveHeadersUri, languageId: "sura", version: 1, text: sensitiveHeadersText },
  });
  notify("textDocument/didOpen", {
    textDocument: { uri: toolPolicyUri, languageId: "sura", version: 1, text: toolPolicyText },
  });
  notify("textDocument/didOpen", {
    textDocument: { uri: legacySyntaxUri, languageId: "sura", version: 1, text: legacySyntaxText },
  });
  await waitForDiagnostics(uriA, (diagnostics) => diagnostics.length === 0, "clean diagnostics for uriA");
  await waitForDiagnostics(uriB, (diagnostics) => diagnostics.length === 0, "clean diagnostics for uriB");
  await waitForDiagnostics(constantsUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for constants doc");
  await waitForDiagnostics(typedUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for typed hover doc");
  const sensitiveHeaderDiagnostics = await waitForDiagnostics(
    sensitiveHeadersUri,
    (diagnostics) =>
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("headers_redact") &&
          diagnostic.range.start.line === sensitivePrintLine,
      ) &&
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("headers_redact") &&
          diagnostic.range.start.line === sensitiveLogLine,
      ),
    "sensitive header diagnostics",
  );
  const toolPolicyDiagnostics = await waitForDiagnostics(
    toolPolicyUri,
    (diagnostics) =>
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("tool_call_policy") &&
          diagnostic.range.start.line === directToolCallLine,
      ) &&
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("tool.call_policy") &&
          diagnostic.range.start.line === moduleToolCallLine,
      ) &&
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("weak tool policy") &&
          diagnostic.range.start.line === weakToolPolicyLine,
      ) &&
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("weak tool policy") &&
          diagnostic.range.start.line === weakModuleToolPolicyLine,
      ) &&
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("url_prefixes") &&
          diagnostic.range.start.line === prefixlessToolPolicyLine,
      ) &&
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("url_prefixes") &&
          diagnostic.range.start.line === prefixlessModuleToolPolicyLine,
      ),
    "direct tool_call policy diagnostics",
  );
  const legacySyntaxDiagnostics = await waitForDiagnostics(
    legacySyntaxUri,
    (diagnostics) =>
      diagnostics.some(
        (diagnostic) =>
          diagnostic.source === "sura-lint" &&
          diagnostic.message.includes("legacy command syntax") &&
          diagnostic.range.start.line === 0,
      ),
    "legacy command syntax diagnostic",
  );

  const incrementalUri = "file:///C:/tmp/sura_lsp_incremental.sura";
  const incrementalText = ["func change_score(x) do", "return x", "end", ""].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: incrementalUri, languageId: "sura", version: 1, text: incrementalText },
  });
  await waitForDiagnostics(
    incrementalUri,
    (diagnostics) => diagnostics.length === 0,
    "clean diagnostics for incremental doc",
  );
  notify("textDocument/didChange", {
    textDocument: { uri: incrementalUri, version: 2 },
    contentChanges: [
      {
        range: { start: { line: 0, character: 5 }, end: { line: 0, character: 17 } },
        rangeLength: 12,
        text: "changed_score",
      },
    ],
  });
  await waitForDiagnostics(
    incrementalUri,
    (diagnostics) => diagnostics.length === 0,
    "clean diagnostics after incremental change",
  );
  const incrementalSymbols = await request("textDocument/documentSymbol", {
    textDocument: { uri: incrementalUri },
  });
  const incrementalNames = incrementalSymbols.result.map((symbol) => symbol.name);
  if (!incrementalNames.includes("changed_score") || incrementalNames.includes("change_score")) {
    throw new Error(`incremental didChange was not applied: ${JSON.stringify(incrementalSymbols.result)}`);
  }

  const completion = await request("textDocument/completion", {
    textDocument: { uri: uriB },
    position: { line: 0, character: 0 },
  });
  const completionItems = Array.isArray(completion.result)
    ? completion.result
    : (completion.result && Array.isArray(completion.result.items) ? completion.result.items : []);
  const completionLabels = completionItems.map((item) => item.label);
  if (
    !completionLabels.includes("schema_errors") ||
    !completionLabels.includes("http_request") ||
    !completionLabels.includes("async_http_get") ||
    !completionLabels.includes("async_sleep") ||
    !completionLabels.includes("test_summary") ||
    !completionLabels.includes("async_status") ||
    !completionLabels.includes("async_cleanup") ||
    !completionLabels.includes("http_request_json_checked") ||
    !completionLabels.includes("http_request_retry_json") ||
    !completionLabels.includes("http_request_retry_json_checked") ||
    !completionLabels.includes("auth_bearer") ||
    !completionLabels.includes("headers_merge") ||
    !completionLabels.includes("headers_get") ||
    !completionLabels.includes("headers_has") ||
    !completionLabels.includes("headers_redact") ||
    !completionLabels.includes("cookie_parse") ||
    !completionLabels.includes("cookie_build") ||
    !completionLabels.includes("cookie_get") ||
    !completionLabels.includes("form_build") ||
    !completionLabels.includes("form_parse") ||
    !completionLabels.includes("http_content_type") ||
    !completionLabels.includes("http_charset") ||
    !completionLabels.includes("http_is_json") ||
    !completionLabels.includes("http_retry_after") ||
    !completionLabels.includes("http_backoff_delays") ||
    !completionLabels.includes("file_sha256") ||
    !completionLabels.includes("file_hmac_sha256") ||
    !completionLabels.includes("temp_dir") ||
    !completionLabels.includes("cmd_exists") ||
    !completionLabels.includes("cmd_quote") ||
    !completionLabels.includes("cmd_join") ||
    !completionLabels.includes("cmd_run") ||
    !completionLabels.includes("cmd_run_checked") ||
    !completionLabels.includes("win_poll") ||
    !completionLabels.includes("crypto_random_hex") ||
    !completionLabels.includes("constant_time_eq") ||
    !completionLabels.includes("log_set_level") ||
    !completionLabels.includes("log_get_level") ||
    !completionLabels.includes("log_level") ||
    !completionLabels.includes("tensor_clip") ||
    !completionLabels.includes("tensor_mean") ||
    !completionLabels.includes("tensor_variance") ||
    !completionLabels.includes("tensor_std") ||
    !completionLabels.includes("tensor_max") ||
    !completionLabels.includes("tensor_argmin") ||
    !completionLabels.includes("tensor_argmax") ||
    !completionLabels.includes("tensor_zscore") ||
    !completionLabels.includes("tensor_softmax") ||
    !completionLabels.includes("nn_train") ||
    !completionLabels.includes("nn_predict") ||
    !completionLabels.includes("nn_standardize") ||
    !completionLabels.includes("stream_batch") ||
    !completionLabels.includes("datetime_parse") ||
    !completionLabels.includes("array") ||
    !completionLabels.includes("math") ||
    !completionLabels.includes("path") ||
    !completionLabels.includes("string") ||
    !completionLabels.includes("os") ||
    !completionLabels.includes("cli") ||
    !completionLabels.includes("json") ||
    !completionLabels.includes("fs") ||
    !completionLabels.includes("db") ||
    !completionLabels.includes("llm") ||
    !completionLabels.includes("random") ||
    !completionLabels.includes("python") ||
    !completionLabels.includes("ffi") ||
    !completionLabels.includes("plugin") ||
    !completionLabels.includes("vector") ||
    !completionLabels.includes("graphics3d") ||
    !completionLabels.includes("nn") ||
    !completionLabels.includes("ai") ||
    !completionLabels.includes("autograd") ||
    !completionLabels.includes("tokenizer") ||
    !completionLabels.includes("dataset") ||
    !completionLabels.includes("autograd_dtype") ||
    !completionLabels.includes("autograd_all_reduce_gradients") ||
    !completionLabels.includes("tokenizer_encode") ||
    !completionLabels.includes("dataset_open") ||
    !completionLabels.includes("shared_score") ||
    !completionLabels.includes("AgentScore") ||
    !completionLabels.includes("disk_score") ||
    !completionLabels.includes("BUILD_LIMIT") ||
    completionLabels.includes("local_constant")
  ) {
    throw new Error(`completion missing stdlib or workspace symbols, or leaked a local constant: ${JSON.stringify(completion.result)}`);
  }
  const directAutogradBuiltins = [
    "autograd_tensor", "autograd_parameter", "autograd_zeros", "autograd_ones", "autograd_randn",
    "autograd_data", "autograd_grad", "autograd_grad_info", "autograd_dtype", "autograd_device", "autograd_to", "autograd_storage_bytes", "autograd_cast",
    "autograd_shape", "autograd_numel", "autograd_limits", "autograd_autocast", "autograd_item",
    "autograd_detach", "autograd_set_requires_grad", "autograd_requires_grad", "autograd_add", "autograd_sub", "autograd_mul",
    "autograd_div", "autograd_neg", "autograd_reshape", "autograd_matmul", "autograd_transpose", "autograd_linear",
    "autograd_relu", "autograd_tanh", "autograd_sigmoid", "autograd_gelu", "autograd_layer_norm",
    "autograd_embedding", "autograd_causal_attention", "autograd_softmax", "autograd_sum",
    "autograd_mean", "autograd_mse", "autograd_bce", "autograd_bce_logits", "autograd_cross_entropy", "autograd_cross_entropy_ids",
    "autograd_backward", "autograd_backward_scaled", "autograd_zero_grad", "autograd_unscale_gradients", "autograd_sgd", "autograd_adam",
    "autograd_reset_optimizer", "autograd_grad_norm", "autograd_clip_grad_norm",
    "autograd_save_checkpoint", "autograd_load_checkpoint",
    "autograd_cuda_available", "autograd_cuda_info", "autograd_cuda_stats", "autograd_cuda_reset_stats", "autograd_cuda_synchronize",
    "autograd_save_safetensors", "autograd_load_safetensors",
    "autograd_save_onnx_weights", "autograd_load_onnx_weights", "autograd_run_onnx",
    "autograd_all_reduce_gradients",
  ];
  if (directAutogradBuiltins.length !== 67) {
    throw new Error(`expected 67 direct autograd built-ins, got ${directAutogradBuiltins.length}`);
  }
  const missingDirectAutogradBuiltins = directAutogradBuiltins.filter((name) => !completionLabels.includes(name));
  if (missingDirectAutogradBuiltins.length > 0) {
    throw new Error(`completion missing direct autograd built-ins: ${missingDirectAutogradBuiltins.join(", ")}`);
  }
  const directTokenizerBuiltins = [
    "tokenizer_byte", "tokenizer_encode", "tokenizer_decode",
    "tokenizer_info", "tokenizer_save", "tokenizer_load",
  ];
  const directDatasetBuiltins = [
    "dataset_pack_text", "dataset_open", "dataset_next",
    "dataset_reset", "dataset_close", "dataset_info",
  ];
  if (directTokenizerBuiltins.length !== 6 || directDatasetBuiltins.length !== 6) {
    throw new Error("expected six direct tokenizer and six direct dataset built-ins");
  }
  const missingDirectTokenizerBuiltins = directTokenizerBuiltins.filter((name) => !completionLabels.includes(name));
  const missingDirectDatasetBuiltins = directDatasetBuiltins.filter((name) => !completionLabels.includes(name));
  if (missingDirectTokenizerBuiltins.length > 0 || missingDirectDatasetBuiltins.length > 0) {
    throw new Error(
      `completion missing tokenizer/dataset built-ins: ${[...missingDirectTokenizerBuiltins, ...missingDirectDatasetBuiltins].join(", ")}`,
    );
  }

  const moduleUri = "file:///C:/tmp/sura_lsp_modules.sura";
  const moduleText = [
    "use cli",
    "use json",
    "use fs",
    "use crypto",
    "use llm",
    "use vector",
    "use graphics3d",
    "use python",
    "use ffi",
    "use plugin",
    'parsed is cli.parse("--fast")',
    'ok is json.path({ok: true}, "ok")',
    'contents is fs.read("note.txt")',
    'digest is crypto.sha256("abc")',
    'body is llm.request_schema_json("model", llm.messages("sys", "user"), {answer: "string"}, 0, "answer")',
    "score is vector.cosine([1, 0], [1, 0])",
    "screen is graphics3d.project([0, 0, 0], {position: [0, 0, -5], target: [0, 0, 0]}, 800, 600)",
    "pyok is python.available()",
    "ffi_name is ffi.name",
    "plugin_name is plugin.name",
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: moduleUri, languageId: "sura", version: 1, text: moduleText },
  });
  await waitForDiagnostics(moduleUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for stdlib module doc");

  const cliCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 10, character: "parsed is cli.".length },
  });
  const cliItems = cliCompletion.result && Array.isArray(cliCompletion.result.items) ? cliCompletion.result.items : [];
  const cliLabels = cliItems.map((item) => item.label);
  if (!cliLabels.includes("parse") || !cliLabels.includes("argv") || cliLabels.includes("schema_errors")) {
    throw new Error(`module completion missing cli members or leaked globals: ${JSON.stringify(cliCompletion.result)}`);
  }

  const jsonCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 11, character: "ok is json.".length },
  });
  const jsonItems = jsonCompletion.result && Array.isArray(jsonCompletion.result.items) ? jsonCompletion.result.items : [];
  const jsonLabels = jsonItems.map((item) => item.label);
  if (!jsonLabels.includes("path") || !jsonLabels.includes("stringify") || !jsonLabels.includes("pretty") || !jsonLabels.includes("schema_errors") || !jsonLabels.includes("template_render")) {
    throw new Error(`module completion missing json members: ${JSON.stringify(jsonCompletion.result)}`);
  }

  const fsCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 12, character: "contents is fs.".length },
  });
  const fsItems = fsCompletion.result && Array.isArray(fsCompletion.result.items) ? fsCompletion.result.items : [];
  const fsLabels = fsItems.map((item) => item.label);
  if (!fsLabels.includes("read") || !fsLabels.includes("write") || !fsLabels.includes("read_bytes") || !fsLabels.includes("write_bytes") || !fsLabels.includes("sha256") || !fsLabels.includes("walk") || !fsLabels.includes("glob")) {
    throw new Error(`module completion missing fs members: ${JSON.stringify(fsCompletion.result)}`);
  }

  const cryptoCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 13, character: 'digest is crypto.'.length },
  });
  const cryptoItems = cryptoCompletion.result && Array.isArray(cryptoCompletion.result.items) ? cryptoCompletion.result.items : [];
  const cryptoLabels = cryptoItems.map((item) => item.label);
  if (!cryptoLabels.includes("sha256") || !cryptoLabels.includes("file_sha256") || !cryptoLabels.includes("hmac_sha256") || !cryptoLabels.includes("file_hmac_sha256") || !cryptoLabels.includes("random_bytes") || !cryptoLabels.includes("random_hex") || !cryptoLabels.includes("constant_time_eq")) {
    throw new Error(`module completion missing crypto members: ${JSON.stringify(cryptoCompletion.result)}`);
  }

  const llmCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 14, character: 'body is llm.'.length },
  });
  const llmItems = llmCompletion.result && Array.isArray(llmCompletion.result.items) ? llmCompletion.result.items : [];
  const llmLabels = llmItems.map((item) => item.label);
  if (
    !llmLabels.includes("request_json") ||
    !llmLabels.includes("request_schema_json") ||
    !llmLabels.includes("tools") ||
    !llmLabels.includes("request_tools") ||
    !llmLabels.includes("request_tools_schema_json") ||
    !llmLabels.includes("chat_request") ||
    !llmLabels.includes("extract_json") ||
    !llmLabels.includes("tool_calls") ||
    !llmLabels.includes("tool_result") ||
    !llmLabels.includes("run_tools") ||
    !llmLabels.includes("next_messages") ||
    !llmLabels.includes("next_request") ||
    !llmLabels.includes("next_schema_request") ||
    !llmLabels.includes("messages") ||
    llmLabels.includes("schema_errors")
  ) {
    throw new Error(`module completion missing llm members or leaked globals: ${JSON.stringify(llmCompletion.result)}`);
  }

  const vectorCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 15, character: "score is vector.".length },
  });
  const vectorItems = vectorCompletion.result && Array.isArray(vectorCompletion.result.items) ? vectorCompletion.result.items : [];
  const vectorLabels = vectorItems.map((item) => item.label);
  if (!vectorLabels.includes("cosine") || !vectorLabels.includes("search")) {
    throw new Error(`module completion missing vector members: ${JSON.stringify(vectorCompletion.result)}`);
  }

  const graphicsCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 16, character: "screen is graphics3d.".length },
  });
  const graphicsItems = graphicsCompletion.result && Array.isArray(graphicsCompletion.result.items) ? graphicsCompletion.result.items : [];
  const graphicsLabels = graphicsItems.map((item) => item.label);
  if (!graphicsLabels.includes("cube") || !graphicsLabels.includes("project") || !graphicsLabels.includes("bounds")) {
    throw new Error(`module completion missing graphics3d members: ${JSON.stringify(graphicsCompletion.result)}`);
  }

  const pythonCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 17, character: "pyok is python.".length },
  });
  const pythonItems = pythonCompletion.result && Array.isArray(pythonCompletion.result.items) ? pythonCompletion.result.items : [];
  const pythonLabels = pythonItems.map((item) => item.label);
  if (!pythonLabels.includes("available") || !pythonLabels.includes("call_json") || pythonLabels.includes("schema_errors")) {
    throw new Error(`module completion missing python members or leaked globals: ${JSON.stringify(pythonCompletion.result)}`);
  }

  const ffiCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 18, character: "ffi_name is ffi.".length },
  });
  const ffiItems = ffiCompletion.result && Array.isArray(ffiCompletion.result.items) ? ffiCompletion.result.items : [];
  const ffiLabels = ffiItems.map((item) => item.label);
  if (!ffiLabels.includes("load") || !ffiLabels.includes("call") || ffiLabels.includes("schema_errors")) {
    throw new Error(`module completion missing ffi members or leaked globals: ${JSON.stringify(ffiCompletion.result)}`);
  }

  const pluginCompletion = await request("textDocument/completion", {
    textDocument: { uri: moduleUri },
    position: { line: 19, character: "plugin_name is plugin.".length },
  });
  const pluginItems = pluginCompletion.result && Array.isArray(pluginCompletion.result.items) ? pluginCompletion.result.items : [];
  const pluginLabels = pluginItems.map((item) => item.label);
  if (!pluginLabels.includes("load") || !pluginLabels.includes("load_manifest") || !pluginLabels.includes("call") || pluginLabels.includes("schema_errors")) {
    throw new Error(`module completion missing plugin members or leaked globals: ${JSON.stringify(pluginCompletion.result)}`);
  }

  const nnUri = "file:///C:/tmp/sura_lsp_nn_module.sura";
  const nnText = [
    "use nn",
    'model is nn.mlp([2, 4, 1], {task: "binary"})',
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: nnUri, languageId: "sura", version: 1, text: nnText },
  });
  await waitForDiagnostics(nnUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for nn module doc");
  const nnCompletion = await request("textDocument/completion", {
    textDocument: { uri: nnUri },
    position: { line: 1, character: "model is nn.".length },
  });
  const nnItems = nnCompletion.result && Array.isArray(nnCompletion.result.items) ? nnCompletion.result.items : [];
  const nnLabels = nnItems.map((item) => item.label);
  if (!nnLabels.includes("mlp") || !nnLabels.includes("train") || !nnLabels.includes("predict") ||
      !nnLabels.includes("evaluate") || !nnLabels.includes("fit_standardizer") ||
      !nnLabels.includes("standardize") || !nnLabels.includes("split") ||
      !nnLabels.includes("save") || nnLabels.includes("schema_errors")) {
    throw new Error(`module completion missing nn members or leaked globals: ${JSON.stringify(nnCompletion.result)}`);
  }

  const autogradUri = "file:///C:/tmp/sura_lsp_autograd_module.sura";
  const autogradText = [
    "use autograd",
    "weights is autograd.parameter([[0.1], [0.2]])",
    "prediction is autograd.matmul([[1, 2]], weights)",
    "loss is autograd.mse(prediction, [[1]])",
    "autograd.backward(loss)",
    "autograd.adam([weights], 0.01, {weight_decay: 0.001})",
    "direct_loss is autograd_bce_logits([0], [1])",
    'typed_weights is autograd.cast(weights, "float32")',
    'cuda_weights is autograd.to(typed_weights, "cuda")',
    "device_name is autograd.device(cuda_weights)",
    "cuda_counters is autograd.cuda_stats()",
    "autograd.cuda_reset_stats()",
    'attention is autograd.causal_attention(typed_weights, typed_weights, typed_weights, {precision: "auto"})',
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: autogradUri, languageId: "sura", version: 1, text: autogradText },
  });
  await waitForDiagnostics(autogradUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for autograd module doc");

  const autogradCompletion = await request("textDocument/completion", {
    textDocument: { uri: autogradUri },
    position: { line: 1, character: "weights is autograd.".length },
  });
  const autogradItems = autogradCompletion.result && Array.isArray(autogradCompletion.result.items) ? autogradCompletion.result.items : [];
  const autogradLabels = autogradItems.map((item) => item.label);
  const autogradMethods = directAutogradBuiltins.map((name) => name.slice("autograd_".length));
  const missingAutogradMethods = autogradMethods.filter((name) => !autogradLabels.includes(name));
  if (missingAutogradMethods.length > 0 || autogradLabels.includes("schema_errors")) {
    throw new Error(`module completion missing autograd members or leaked globals: ${missingAutogradMethods.join(", ")}`);
  }

  const autogradHover = await request("textDocument/hover", {
    textDocument: { uri: autogradUri },
    position: { line: 2, character: "prediction is autograd.matm".length },
  });
  const autogradHoverText = autogradHover.result && autogradHover.result.contents && autogradHover.result.contents.value;
  if (!autogradHoverText || !autogradHoverText.includes("autograd.matmul(a, b, [options])")) {
    throw new Error(`module hover missing autograd.matmul signature: ${JSON.stringify(autogradHover.result)}`);
  }

  const autogradSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: autogradUri },
    position: { line: 5, character: "autograd.adam([weights], 0.01, ".length },
  });
  if (
    !autogradSignature.result ||
    !autogradSignature.result.signatures ||
    !autogradSignature.result.signatures[0].label.includes("autograd.adam(parameters, learning_rate, [options])") ||
    autogradSignature.result.activeParameter !== 2
  ) {
    throw new Error(`module signature help missing autograd.adam metadata: ${JSON.stringify(autogradSignature.result)}`);
  }

  const attentionHover = await request("textDocument/hover", {
    textDocument: { uri: autogradUri },
    position: { line: 12, character: "attention is autograd.causal_att".length },
  });
  const attentionHoverText = attentionHover.result && attentionHover.result.contents && attentionHover.result.contents.value;
  if (
    !attentionHoverText ||
    !attentionHoverText.includes("autograd.causal_attention(query, key, value, [options])") ||
    !attentionHoverText.includes("precision auto/fast/strict")
  ) {
    throw new Error(`module hover missing causal-attention precision contract: ${JSON.stringify(attentionHover.result)}`);
  }

  const attentionSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: autogradUri },
    position: { line: 12, character: 'attention is autograd.causal_attention(typed_weights, typed_weights, typed_weights, '.length },
  });
  if (
    !attentionSignature.result ||
    !attentionSignature.result.signatures ||
    !attentionSignature.result.signatures[0].label.includes("autograd.causal_attention(query, key, value, [options])") ||
    attentionSignature.result.activeParameter !== 3
  ) {
    throw new Error(`module signature help missing causal-attention options metadata: ${JSON.stringify(attentionSignature.result)}`);
  }

  const directAutogradHover = await request("textDocument/hover", {
    textDocument: { uri: autogradUri },
    position: { line: 6, character: "direct_loss is autograd_bce_log".length },
  });
  const directAutogradHoverText = directAutogradHover.result && directAutogradHover.result.contents && directAutogradHover.result.contents.value;
  if (!directAutogradHoverText || !directAutogradHoverText.includes("autograd_bce_logits(logits, target)")) {
    throw new Error(`direct hover missing autograd_bce_logits signature: ${JSON.stringify(directAutogradHover.result)}`);
  }

  const autogradCastHover = await request("textDocument/hover", {
    textDocument: { uri: autogradUri },
    position: { line: 7, character: "typed_weights is autograd.cas".length },
  });
  const autogradCastHoverText = autogradCastHover.result && autogradCastHover.result.contents && autogradCastHover.result.contents.value;
  if (!autogradCastHoverText || !autogradCastHoverText.includes("autograd.cast(tensor, dtype)")) {
    throw new Error(`module hover missing autograd.cast signature: ${JSON.stringify(autogradCastHover.result)}`);
  }

  const autogradToHover = await request("textDocument/hover", {
    textDocument: { uri: autogradUri },
    position: { line: 8, character: "cuda_weights is autograd.t".length },
  });
  const autogradToHoverText = autogradToHover.result && autogradToHover.result.contents && autogradToHover.result.contents.value;
  if (!autogradToHoverText || !autogradToHoverText.includes("autograd.to(tensor, device)")) {
    throw new Error(`module hover missing autograd.to signature: ${JSON.stringify(autogradToHover.result)}`);
  }

  const autogradStatsHover = await request("textDocument/hover", {
    textDocument: { uri: autogradUri },
    position: { line: 10, character: "cuda_counters is autograd.cuda_sta".length },
  });
  const autogradStatsHoverText = autogradStatsHover.result && autogradStatsHover.result.contents && autogradStatsHover.result.contents.value;
  if (!autogradStatsHoverText || !autogradStatsHoverText.includes("autograd.cuda_stats()")) {
    throw new Error(`module hover missing autograd.cuda_stats signature: ${JSON.stringify(autogradStatsHover.result)}`);
  }

  const tokenizerUri = "file:///C:/tmp/sura_lsp_tokenizer_module.sura";
  const tokenizerText = [
    "use tokenizer",
    "tok is tokenizer.byte()",
    'ids is tokenizer.encode(tok, "Sura")',
    "decoded is tokenizer.decode(tok, ids)",
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: tokenizerUri, languageId: "sura", version: 1, text: tokenizerText },
  });
  await waitForDiagnostics(tokenizerUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for tokenizer module doc");
  const tokenizerCompletion = await request("textDocument/completion", {
    textDocument: { uri: tokenizerUri },
    position: { line: 1, character: "tok is tokenizer.".length },
  });
  const tokenizerItems = tokenizerCompletion.result && Array.isArray(tokenizerCompletion.result.items) ? tokenizerCompletion.result.items : [];
  const tokenizerLabels = tokenizerItems.map((item) => item.label);
  const tokenizerMethods = directTokenizerBuiltins.map((name) => name.slice("tokenizer_".length));
  const missingTokenizerMethods = tokenizerMethods.filter((name) => !tokenizerLabels.includes(name));
  if (tokenizerMethods.length !== 6 || missingTokenizerMethods.length > 0 || tokenizerLabels.includes("schema_errors")) {
    throw new Error(`module completion missing tokenizer members: ${missingTokenizerMethods.join(", ")}`);
  }
  const tokenizerHover = await request("textDocument/hover", {
    textDocument: { uri: tokenizerUri },
    position: { line: 2, character: "ids is tokenizer.enc".length },
  });
  const tokenizerHoverText = tokenizerHover.result && tokenizerHover.result.contents && tokenizerHover.result.contents.value;
  if (!tokenizerHoverText || !tokenizerHoverText.includes("tokenizer.encode(tokenizer, text, [options])")) {
    throw new Error(`module hover missing tokenizer.encode signature: ${JSON.stringify(tokenizerHover.result)}`);
  }

  const datasetUri = "file:///C:/tmp/sura_lsp_dataset_module.sura";
  const datasetText = [
    "use dataset",
    'loader is dataset.open("train.suradata")',
    "batch is dataset.next(loader)",
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: datasetUri, languageId: "sura", version: 1, text: datasetText },
  });
  await waitForDiagnostics(datasetUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for dataset module doc");
  const datasetCompletion = await request("textDocument/completion", {
    textDocument: { uri: datasetUri },
    position: { line: 1, character: "loader is dataset.".length },
  });
  const datasetItems = datasetCompletion.result && Array.isArray(datasetCompletion.result.items) ? datasetCompletion.result.items : [];
  const datasetLabels = datasetItems.map((item) => item.label);
  const datasetMethods = directDatasetBuiltins.map((name) => name.slice("dataset_".length));
  const missingDatasetMethods = datasetMethods.filter((name) => !datasetLabels.includes(name));
  if (datasetMethods.length !== 6 || missingDatasetMethods.length > 0 || datasetLabels.includes("schema_errors")) {
    throw new Error(`module completion missing dataset members: ${missingDatasetMethods.join(", ")}`);
  }
  const datasetHover = await request("textDocument/hover", {
    textDocument: { uri: datasetUri },
    position: { line: 1, character: "loader is dataset.op".length },
  });
  const datasetHoverText = datasetHover.result && datasetHover.result.contents && datasetHover.result.contents.value;
  if (!datasetHoverText || !datasetHoverText.includes("dataset.open(path, [options])")) {
    throw new Error(`module hover missing dataset.open signature: ${JSON.stringify(datasetHover.result)}`);
  }

  const regexUri = "file:///C:/tmp/sura_lsp_regex_module.sura";
  const regexText = [
    "use regex",
    'escaped is regex.escape("a+b?")',
    'parts is regex.capture("user=kim score=42", "user=([A-Za-z]+) score=([0-9]+)")',
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: regexUri, languageId: "sura", version: 1, text: regexText },
  });
  await waitForDiagnostics(regexUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for regex module doc");

  const regexCompletion = await request("textDocument/completion", {
    textDocument: { uri: regexUri },
    position: { line: 1, character: 'escaped is regex.'.length },
  });
  const regexItems = regexCompletion.result && Array.isArray(regexCompletion.result.items) ? regexCompletion.result.items : [];
  const regexLabels = regexItems.map((item) => item.label);
  if (!regexLabels.includes("escape") || !regexLabels.includes("capture") || !regexLabels.includes("captures") || !regexLabels.includes("find_all") || regexLabels.includes("schema_errors")) {
    throw new Error(`module completion missing regex members or leaked globals: ${JSON.stringify(regexCompletion.result)}`);
  }

  const regexHover = await request("textDocument/hover", {
    textDocument: { uri: regexUri },
    position: { line: 2, character: 'parts is regex.capt'.length },
  });
  const regexHoverText = regexHover.result && regexHover.result.contents && regexHover.result.contents.value;
  if (!regexHoverText || !regexHoverText.includes("regex.capture(text, pattern)")) {
    throw new Error(`module hover missing regex.capture signature: ${JSON.stringify(regexHover.result)}`);
  }

  const regexSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: regexUri },
    position: { line: 2, character: 'parts is regex.capture("user=kim score=42", '.length },
  });
  if (
    !regexSignature.result ||
    !regexSignature.result.signatures ||
    !regexSignature.result.signatures[0].label.includes("regex.capture(text, pattern)") ||
    regexSignature.result.activeParameter !== 1
  ) {
    throw new Error(`module signature help missing regex.capture metadata: ${JSON.stringify(regexSignature.result)}`);
  }

  const dbUri = "file:///C:/tmp/sura_lsp_db_module.sura";
  const dbText = [
    "use db",
    'rows is db.query("rows.json", {}, {sort: "score", desc: true, limit: 1})',
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: dbUri, languageId: "sura", version: 1, text: dbText },
  });
  await waitForDiagnostics(dbUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for db module doc");

  const dbCompletion = await request("textDocument/completion", {
    textDocument: { uri: dbUri },
    position: { line: 1, character: 'rows is db.'.length },
  });
  const dbItems = dbCompletion.result && Array.isArray(dbCompletion.result.items) ? dbCompletion.result.items : [];
  const dbLabels = dbItems.map((item) => item.label);
  if (!dbLabels.includes("query") || !dbLabels.includes("find") || !dbLabels.includes("insert") || dbLabels.includes("schema_errors")) {
    throw new Error(`module completion missing db members or leaked globals: ${JSON.stringify(dbCompletion.result)}`);
  }

  const dbHover = await request("textDocument/hover", {
    textDocument: { uri: dbUri },
    position: { line: 1, character: 'rows is db.que'.length },
  });
  const dbHoverText = dbHover.result && dbHover.result.contents && dbHover.result.contents.value;
  if (!dbHoverText || !dbHoverText.includes("db.query(path, [criteria], [options])")) {
    throw new Error(`module hover missing db.query signature: ${JSON.stringify(dbHover.result)}`);
  }

  const dbSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: dbUri },
    position: { line: 1, character: 'rows is db.query("rows.json", '.length },
  });
  if (
    !dbSignature.result ||
    !dbSignature.result.signatures ||
    !dbSignature.result.signatures[0].label.includes("db.query(path, [criteria], [options])") ||
    dbSignature.result.activeParameter !== 1
  ) {
    throw new Error(`module signature help missing db.query metadata: ${JSON.stringify(dbSignature.result)}`);
  }

  const testUri = "file:///C:/tmp/sura_lsp_test_module.sura";
  const testText = [
    "use test",
    "ok is test.approx(0.1 + 0.2, 0.3, 0.000001)",
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: testUri, languageId: "sura", version: 1, text: testText },
  });
  await waitForDiagnostics(testUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for test module doc");

  const testCompletion = await request("textDocument/completion", {
    textDocument: { uri: testUri },
    position: { line: 1, character: "ok is test.".length },
  });
  const testItems = testCompletion.result && Array.isArray(testCompletion.result.items) ? testCompletion.result.items : [];
  const testLabels = testItems.map((item) => item.label);
  if (!testLabels.includes("approx") || !testLabels.includes("not_contains") || !testLabels.includes("check_match") || testLabels.includes("schema_errors")) {
    throw new Error(`module completion missing test assertion members or leaked globals: ${JSON.stringify(testCompletion.result)}`);
  }

  const testHover = await request("textDocument/hover", {
    textDocument: { uri: testUri },
    position: { line: 1, character: "ok is test.app".length },
  });
  if (!testHover.result || !JSON.stringify(testHover.result.contents).includes("test.approx(actual, expected, [epsilon], [message])")) {
    throw new Error(`module hover missing test.approx signature: ${JSON.stringify(testHover.result)}`);
  }

  const testSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: testUri },
    position: { line: 1, character: "ok is test.approx(".length },
  });
  if (
    !testSignature.result ||
    !testSignature.result.signatures ||
    !testSignature.result.signatures[0].label.includes("test.approx(actual, expected, [epsilon], [message])")
  ) {
    throw new Error(`module signature help missing test.approx metadata: ${JSON.stringify(testSignature.result)}`);
  }

  const httpUri = "file:///C:/tmp/sura_lsp_http.sura";
  const httpText = [
    "use http",
    'data is http.request_json_checked({url: "file:///tmp/data.json"})',
    'retryData is http.request_retry_json_checked({url: "file:///tmp/data.json"}, 2, 10)',
    'headers is http.headers_merge(http.auth_bearer("token"), {"X-Agent": "sura"})',
    'cookies is http.cookie_parse("session=abc%20123")',
    'formData is http.form_parse("q=sura+agent")',
    'safeHeaders is http.headers_redact({"Authorization": "Bearer x"})',
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: httpUri, languageId: "sura", version: 1, text: httpText },
  });
  await waitForDiagnostics(httpUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for http module doc");

  const httpCompletion = await request("textDocument/completion", {
    textDocument: { uri: httpUri },
    position: { line: 1, character: "data is http.".length },
  });
  const httpItems = httpCompletion.result && Array.isArray(httpCompletion.result.items) ? httpCompletion.result.items : [];
  const httpLabels = httpItems.map((item) => item.label);
  if (!httpLabels.includes("request_json_checked") || !httpLabels.includes("request_retry_json") || !httpLabels.includes("request_retry_json_checked") || !httpLabels.includes("url_parse") || !httpLabels.includes("url_build") || !httpLabels.includes("status_ok") || !httpLabels.includes("status_text") || !httpLabels.includes("status_retryable") || !httpLabels.includes("retry_after") || !httpLabels.includes("backoff_delays") || !httpLabels.includes("auth_bearer") || !httpLabels.includes("auth_basic") || !httpLabels.includes("headers_merge") || !httpLabels.includes("headers_get") || !httpLabels.includes("headers_has") || !httpLabels.includes("headers_redact") || !httpLabels.includes("cookie_parse") || !httpLabels.includes("cookie_build") || !httpLabels.includes("cookie_get") || !httpLabels.includes("form_build") || !httpLabels.includes("form_parse") || !httpLabels.includes("content_type") || !httpLabels.includes("charset") || !httpLabels.includes("is_json") || httpLabels.includes("schema_errors")) {
    throw new Error(`module completion missing http members or leaked globals: ${JSON.stringify(httpCompletion.result)}`);
  }

  const httpHover = await request("textDocument/hover", {
    textDocument: { uri: httpUri },
    position: { line: 1, character: "data is http.request_json_ch".length },
  });
  const httpHoverText = httpHover.result && httpHover.result.contents && httpHover.result.contents.value;
  if (!httpHoverText || !httpHoverText.includes("http.request_json_checked(spec)")) {
    throw new Error(`module hover missing http.request_json_checked signature: ${JSON.stringify(httpHover.result)}`);
  }

  const httpCookieHover = await request("textDocument/hover", {
    textDocument: { uri: httpUri },
    position: { line: 4, character: "cookies is http.cookie_par".length },
  });
  const httpCookieHoverText = httpCookieHover.result && httpCookieHover.result.contents && httpCookieHover.result.contents.value;
  if (!httpCookieHoverText || !httpCookieHoverText.includes("http.cookie_parse(header_or_headers)")) {
    throw new Error(`module hover missing http.cookie_parse signature: ${JSON.stringify(httpCookieHover.result)}`);
  }

  const httpFormHover = await request("textDocument/hover", {
    textDocument: { uri: httpUri },
    position: { line: 5, character: "formData is http.form_par".length },
  });
  const httpFormHoverText = httpFormHover.result && httpFormHover.result.contents && httpFormHover.result.contents.value;
  if (!httpFormHoverText || !httpFormHoverText.includes("http.form_parse(body)")) {
    throw new Error(`module hover missing http.form_parse signature: ${JSON.stringify(httpFormHover.result)}`);
  }

  const httpHeadersRedactHover = await request("textDocument/hover", {
    textDocument: { uri: httpUri },
    position: { line: 6, character: "safeHeaders is http.headers_red".length },
  });
  const httpHeadersRedactHoverText = httpHeadersRedactHover.result && httpHeadersRedactHover.result.contents && httpHeadersRedactHover.result.contents.value;
  if (!httpHeadersRedactHoverText || !httpHeadersRedactHoverText.includes("http.headers_redact(headers, [names], [mask])")) {
    throw new Error(`module hover missing http.headers_redact signature: ${JSON.stringify(httpHeadersRedactHover.result)}`);
  }

  const asyncUri = "file:///C:/tmp/sura_lsp_async.sura";
  const asyncText = [
    "use async",
    "timer is async.sleep(25)",
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: asyncUri, languageId: "sura", version: 1, text: asyncText },
  });
  await waitForDiagnostics(asyncUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for async module doc");

  const asyncCompletion = await request("textDocument/completion", {
    textDocument: { uri: asyncUri },
    position: { line: 1, character: "timer is async.".length },
  });
  const asyncItems = asyncCompletion.result && Array.isArray(asyncCompletion.result.items) ? asyncCompletion.result.items : [];
  const asyncLabels = asyncItems.map((item) => item.label);
  if (!asyncLabels.includes("sleep") || !asyncLabels.includes("await_timeout") ||
      !asyncLabels.includes("cancel") || !asyncLabels.includes("scope_close") ||
      !asyncLabels.includes("limits") || asyncLabels.includes("schema_errors")) {
    throw new Error(`module completion missing async members or leaked globals: ${JSON.stringify(asyncCompletion.result)}`);
  }

  const asyncHover = await request("textDocument/hover", {
    textDocument: { uri: asyncUri },
    position: { line: 1, character: "timer is async.sle".length },
  });
  const asyncHoverText = asyncHover.result && asyncHover.result.contents && asyncHover.result.contents.value;
  if (!asyncHoverText || !asyncHoverText.includes("async.sleep(milliseconds, [scope_id])")) {
    throw new Error(`module hover missing async.sleep signature: ${JSON.stringify(asyncHover.result)}`);
  }

  const coreUri = "file:///C:/tmp/sura_lsp_core_modules.sura";
  const coreText = [
    "use array",
    "use math",
    "use path",
    "use string",
    "use os",
    "part is array.slice([1, 2, 3], 1)",
    "rounded is math.floor(math.pi)",
    'joined is path.join("dir", "file.sura")',
    'piece is string.chunks("abcdef", 2)[0]',
    'home is os.env_get("HOME", "")',
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: coreUri, languageId: "sura", version: 1, text: coreText },
  });
  await waitForDiagnostics(coreUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for core module doc");

  const arrayCompletion = await request("textDocument/completion", {
    textDocument: { uri: coreUri },
    position: { line: 5, character: "part is array.".length },
  });
  const arrayItems = arrayCompletion.result && Array.isArray(arrayCompletion.result.items) ? arrayCompletion.result.items : [];
  const arrayLabels = arrayItems.map((item) => item.label);
  if (!arrayLabels.includes("slice") || !arrayLabels.includes("push") || !arrayLabels.includes("index_of") || !arrayLabels.includes("length") || !arrayLabels.includes("size")) {
    throw new Error(`module completion missing array members: ${JSON.stringify(arrayCompletion.result)}`);
  }

  const mathCompletion = await request("textDocument/completion", {
    textDocument: { uri: coreUri },
    position: { line: 6, character: "rounded is math.".length },
  });
  const mathItems = mathCompletion.result && Array.isArray(mathCompletion.result.items) ? mathCompletion.result.items : [];
  const mathLabels = mathItems.map((item) => item.label);
  if (!mathLabels.includes("floor") || !mathLabels.includes("pow") || !mathLabels.includes("clamp")) {
    throw new Error(`module completion missing math members: ${JSON.stringify(mathCompletion.result)}`);
  }

  const pathCompletion = await request("textDocument/completion", {
    textDocument: { uri: coreUri },
    position: { line: 7, character: 'joined is path.'.length },
  });
  const pathItems = pathCompletion.result && Array.isArray(pathCompletion.result.items) ? pathCompletion.result.items : [];
  const pathLabels = pathItems.map((item) => item.label);
  if (!pathLabels.includes("join") || !pathLabels.includes("basename") || !pathLabels.includes("relative")) {
    throw new Error(`module completion missing path members: ${JSON.stringify(pathCompletion.result)}`);
  }

  const stringCompletion = await request("textDocument/completion", {
    textDocument: { uri: coreUri },
    position: { line: 8, character: 'piece is string.'.length },
  });
  const stringItems = stringCompletion.result && Array.isArray(stringCompletion.result.items) ? stringCompletion.result.items : [];
  const stringLabels = stringItems.map((item) => item.label);
  if (!stringLabels.includes("chunks") || !stringLabels.includes("lines") || !stringLabels.includes("pad_left") || !stringLabels.includes("starts_with") || !stringLabels.includes("upper") || !stringLabels.includes("length") || !stringLabels.includes("size")) {
    throw new Error(`module completion missing string members: ${JSON.stringify(stringCompletion.result)}`);
  }

  const osCompletion = await request("textDocument/completion", {
    textDocument: { uri: coreUri },
    position: { line: 9, character: 'home is os.'.length },
  });
  const osItems = osCompletion.result && Array.isArray(osCompletion.result.items) ? osCompletion.result.items : [];
  const osLabels = osItems.map((item) => item.label);
  if (!osLabels.includes("env_get") || !osLabels.includes("env_load") || !osLabels.includes("cwd") || !osLabels.includes("wait") || !osLabels.includes("home_dir") || !osLabels.includes("temp_dir") || !osLabels.includes("path_separator") || !osLabels.includes("name") || !osLabels.includes("is_windows") || !osLabels.includes("which") || !osLabels.includes("cmd_exists") || !osLabels.includes("cmd_quote") || !osLabels.includes("cmd_join") || !osLabels.includes("run") || !osLabels.includes("run_checked")) {
    throw new Error(`module completion missing os members: ${JSON.stringify(osCompletion.result)}`);
  }

  const mathHover = await request("textDocument/hover", {
    textDocument: { uri: coreUri },
    position: { line: 6, character: "rounded is math.flo".length },
  });
  const mathHoverText = mathHover.result && mathHover.result.contents && mathHover.result.contents.value;
  if (!mathHoverText || !mathHoverText.includes("math.floor(value)")) {
    throw new Error(`module hover missing math.floor signature: ${JSON.stringify(mathHover.result)}`);
  }

  const randomUri = "file:///C:/tmp/sura_lsp_random.sura";
  const randomText = [
    "use random",
    "roll is random.int(1, 6)",
    "pick is random.choice([1, 2, 3])",
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: randomUri, languageId: "sura", version: 1, text: randomText },
  });
  await waitForDiagnostics(randomUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for random module doc");

  const randomCompletion = await request("textDocument/completion", {
    textDocument: { uri: randomUri },
    position: { line: 1, character: "roll is random.".length },
  });
  const randomItems = randomCompletion.result && Array.isArray(randomCompletion.result.items) ? randomCompletion.result.items : [];
  const randomLabels = randomItems.map((item) => item.label);
  if (!randomLabels.includes("seed") || !randomLabels.includes("int") || !randomLabels.includes("choice") || randomLabels.includes("schema_errors")) {
    throw new Error(`module completion missing random members or leaked globals: ${JSON.stringify(randomCompletion.result)}`);
  }

  const randomHover = await request("textDocument/hover", {
    textDocument: { uri: randomUri },
    position: { line: 1, character: "roll is random.in".length },
  });
  const randomHoverText = randomHover.result && randomHover.result.contents && randomHover.result.contents.value;
  if (!randomHoverText || !randomHoverText.includes("random.int(max)")) {
    throw new Error(`module hover missing random.int signature: ${JSON.stringify(randomHover.result)}`);
  }

  const logUri = "file:///C:/tmp/sura_lsp_log.sura";
  const logText = [
    "use log",
    'current_level is log.level("WARN")',
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: logUri, languageId: "sura", version: 1, text: logText },
  });
  await waitForDiagnostics(logUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for log module doc");

  const logCompletion = await request("textDocument/completion", {
    textDocument: { uri: logUri },
    position: { line: 1, character: 'current_level is log.'.length },
  });
  const logItems = logCompletion.result && Array.isArray(logCompletion.result.items) ? logCompletion.result.items : [];
  const logLabels = logItems.map((item) => item.label);
  if (!logLabels.includes("set_level") || !logLabels.includes("get_level") || !logLabels.includes("level") || !logLabels.includes("event") || logLabels.includes("schema_errors")) {
    throw new Error(`module completion missing log members or leaked globals: ${JSON.stringify(logCompletion.result)}`);
  }

  const logHover = await request("textDocument/hover", {
    textDocument: { uri: logUri },
    position: { line: 1, character: "current_level is log.lev".length },
  });
  const logHoverText = logHover.result && logHover.result.contents && logHover.result.contents.value;
  if (!logHoverText || !logHoverText.includes("log.level([level])")) {
    throw new Error(`module hover missing log.level signature: ${JSON.stringify(logHover.result)}`);
  }

  const logSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: logUri },
    position: { line: 1, character: 'current_level is log.level("'.length },
  });
  if (
    !logSignature.result ||
    !logSignature.result.signatures ||
    !logSignature.result.signatures[0].label.includes("log.level([level])") ||
    logSignature.result.activeParameter !== 0
  ) {
    throw new Error(`module signature help missing log.level metadata: ${JSON.stringify(logSignature.result)}`);
  }

  const consoleUri = "file:///C:/tmp/sura_lsp_console.sura";
  const consoleText = [
    "use console",
    'console.time("phase")',
    'elapsed is console.timeLog("phase", "checkpoint")',
    'console.groupCollapsed("boot")',
    "console.groupEnd()",
    "",
  ].join("\n");
  notify("textDocument/didOpen", {
    textDocument: { uri: consoleUri, languageId: "sura", version: 1, text: consoleText },
  });
  await waitForDiagnostics(consoleUri, (diagnostics) => diagnostics.length === 0, "clean diagnostics for console module doc");

  const consoleCompletion = await request("textDocument/completion", {
    textDocument: { uri: consoleUri },
    position: { line: 2, character: "elapsed is console.".length },
  });
  const consoleItems = consoleCompletion.result && Array.isArray(consoleCompletion.result.items) ? consoleCompletion.result.items : [];
  const consoleLabels = consoleItems.map((item) => item.label);
  if (
    !consoleLabels.includes("timeLog") ||
    !consoleLabels.includes("timeStamp") ||
    !consoleLabels.includes("countReset") ||
    !consoleLabels.includes("table") ||
    !consoleLabels.includes("dirxml") ||
    !consoleLabels.includes("groupCollapsed") ||
    !consoleLabels.includes("profileEnd") ||
    !consoleLabels.includes("stripAnsi") ||
    !consoleLabels.includes("setColor") ||
    !consoleLabels.includes("resetColor") ||
    !consoleLabels.includes("isTTY") ||
    !consoleLabels.includes("readLine") ||
    consoleLabels.includes("schema_errors")
  ) {
    throw new Error(`module completion missing console members or leaked globals: ${JSON.stringify(consoleCompletion.result)}`);
  }

  const consoleHover = await request("textDocument/hover", {
    textDocument: { uri: consoleUri },
    position: { line: 2, character: "elapsed is console.timeL".length },
  });
  const consoleHoverText = consoleHover.result && consoleHover.result.contents && consoleHover.result.contents.value;
  if (!consoleHoverText || !consoleHoverText.includes("console.timeLog([label], [message...])")) {
    throw new Error(`module hover missing console.timeLog signature: ${JSON.stringify(consoleHover.result)}`);
  }

  const consoleSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: consoleUri },
    position: { line: 2, character: 'elapsed is console.timeLog("'.length },
  });
  if (
    !consoleSignature.result ||
    !consoleSignature.result.signatures ||
    !consoleSignature.result.signatures[0].label.includes("console.timeLog([label], [message...])") ||
    consoleSignature.result.activeParameter !== 0
  ) {
    throw new Error(`module signature help missing console.timeLog metadata: ${JSON.stringify(consoleSignature.result)}`);
  }

  const cliHover = await request("textDocument/hover", {
    textDocument: { uri: moduleUri },
    position: { line: 10, character: "parsed is cli.par".length },
  });
  const cliHoverText = cliHover.result && cliHover.result.contents && cliHover.result.contents.value;
  if (!cliHoverText || !cliHoverText.includes("cli.parse(text, [value_flags])")) {
    throw new Error(`module hover missing cli.parse signature: ${JSON.stringify(cliHover.result)}`);
  }

  const jsonSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: moduleUri },
    position: { line: 11, character: 'ok is json.path({ok: true}, "'.length },
  });
  if (
    !jsonSignature.result ||
    !jsonSignature.result.signatures ||
    !jsonSignature.result.signatures[0].label.includes("json.path(value, path, [default])") ||
    jsonSignature.result.activeParameter !== 1
  ) {
    throw new Error(`module signature help missing json.path metadata: ${JSON.stringify(jsonSignature.result)}`);
  }

  const llmSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: moduleUri },
    position: { line: 14, character: 'body is llm.request_schema_json("model", '.length },
  });
  if (
    !llmSignature.result ||
    !llmSignature.result.signatures ||
    !llmSignature.result.signatures[0].label.includes("llm.request_schema_json(model, messages, schema, [temperature], [name], [strict])") ||
    llmSignature.result.activeParameter !== 1
  ) {
    throw new Error(`module signature help missing llm.request_schema_json metadata: ${JSON.stringify(llmSignature.result)}`);
  }

  const workspace = await request("workspace/symbol", { query: "score" });
  const names = workspace.result.map((symbol) => symbol.name).sort();
  if (!names.includes("shared_score") || !names.includes("AgentScore") || !names.includes("disk_score")) {
    throw new Error(`workspace symbols missing: ${JSON.stringify(names)}`);
  }
  if (names.includes("ignored_score")) {
    throw new Error(`workspace index did not skip node_modules: ${JSON.stringify(names)}`);
  }

  const constantDocumentSymbols = await request("textDocument/documentSymbol", {
    textDocument: { uri: constantsUri },
  });
  const buildLimitSymbol = constantDocumentSymbols.result.find((symbol) => symbol.name === "BUILD_LIMIT");
  if (!buildLimitSymbol || buildLimitSymbol.kind !== 14) {
    throw new Error(`constant document symbol missing: ${JSON.stringify(constantDocumentSymbols.result)}`);
  }
  if (constantDocumentSymbols.result.some((symbol) => symbol.name === "local_constant")) {
    throw new Error(`local constant leaked into document symbols: ${JSON.stringify(constantDocumentSymbols.result)}`);
  }

  const constantWorkspace = await request("workspace/symbol", { query: "build" });
  const constantWorkspaceNames = constantWorkspace.result.map((symbol) => symbol.name).sort();
  if (!constantWorkspaceNames.includes("BUILD_LIMIT")) {
    throw new Error(`constant workspace symbol missing: ${JSON.stringify(constantWorkspaceNames)}`);
  }
  const localConstantWorkspace = await request("workspace/symbol", { query: "constant" });
  if (localConstantWorkspace.result.some((symbol) => symbol.name === "local_constant")) {
    throw new Error(`local constant leaked into workspace symbols: ${JSON.stringify(localConstantWorkspace.result)}`);
  }

  const semantic = await request("textDocument/semanticTokens/full", {
    textDocument: { uri: uriA },
  });
  if (
    !semantic.result ||
    !Array.isArray(semantic.result.data) ||
    semantic.result.data.length === 0 ||
    semantic.result.data.length % 5 !== 0
  ) {
    throw new Error(`bad semantic tokens: ${JSON.stringify(semantic.result)}`);
  }

  const userHover = await request("textDocument/hover", {
    textDocument: { uri: uriA },
    position: { line: 0, character: 6 },
  });
  const userHoverText = userHover.result && userHover.result.contents && userHover.result.contents.value;
  if (!userHoverText || !userHoverText.includes("shared_score(x)")) {
    throw new Error(`bad user hover: ${JSON.stringify(userHover.result)}`);
  }

  const builtinHover = await request("textDocument/hover", {
    textDocument: { uri: uriB },
    position: { line: httpPostLine, character: textBLines[httpPostLine].indexOf("http_post") + 2 },
  });
  const builtinHoverText = builtinHover.result && builtinHover.result.contents && builtinHover.result.contents.value;
  if (!builtinHoverText || !builtinHoverText.includes("http_post(url, body, [content_type])")) {
    throw new Error(`bad builtin hover: ${JSON.stringify(builtinHover.result)}`);
  }

  const userSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: uriB },
    position: { line: 0, character: textBLines[0].indexOf("41") + 2 },
  });
  if (
    !userSignature.result ||
    userSignature.result.activeParameter !== 0 ||
    !userSignature.result.signatures[0].label.includes("shared_score(x)")
  ) {
    throw new Error(`bad user signature help: ${JSON.stringify(userSignature.result)}`);
  }

  const builtinSignature = await request("textDocument/signatureHelp", {
    textDocument: { uri: uriB },
    position: { line: httpPostLine, character: textBLines[httpPostLine].lastIndexOf('"text/plain"') + 2 },
  });
  if (
    !builtinSignature.result ||
    builtinSignature.result.activeParameter !== 2 ||
    !builtinSignature.result.signatures[0].label.includes("http_post(url, body, [content_type])")
  ) {
    throw new Error(`bad builtin signature help: ${JSON.stringify(builtinSignature.result)}`);
  }

  const httpPostActions = await request("textDocument/codeAction", {
    textDocument: { uri: uriB },
    range: { start: { line: httpPostLine, character: 0 }, end: { line: httpPostLine, character: textBLines[httpPostLine].length } },
    context: { diagnostics: [] },
  });
  const convertHttpPost = Array.isArray(httpPostActions.result)
    ? httpPostActions.result.find((action) => action.title === "Convert http_post to http_request")
    : null;
  const convertEdit =
    convertHttpPost &&
    convertHttpPost.edit &&
    convertHttpPost.edit.changes &&
    convertHttpPost.edit.changes[uriB] &&
    convertHttpPost.edit.changes[uriB][0];
  if (
    !convertEdit ||
    !convertEdit.newText.includes('http_request({method: "POST"') ||
    !convertEdit.newText.includes('url: "https://example.com"') ||
    !convertEdit.newText.includes('body: "body"') ||
    !convertEdit.newText.includes('content_type: "text/plain"')
  ) {
    throw new Error(`missing http_post conversion action: ${JSON.stringify(httpPostActions.result)}`);
  }

  const httpGetActions = await request("textDocument/codeAction", {
    textDocument: { uri: uriB },
    range: { start: { line: httpGetLine, character: 0 }, end: { line: httpGetLine, character: textBLines[httpGetLine].length } },
    context: { diagnostics: [] },
  });
  const convertHttpGet = Array.isArray(httpGetActions.result)
    ? httpGetActions.result.find((action) => action.title === "Convert http_get to http_request")
    : null;
  const convertGetEdit =
    convertHttpGet &&
    convertHttpGet.edit &&
    convertHttpGet.edit.changes &&
    convertHttpGet.edit.changes[uriB] &&
    convertHttpGet.edit.changes[uriB][0];
  if (
    !convertGetEdit ||
    !convertGetEdit.newText.includes('http_request({method: "GET"') ||
    !convertGetEdit.newText.includes('url: "https://example.com/page"')
  ) {
    throw new Error(`missing http_get conversion action: ${JSON.stringify(httpGetActions.result)}`);
  }

  const httpJsonActions = await request("textDocument/codeAction", {
    textDocument: { uri: uriB },
    range: { start: { line: httpJsonLine, character: 0 }, end: { line: httpJsonLine, character: textBLines[httpJsonLine].length } },
    context: { diagnostics: [] },
  });
  const convertHttpJson = Array.isArray(httpJsonActions.result)
    ? httpJsonActions.result.find((action) => action.title === "Convert http_json to http_request_json")
    : null;
  const convertJsonEdit =
    convertHttpJson &&
    convertHttpJson.edit &&
    convertHttpJson.edit.changes &&
    convertHttpJson.edit.changes[uriB] &&
    convertHttpJson.edit.changes[uriB][0];
  if (
    !convertJsonEdit ||
    !convertJsonEdit.newText.includes('http_request_json({method: "GET"') ||
    !convertJsonEdit.newText.includes('url: "https://example.com/data.json"')
  ) {
    throw new Error(`missing http_json conversion action: ${JSON.stringify(httpJsonActions.result)}`);
  }

  const asyncHttpGetActions = await request("textDocument/codeAction", {
    textDocument: { uri: uriB },
    range: { start: { line: asyncHttpGetLine, character: 0 }, end: { line: asyncHttpGetLine, character: textBLines[asyncHttpGetLine].length } },
    context: { diagnostics: [] },
  });
  const convertAsyncHttpGet = Array.isArray(asyncHttpGetActions.result)
    ? asyncHttpGetActions.result.find((action) => action.title === "Convert async_http_get to async.http_get")
    : null;
  const convertAsyncGetEdit =
    convertAsyncHttpGet &&
    convertAsyncHttpGet.edit &&
    convertAsyncHttpGet.edit.changes &&
    convertAsyncHttpGet.edit.changes[uriB] &&
    convertAsyncHttpGet.edit.changes[uriB][0];
  if (
    !convertAsyncGetEdit ||
    !convertAsyncGetEdit.newText.includes('async.http_get("https://example.com/status")')
  ) {
    throw new Error(`missing async_http_get conversion action: ${JSON.stringify(asyncHttpGetActions.result)}`);
  }

  const asyncHttpRequestActions = await request("textDocument/codeAction", {
    textDocument: { uri: uriB },
    range: { start: { line: asyncHttpRequestLine, character: 0 }, end: { line: asyncHttpRequestLine, character: textBLines[asyncHttpRequestLine].length } },
    context: { diagnostics: [] },
  });
  const convertAsyncHttpRequest = Array.isArray(asyncHttpRequestActions.result)
    ? asyncHttpRequestActions.result.find((action) => action.title === "Convert async_http_request to async.http_request")
    : null;
  const convertAsyncRequestEdit =
    convertAsyncHttpRequest &&
    convertAsyncHttpRequest.edit &&
    convertAsyncHttpRequest.edit.changes &&
    convertAsyncHttpRequest.edit.changes[uriB] &&
    convertAsyncHttpRequest.edit.changes[uriB][0];
  if (
    !convertAsyncRequestEdit ||
    !convertAsyncRequestEdit.newText.includes('async.http_request({url: "https://example.com/status", timeout: 10})')
  ) {
    throw new Error(`missing async_http_request conversion action: ${JSON.stringify(asyncHttpRequestActions.result)}`);
  }

  async function assertCallNameConversion(line, title, expectedName) {
    const actions = await request("textDocument/codeAction", {
      textDocument: { uri: uriB },
      range: { start: { line, character: 0 }, end: { line, character: textBLines[line].length } },
      context: { diagnostics: [] },
    });
    const action = Array.isArray(actions.result)
      ? actions.result.find((candidate) => candidate.title === title)
      : null;
    const edit =
      action &&
      action.edit &&
      action.edit.changes &&
      action.edit.changes[uriB] &&
      action.edit.changes[uriB][0];
    if (!edit || edit.newText !== expectedName) {
      throw new Error(`missing ${title} action: ${JSON.stringify(actions.result)}`);
    }
  }
  await assertCallNameConversion(pythonCallLine, "Convert python_call to python.call", "python.call");
  await assertCallNameConversion(ffiLoadLine, "Convert ffi_load to ffi.load", "ffi.load");
  await assertCallNameConversion(ffiCallLine, "Convert ffi_call to ffi.call", "ffi.call");
  await assertCallNameConversion(pluginLoadManifestLine, "Convert plugin_load_manifest to plugin.load_manifest", "plugin.load_manifest");
  await assertCallNameConversion(pluginCallLine, "Convert plugin_call to plugin.call", "plugin.call");

  const sensitiveHeaderActions = await request("textDocument/codeAction", {
    textDocument: { uri: sensitiveHeadersUri },
    range: { start: { line: sensitivePrintLine, character: 0 }, end: { line: sensitivePrintLine, character: sensitiveHeadersLines[sensitivePrintLine].length } },
    context: { diagnostics: sensitiveHeaderDiagnostics },
  });
  const wrapSensitiveHeaders = Array.isArray(sensitiveHeaderActions.result)
    ? sensitiveHeaderActions.result.find((action) => action.title === "Wrap sensitive headers with headers_redact")
    : null;
  const wrapSensitiveHeadersEdit =
    wrapSensitiveHeaders &&
    wrapSensitiveHeaders.edit &&
    wrapSensitiveHeaders.edit.changes &&
    wrapSensitiveHeaders.edit.changes[sensitiveHeadersUri] &&
    wrapSensitiveHeaders.edit.changes[sensitiveHeadersUri][0];
  if (
    !wrapSensitiveHeadersEdit ||
    wrapSensitiveHeadersEdit.newText !== "headers_redact(headers)"
  ) {
    throw new Error(`missing sensitive header redaction action: ${JSON.stringify(sensitiveHeaderActions.result)}`);
  }
  const wrapSensitiveLogHeaders = Array.isArray(sensitiveHeaderActions.result)
    ? sensitiveHeaderActions.result.find((action) => {
      const edit =
        action.edit &&
        action.edit.changes &&
        action.edit.changes[sensitiveHeadersUri] &&
        action.edit.changes[sensitiveHeadersUri][0];
      return action.title === "Wrap sensitive headers with headers_redact" &&
        edit &&
        edit.range.start.line === sensitiveLogLine &&
        edit.newText === "headers_redact(headers)";
    })
    : null;
  if (!wrapSensitiveLogHeaders) {
    throw new Error(`missing sensitive log header redaction action: ${JSON.stringify(sensitiveHeaderActions.result)}`);
  }

  const toolPolicyActions = await request("textDocument/codeAction", {
    textDocument: { uri: toolPolicyUri },
    range: { start: { line: directToolCallLine, character: 0 }, end: { line: directToolCallLine, character: toolPolicyLines[directToolCallLine].length } },
    context: { diagnostics: toolPolicyDiagnostics },
  });
  const wrapToolCall = Array.isArray(toolPolicyActions.result)
    ? toolPolicyActions.result.find((action) => action.title === "Wrap tool_call with tool_call_policy")
    : null;
  const wrapToolCallEdit =
    wrapToolCall &&
    wrapToolCall.edit &&
    wrapToolCall.edit.changes &&
    wrapToolCall.edit.changes[toolPolicyUri] &&
    wrapToolCall.edit.changes[toolPolicyUri][0];
  if (
    !wrapToolCallEdit ||
    wrapToolCallEdit.newText !== "tool_call_policy(spec, policy)"
  ) {
    throw new Error(`missing tool_call policy action: ${JSON.stringify(toolPolicyActions.result)}`);
  }
  const wrapModuleToolCall = Array.isArray(toolPolicyActions.result)
    ? toolPolicyActions.result.find((action) => action.title === "Wrap tool.call with tool.call_policy")
    : null;
  const wrapModuleToolCallEdit =
    wrapModuleToolCall &&
    wrapModuleToolCall.edit &&
    wrapModuleToolCall.edit.changes &&
    wrapModuleToolCall.edit.changes[toolPolicyUri] &&
    wrapModuleToolCall.edit.changes[toolPolicyUri][0];
  if (
    !wrapModuleToolCallEdit ||
    wrapModuleToolCallEdit.newText !== "tool.call_policy(spec, policy)"
  ) {
    throw new Error(`missing tool.call policy action: ${JSON.stringify(toolPolicyActions.result)}`);
  }
  const starterPolicyAssignment = Array.isArray(toolPolicyActions.result)
    ? toolPolicyActions.result.find((action) => {
      const edit =
        action.edit &&
        action.edit.changes &&
        action.edit.changes[toolPolicyUri] &&
        action.edit.changes[toolPolicyUri][0];
      return action.title === "Replace empty tool policy with starter policy" &&
        edit &&
        edit.range.start.line === widePolicyLine &&
        edit.newText === starterToolPolicy;
    })
    : null;
  if (!starterPolicyAssignment) {
    throw new Error(`missing empty policy assignment action: ${JSON.stringify(toolPolicyActions.result)}`);
  }
  const starterPolicyInline = Array.isArray(toolPolicyActions.result)
    ? toolPolicyActions.result.find((action) => {
      const edit =
        action.edit &&
        action.edit.changes &&
        action.edit.changes[toolPolicyUri] &&
        action.edit.changes[toolPolicyUri][0];
      return action.title === "Replace empty tool policy with starter policy" &&
        edit &&
        edit.range.start.line === weakModuleToolPolicyLine &&
        edit.newText === starterToolPolicy;
    })
    : null;
  if (!starterPolicyInline) {
    throw new Error(`missing inline empty policy action: ${JSON.stringify(toolPolicyActions.result)}`);
  }
  const starterPolicyPrefixlessAssignment = Array.isArray(toolPolicyActions.result)
    ? toolPolicyActions.result.find((action) => {
      const edit =
        action.edit &&
        action.edit.changes &&
        action.edit.changes[toolPolicyUri] &&
        action.edit.changes[toolPolicyUri][0];
      return action.title === "Replace empty tool policy with starter policy" &&
        edit &&
        edit.range.start.line === prefixlessPolicyLine &&
        edit.newText === starterToolPolicy;
    })
    : null;
  if (!starterPolicyPrefixlessAssignment) {
    throw new Error(`missing prefixless policy assignment action: ${JSON.stringify(toolPolicyActions.result)}`);
  }
  const starterPolicyPrefixlessInline = Array.isArray(toolPolicyActions.result)
    ? toolPolicyActions.result.find((action) => {
      const edit =
        action.edit &&
        action.edit.changes &&
        action.edit.changes[toolPolicyUri] &&
        action.edit.changes[toolPolicyUri][0];
      return action.title === "Replace empty tool policy with starter policy" &&
        edit &&
        edit.range.start.line === prefixlessModuleToolPolicyLine &&
        edit.newText === starterToolPolicy;
    })
    : null;
  if (!starterPolicyPrefixlessInline) {
    throw new Error(`missing inline prefixless policy action: ${JSON.stringify(toolPolicyActions.result)}`);
  }

  const formatting = await request("textDocument/formatting", {
    textDocument: { uri: uriA },
    options: { tabSize: 2, insertSpaces: true },
  });
  if (!Array.isArray(formatting.result) || formatting.result.length !== 1) {
    throw new Error(`bad formatting edits: ${JSON.stringify(formatting.result)}`);
  }

  const definition = await request("textDocument/definition", {
    textDocument: { uri: uriB },
    position: { line: 0, character: 12 },
  });
  if (!definition.result || definition.result.uri !== uriA || definition.result.range.start.line !== 0) {
    throw new Error(`bad cross-file definition: ${JSON.stringify(definition.result)}`);
  }

  const constantDefinition = await request("textDocument/definition", {
    textDocument: { uri: constantsUri },
    position: { line: 3, character: constantsLines[3].indexOf("BUILD_LIMIT") + 2 },
  });
  if (!constantDefinition.result || constantDefinition.result.uri !== constantsUri || constantDefinition.result.range.start.line !== 0) {
    throw new Error(`bad constant definition: ${JSON.stringify(constantDefinition.result)}`);
  }

  const constantHover = await request("textDocument/hover", {
    textDocument: { uri: constantsUri },
    position: { line: 3, character: constantsLines[3].indexOf("BUILD_LIMIT") + 2 },
  });
  const constantHoverText = constantHover.result && constantHover.result.contents && constantHover.result.contents.value;
  if (!constantHoverText || !constantHoverText.includes("BUILD_LIMIT is 42")) {
    throw new Error(`bad constant hover: ${JSON.stringify(constantHover.result)}`);
  }

  const typedHoverTexts = [];
  async function assertTypedHover(line, word, expectedType, expectedSource) {
    const typedHover = await request("textDocument/hover", {
      textDocument: { uri: typedUri },
      position: { line, character: typedLines[line].indexOf(word) + 2 },
    });
    const text = typedHover.result && typedHover.result.contents && typedHover.result.contents.value;
    if (!text || !text.includes(`${word}: ${expectedType}`) || !text.includes(expectedSource)) {
      throw new Error(`bad inferred type hover for ${word}: ${JSON.stringify(typedHover.result)}`);
    }
    typedHoverTexts.push(text);
    return text;
  }

  await assertTypedHover(1, "count", "number", "count is 42");
  await assertTypedHover(2, "count", "string", 'count is "forty-two"');
  await assertTypedHover(3, "name", "string", 'name is "Ari"');
  await assertTypedHover(4, "flags", "array", "flags is [true, false]");
  await assertTypedHover(5, "config", "dict", 'config is {mode: "fast", retries: 2}');
  await assertTypedHover(7, "inner", "nil", "inner is nil");

  const references = await request("textDocument/references", {
    textDocument: { uri: uriB },
    position: { line: localReturnLine, character: 12 },
    context: { includeDeclaration: true },
  });
  if (!Array.isArray(references.result) || references.result.length !== 3) {
    throw new Error(`bad cross-file references: ${JSON.stringify(references.result)}`);
  }

  const rename = await request("textDocument/rename", {
    textDocument: { uri: uriB },
    position: { line: 0, character: 12 },
    newName: "rank_score",
  });
  const changes = rename.result && rename.result.changes;
  if (!changes || !changes[uriA] || !changes[uriB]) {
    throw new Error(`rename did not include both docs: ${JSON.stringify(rename.result)}`);
  }
  const editCount = Object.values(changes).reduce((count, edits) => count + edits.length, 0);
  if (editCount !== 3) throw new Error(`bad rename edit count: ${editCount}`);

  const diskDefinition = await request("textDocument/definition", {
    textDocument: { uri: uriB },
    position: { line: 1, character: 16 },
  });
  if (!diskDefinition.result || diskDefinition.result.uri !== diskDefUri || diskDefinition.result.range.start.line !== 0) {
    throw new Error(`bad indexed definition: ${JSON.stringify(diskDefinition.result)} expected ${diskDefUri}`);
  }

  const diskReferences = await request("textDocument/references", {
    textDocument: { uri: uriB },
    position: { line: 1, character: 16 },
    context: { includeDeclaration: true },
  });
  if (!Array.isArray(diskReferences.result) || diskReferences.result.length !== 2) {
    throw new Error(`bad indexed references: ${JSON.stringify(diskReferences.result)}`);
  }
  const diskReferenceUris = new Set(diskReferences.result.map((reference) => reference.uri));
  if (!diskReferenceUris.has(uriB) || !diskReferenceUris.has(diskDefUri)) {
    throw new Error(`indexed references missing files: ${JSON.stringify(diskReferences.result)}`);
  }

  const syntaxUri = "file:///C:/tmp/sura_lsp_syntax_error.sura";
  notify("textDocument/didOpen", {
    textDocument: {
      uri: syntaxUri,
      languageId: "sura",
      version: 1,
      text: ["value is", "func broken() do", "return 1", ""].join("\n"),
    },
  });
  const syntaxDiagnostics = await waitForDiagnostics(
    syntaxUri,
    (diagnostics) => diagnostics.filter((diagnostic) => diagnostic.source === "sura-parser").length >= 2,
    "parser diagnostics",
  );
  const codeActions = await request("textDocument/codeAction", {
    textDocument: { uri: syntaxUri },
    range: { start: { line: 1, character: 0 }, end: { line: 1, character: 1 } },
    context: { diagnostics: syntaxDiagnostics },
  });
  if (
    !Array.isArray(codeActions.result) ||
    !codeActions.result.some((action) => action.title === "Insert missing end")
  ) {
    throw new Error(`missing code action: ${JSON.stringify(codeActions.result)}`);
  }

  const typeUri = "file:///C:/tmp/sura_lsp_type_error.sura";
  notify("textDocument/didOpen", {
    textDocument: {
      uri: typeUri,
      languageId: "sura",
      version: 1,
      text: [
        "func add(x: number, y: number) -> number do",
        "return x + y",
        "end",
        "value is add(1, \"wrong\")",
        "",
      ].join("\n"),
    },
  });
  const typeDiagnostics = await waitForDiagnostics(
    typeUri,
    (diagnostics) => diagnostics.some((diagnostic) => diagnostic.source === "sura-typechecker"),
    "typechecker diagnostics",
  );

  await request("shutdown", null);
  notify("exit", null);

  console.log(
    JSON.stringify(
      {
        ok: true,
        workspaceSymbols: names,
        constantDocumentSymbol: buildLimitSymbol.kind,
        constantWorkspaceSymbol: constantWorkspaceNames.includes("BUILD_LIMIT"),
        constantDefinition: constantDefinition.result.range.start.line,
        semanticTokenInts: semantic.result.data.length,
        incrementalDidChange: incrementalNames.includes("changed_score"),
        referenceCount: references.result.length,
        renameEditCount: editCount,
        indexedReferenceCount: diskReferences.result.length,
        userHover: userHoverText.includes("shared_score(x)"),
        completionHasSchemaErrors: completionLabels.includes("schema_errors"),
        completionHasHttpRequest: completionLabels.includes("http_request"),
        completionHasAsyncHttpGet: completionLabels.includes("async_http_get"),
        completionHasAsyncSleep: completionLabels.includes("async_sleep"),
        completionHasTestSummary: completionLabels.includes("test_summary"),
        completionHasAsyncStatus: completionLabels.includes("async_status"),
        completionHasAsyncCleanup: completionLabels.includes("async_cleanup"),
        completionHasHttpRequestJsonChecked: completionLabels.includes("http_request_json_checked"),
        completionHasHttpRequestRetryJson: completionLabels.includes("http_request_retry_json"),
        completionHasHttpRequestRetryJsonChecked: completionLabels.includes("http_request_retry_json_checked"),
        completionHasAuthBearer: completionLabels.includes("auth_bearer"),
        completionHasHeadersMerge: completionLabels.includes("headers_merge"),
        completionHasHeadersGet: completionLabels.includes("headers_get"),
        completionHasHeadersHas: completionLabels.includes("headers_has"),
        completionHasHeadersRedact: completionLabels.includes("headers_redact"),
        completionHasCookieParse: completionLabels.includes("cookie_parse"),
        completionHasCookieBuild: completionLabels.includes("cookie_build"),
        completionHasCookieGet: completionLabels.includes("cookie_get"),
        completionHasFormBuild: completionLabels.includes("form_build"),
        completionHasFormParse: completionLabels.includes("form_parse"),
        completionHasHttpContentType: completionLabels.includes("http_content_type"),
        completionHasHttpCharset: completionLabels.includes("http_charset"),
        completionHasHttpIsJson: completionLabels.includes("http_is_json"),
        completionHasHttpRetryAfter: completionLabels.includes("http_retry_after"),
        completionHasHttpBackoffDelays: completionLabels.includes("http_backoff_delays"),
        completionHasFileSha256: completionLabels.includes("file_sha256"),
        completionHasFileHmacSha256: completionLabels.includes("file_hmac_sha256"),
        completionHasTempDir: completionLabels.includes("temp_dir"),
        completionHasCmdExists: completionLabels.includes("cmd_exists"),
        completionHasCmdQuote: completionLabels.includes("cmd_quote"),
        completionHasCmdJoin: completionLabels.includes("cmd_join"),
        completionHasCmdRun: completionLabels.includes("cmd_run"),
        completionHasCmdRunChecked: completionLabels.includes("cmd_run_checked"),
        completionHasCryptoRandomHex: completionLabels.includes("crypto_random_hex"),
        completionHasConstantTimeEq: completionLabels.includes("constant_time_eq"),
        completionHasLogSetLevel: completionLabels.includes("log_set_level"),
        completionHasLogGetLevel: completionLabels.includes("log_get_level"),
        completionHasLogLevel: completionLabels.includes("log_level"),
        completionHasStreamBatch: completionLabels.includes("stream_batch"),
        completionHasDatetimeParse: completionLabels.includes("datetime_parse"),
        completionHasArrayModule: completionLabels.includes("array"),
        completionHasMathModule: completionLabels.includes("math"),
        completionHasPathModule: completionLabels.includes("path"),
        completionHasStringModule: completionLabels.includes("string"),
        completionHasOsModule: completionLabels.includes("os"),
        completionHasCliModule: completionLabels.includes("cli"),
        completionHasLlmModule: completionLabels.includes("llm"),
        completionHasRandomModule: completionLabels.includes("random"),
        completionHasPythonModule: completionLabels.includes("python"),
        completionHasFfiModule: completionLabels.includes("ffi"),
        completionHasPluginModule: completionLabels.includes("plugin"),
        completionHasVectorModule: completionLabels.includes("vector"),
        completionHasGraphics3dModule: completionLabels.includes("graphics3d"),
        completionHasAutogradModule: completionLabels.includes("autograd"),
        completionHasTokenizerModule: completionLabels.includes("tokenizer"),
        completionHasDatasetModule: completionLabels.includes("dataset"),
        directAutogradBuiltinCount: directAutogradBuiltins.filter((name) => completionLabels.includes(name)).length,
        directTokenizerBuiltinCount: directTokenizerBuiltins.filter((name) => completionLabels.includes(name)).length,
        directDatasetBuiltinCount: directDatasetBuiltins.filter((name) => completionLabels.includes(name)).length,
        completionHasWorkspaceFunction: completionLabels.includes("shared_score"),
        completionHasWorkspaceType: completionLabels.includes("AgentScore"),
        completionHasIndexedFunction: completionLabels.includes("disk_score"),
        completionHasWorkspaceConstant: completionLabels.includes("BUILD_LIMIT"),
        cliModuleCompletionHasParse: cliLabels.includes("parse"),
        jsonModuleCompletionHasPath: jsonLabels.includes("path"),
        jsonModuleCompletionHasPretty: jsonLabels.includes("pretty"),
        fsModuleCompletionHasRead: fsLabels.includes("read"),
        fsModuleCompletionHasReadBytes: fsLabels.includes("read_bytes"),
        fsModuleCompletionHasSha256: fsLabels.includes("sha256"),
        fsModuleCompletionHasGlob: fsLabels.includes("glob"),
        regexModuleCompletionHasEscape: regexLabels.includes("escape"),
        regexModuleCompletionHasCapture: regexLabels.includes("capture"),
        regexModuleCompletionHasCaptures: regexLabels.includes("captures"),
        dbModuleCompletionHasQuery: dbLabels.includes("query"),
        dbModuleCompletionHasFind: dbLabels.includes("find"),
        cryptoModuleCompletionHasFileSha256: cryptoLabels.includes("file_sha256"),
        cryptoModuleCompletionHasFileHmacSha256: cryptoLabels.includes("file_hmac_sha256"),
        cryptoModuleCompletionHasRandomHex: cryptoLabels.includes("random_hex"),
        cryptoModuleCompletionHasConstantTimeEq: cryptoLabels.includes("constant_time_eq"),
        llmModuleCompletionHasRequestJson: llmLabels.includes("request_json"),
        llmModuleCompletionHasTools: llmLabels.includes("tools"),
        llmModuleCompletionHasRequestTools: llmLabels.includes("request_tools"),
        llmModuleCompletionHasToolCalls: llmLabels.includes("tool_calls"),
        llmModuleCompletionHasToolResult: llmLabels.includes("tool_result"),
        llmModuleCompletionHasRunTools: llmLabels.includes("run_tools"),
        llmModuleCompletionHasNextMessages: llmLabels.includes("next_messages"),
        llmModuleCompletionHasNextRequest: llmLabels.includes("next_request"),
        llmModuleCompletionHasNextSchemaRequest: llmLabels.includes("next_schema_request"),
        pythonModuleCompletionHasCallJson: pythonLabels.includes("call_json"),
        ffiModuleCompletionHasCall: ffiLabels.includes("call"),
        pluginModuleCompletionHasCall: pluginLabels.includes("call"),
        httpModuleCompletionHasRequestJsonChecked: httpLabels.includes("request_json_checked"),
        httpModuleCompletionHasRequestRetryJsonChecked: httpLabels.includes("request_retry_json_checked"),
        httpModuleCompletionHasUrlParse: httpLabels.includes("url_parse"),
        httpModuleCompletionHasUrlBuild: httpLabels.includes("url_build"),
        httpModuleCompletionHasStatusText: httpLabels.includes("status_text"),
        httpModuleCompletionHasRetryAfter: httpLabels.includes("retry_after"),
        httpModuleCompletionHasBackoffDelays: httpLabels.includes("backoff_delays"),
        httpModuleCompletionHasAuthBearer: httpLabels.includes("auth_bearer"),
        httpModuleCompletionHasHeadersMerge: httpLabels.includes("headers_merge"),
        httpModuleCompletionHasHeadersGet: httpLabels.includes("headers_get"),
        httpModuleCompletionHasHeadersHas: httpLabels.includes("headers_has"),
        httpModuleCompletionHasHeadersRedact: httpLabels.includes("headers_redact"),
        httpModuleCompletionHasCookieParse: httpLabels.includes("cookie_parse"),
        httpModuleCompletionHasCookieBuild: httpLabels.includes("cookie_build"),
        httpModuleCompletionHasCookieGet: httpLabels.includes("cookie_get"),
        httpModuleCompletionHasFormBuild: httpLabels.includes("form_build"),
        httpModuleCompletionHasFormParse: httpLabels.includes("form_parse"),
        httpModuleCompletionHasContentType: httpLabels.includes("content_type"),
        httpModuleCompletionHasCharset: httpLabels.includes("charset"),
        httpModuleCompletionHasIsJson: httpLabels.includes("is_json"),
        asyncModuleCompletionHasSleep: asyncLabels.includes("sleep"),
        arrayModuleCompletionHasSlice: arrayLabels.includes("slice"),
        arrayModuleCompletionHasLength: arrayLabels.includes("length"),
        arrayModuleCompletionHasSize: arrayLabels.includes("size"),
        mathModuleCompletionHasFloor: mathLabels.includes("floor"),
        pathModuleCompletionHasJoin: pathLabels.includes("join"),
        stringModuleCompletionHasChunks: stringLabels.includes("chunks"),
        stringModuleCompletionHasLength: stringLabels.includes("length"),
        stringModuleCompletionHasSize: stringLabels.includes("size"),
        osModuleCompletionHasEnvGet: osLabels.includes("env_get"),
        osModuleCompletionHasTempDir: osLabels.includes("temp_dir"),
        osModuleCompletionHasName: osLabels.includes("name"),
        osModuleCompletionHasCmdExists: osLabels.includes("cmd_exists"),
        osModuleCompletionHasCmdQuote: osLabels.includes("cmd_quote"),
        osModuleCompletionHasCmdJoin: osLabels.includes("cmd_join"),
        osModuleCompletionHasRun: osLabels.includes("run"),
        osModuleCompletionHasRunChecked: osLabels.includes("run_checked"),
        randomModuleCompletionHasInt: randomLabels.includes("int"),
        vectorModuleCompletionHasCosine: vectorLabels.includes("cosine"),
        autogradModuleCompletionCount: autogradMethods.filter((name) => autogradLabels.includes(name)).length,
        tokenizerModuleCompletionCount: tokenizerMethods.filter((name) => tokenizerLabels.includes(name)).length,
        datasetModuleCompletionCount: datasetMethods.filter((name) => datasetLabels.includes(name)).length,
        moduleHoverHasAutogradMatmul: autogradHoverText.includes("autograd.matmul"),
        moduleHoverHasAutogradCast: autogradCastHoverText.includes("autograd.cast"),
        moduleHoverHasTokenizerEncode: tokenizerHoverText.includes("tokenizer.encode"),
        moduleHoverHasDatasetOpen: datasetHoverText.includes("dataset.open"),
        directHoverHasAutogradBceLogits: directAutogradHoverText.includes("autograd_bce_logits"),
        moduleHoverHasMathFloor: mathHoverText.includes("math.floor"),
        moduleHoverHasRandomInt: randomHoverText.includes("random.int"),
        moduleHoverHasCliParse: cliHoverText.includes("cli.parse"),
        moduleHoverHasHttpRequestJsonChecked: httpHoverText.includes("http.request_json_checked"),
        moduleHoverHasHttpFormParse: httpFormHoverText.includes("http.form_parse"),
        moduleHoverHasHttpHeadersRedact: httpHeadersRedactHoverText.includes("http.headers_redact"),
        moduleHoverHasAsyncSleep: asyncHoverText.includes("async.sleep"),
        moduleHoverHasRegexCapture: regexHoverText.includes("regex.capture"),
        moduleHoverHasDbQuery: dbHoverText.includes("db.query"),
        moduleSignatureActiveParameter: jsonSignature.result.activeParameter,
        llmModuleSignatureActiveParameter: llmSignature.result.activeParameter,
        regexModuleSignatureActiveParameter: regexSignature.result.activeParameter,
        dbModuleSignatureActiveParameter: dbSignature.result.activeParameter,
        autogradModuleSignatureActiveParameter: autogradSignature.result.activeParameter,
        sensitiveHeaderDiagnostics: sensitiveHeaderDiagnostics.length,
        toolCallPolicyDiagnostics: toolPolicyDiagnostics.length,
        legacySyntaxDiagnostics: legacySyntaxDiagnostics.length,
        constantHover: constantHoverText.includes("BUILD_LIMIT is 42"),
        typedHoverCount: typedHoverTexts.length,
        typedHoverHasDict: typedHoverTexts.some((text) => text.includes("config: dict")),
        typedHoverHasLocalNil: typedHoverTexts.some((text) => text.includes("inner: nil")),
        builtinSignatureActiveParameter: builtinSignature.result.activeParameter,
        syntaxDiagnostics: syntaxDiagnostics.length,
        typeDiagnostics: typeDiagnostics.length,
        httpPostCodeActions: httpPostActions.result.length,
        httpGetCodeActions: httpGetActions.result.length,
        httpJsonCodeActions: httpJsonActions.result.length,
        asyncHttpGetCodeActions: asyncHttpGetActions.result.length,
        asyncHttpRequestCodeActions: asyncHttpRequestActions.result.length,
        sensitiveHeaderCodeActions: sensitiveHeaderActions.result.length,
        toolCallPolicyCodeActions: toolPolicyActions.result.length,
        codeActions: codeActions.result.length,
      },
      null,
      2,
    ),
  );
}

main().catch((error) => {
  try {
    notify("exit", null);
  } catch (_) {
    // best effort shutdown
  }
  console.error(error.stack || String(error));
  process.exitCode = 1;
}).finally(() => {
  setTimeout(() => child.kill(), 100);
});
