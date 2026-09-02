# Sura Practical Ecosystem Guide

This guide covers the pieces that make Sura usable beyond toy scripts: the practical standard library, package manager, profiler/debugger, editor support, tests, and JIT benchmarks.

For dependency-free native neural-network creation and training, see [Sura Native AI](AI.md).

The native `use nn` module (also available as `use ai`) provides feature standardization, paired train/test splits, MLP creation, Adam/SGD training, prediction, classification, evaluation, summaries, one-hot targets, and validated JSON model save/load.

## Standard Library

Core collections and strings are available as functions and as methods:

```sura
nums is [3, 1, 2]
print nums.sort().join(",")
print array_sum([1, 2, 3])
print array_avg([2, 4, 6])
print array_min([4, -2, 7])
print array_max([4, -2, 7])
print array_unique([1, 1, 2, 2]).join(",")
print array_flatten([1, [2, [3]]], 2).join(",")
print array_range(5).join(",")
print array_chunk(["a", "b", "c"], 2)[0].join("/")
print array_zip(["name", "score"], ["sura", 9])[1][1]
print set_union(["ai", "game"], ["game", "tool"]).join(",")
print set_difference(["ai", "game"], ["game"]).join(",")
print "  hello  ".trim().upper()
```

Assertions are built in so scripts and test files can fail fast:

```sura
assert(total > 0, "total must be positive")
assert_eq(round(3.5), 4)
assert_ne(name, "")
assert_contains(["ai", "automation"], "ai")
assert_not_contains("sura", "python")
assert_match("agent-2026", "agent-[0-9]+")
assert_type({ok: true}, "dict")
assert_len(["plan", "run", "ship"], 3)
assert_between(score, 0, 100)
assert_approx(0.1 + 0.2, 0.3, 0.000001)
```

Use non-throwing checks when an automation script should collect multiple results before deciding whether to fail:

```sura
results is [check("config loaded", file_exists("sura.pkg.json")), check_eq("math result", 1 + 1, 2), check_match("agent id", "agent-2026", "agent-[0-9]+")]
summary is test_summary(results)
print test_report(results, "preflight checks")
assert(summary.ok, "preflight checks failed")
```

The small `math` module is available for namespaced calls and constants:

```sura
use math
print math.sqrt(16)
print math.pi
print uuid_v4()
```

Practical file APIs:

```sura
file_write("out.txt", "hello")
file_append("out.txt", " world")
print file_exists("out.txt")
print file_read("out.txt")
file_write_bytes("blob.bin", [0, 1, 255])
print file_read_bytes("blob.bin").join(",")
print file_sha256("blob.bin")
file_write_json("config.json", {name: "Sura", enabled: true})
print file_read_json("config.json").name
backup is path_join("tmp", "out.backup.txt")
file_copy("out.txt", backup)
print file_size(backup)
print file_info(backup).modified
print path_basename(backup)
print path_stem(backup)
print path_relative(path_abs(backup), cwd())
print path_normalize(path_join("tmp", "..", "out.txt"))
print file_list(".")
print file_walk("src", ".sura")
print file_glob("src/**/*.sura")
fs.write_json("config.json", {name: "Sura", enabled: true})
print fs.read_json("config.json").enabled
fs.write_bytes("blob.bin", [5, 6])
print fs.read_bytes("blob.bin")[1]
print fs.sha256("blob.bin")
print fs.glob("*.txt")
file_delete(backup)
file_delete("out.txt")
file_delete("config.json")
file_delete("blob.bin")
file_remove_tree("scratch-output")
```

On Windows, file and path APIs treat Sura strings as UTF-8, so Korean filenames such as `path_join("한글", "파일.txt")` work without changing the console code page.

Command-line scripts can read the arguments passed after the script path:

```sura
args is argv()
print script_name()
print argc()
print home_dir()
print temp_dir()
print os.name()
print os.path_separator()
print os.which("python")
print os.cmd_exists("git")
result is os.run(os.cmd_join(["echo", "sura"]))
print result.exit_code
print result.output
parsed is cli_parse(args.join(" "))
```

Environment and API secrets:

```sura
env_load(".env")
api_key is env_require("OPENAI_API_KEY")
headers is auth_bearer(api_key)
print env_get("SURA_MODE", "dev")
```

JSON APIs:

```sura
data is json_parse(json_stringify({name: "sura", ok: true}))
print data.name
print json_stringify(data)
print json_pretty(data, 2)
fallback is json_try_parse("not json", {ok:false})
print fallback.ok
print schema_validate(data, {"name":"string", "ok":"bool"})
rows is csv_parse("name,score\nsura,\"9,9\"", true)
print rows[0].score
print csv_stringify(rows, ["name", "score"])
config is json.ini_parse("name=Sura\n[agent]\nmode=fast\n")
print config.agent.mode
print json.ini_stringify({name:"Sura", agent:{mode:"fast"}})
events is jsonl_parse(jsonl_stringify([{role:"user", content:"hi"}]))
print events[0].content
response is {choices: [{message: {content: "done"}}]}
print json_path(response, "$.choices[0].message.content", "")
print json_has_path(response, "$.choices[0].message.content")
print json_merge_patch({model:"fast", options:{stream:true}}, {options:{stream:nil, json:true}}).options.json
print json_delete_path(response, "$.choices[0].message.content")
print json_set_path(response, "$.choices[0].message.content", "updated")
print template_render("Assistant said: [[choices[0].message.content]]", response)
records is [{name:"sura", kind:"lang", meta:{score:10}}, {name:"lua", kind:"lang", meta:{score:7}}, {name:"agent", kind:"bot", meta:{score:9}}]
print pluck(records, "name").join(",")
print count_by(records, "kind").lang
print group_by(records, "kind").bot[0].name
print sort_by(records, "meta.score", true)[0].name
meta is {name: "sura", kind: "lang"}
print dict_keys(meta).join(",")
merged_meta is dict_merge(meta, {score: 9})
print merged_meta.score
picked_meta is dict_pick(meta, ["name"])
print picked_meta.name
agent_schema is {"type":"dict", "required":["name", "steps"], "properties":{"name":{"type":"string", "min_len":1}, "steps":{"type":"array", "items":{"type":"string"}}}, "additional":false}
print schema_errors({"name":"bot", "steps":["read", "act"]}, agent_schema)
print schema_to_json_schema(agent_schema).properties.name.type
```

Network and async helpers:

```sura
print http_get("https://example.com")
print http_json("https://example.com/data.json")
print http_post("https://example.com", "{\"ok\":true}", "application/json")
headers is headers_merge(auth_bearer(env_require("OPENAI_API_KEY")), {"X-Agent": "sura"})
module_headers is http.headers_merge(http.auth_bearer(env_require("OPENAI_API_KEY")), {"X-Agent": "sura"})
print http.headers_get(module_headers, "authorization")
print http.headers_has(module_headers, "x-agent")
safe_headers is http.headers_redact(module_headers, ["X-Agent"], "***")
print safe_headers.Authorization
cookies is http.cookie_parse("Cookie: session=abc%20123; theme=dark")
print cookies.session
cookie_header is http.cookie_build({session: "abc 123", theme: "dark"})
print http.cookie_get({Cookie: cookie_header}, "theme", "missing")
form_body is http.form_build({q: "sura agent", tags: ["ai", "tools"]})
print form_body
print http.form_parse(form_body).q
print http.content_type({"Content-Type": "application/problem+json; charset=utf-8"})
print http.charset({"Content-Type": "application/problem+json; charset=utf-8"})
print http.is_json({"Content-Type": "application/problem+json"})
api_url is http.url_build({scheme: "https", host: "api.example.com", path: "tasks", params: {trace: "sura"}})
print http.url_parse(api_url).params.trace
print http.status_text(429)
print http.status_retryable(503)
print http.retry_after({"Retry-After": "2"}, 250)
print http.backoff_delays(4, 250, 2, 5000).join(",")
response is http_request({method: "POST", url: "https://api.example.com/tasks", query: {trace: "sura"}, headers: headers, json: {task: "summarize", text: "hello"}, timeout: 30})
form_response is http_request({url: "https://api.example.com/login", form: {username: "sura", scope: ["agent", "api"]}, timeout: 30})
full is http_request_full({url: "https://api.example.com/status"})
print full.status
print full.headers["content-type"]
stable is http_request_retry({url: "https://api.example.com/status"}, 3, 250)
print stable.attempts
print http_request_json({url: "https://api.example.com/status"}).ok
print http_request_json_checked({url: "https://api.example.com/status"}).ok
print http_request_retry_json({url: "https://api.example.com/status"}, 3, 250).ok
print http_request_retry_json_checked({url: "https://api.example.com/status"}, 3, 250).ok
get_task is async_http_get("https://api.example.com/status")
request_task is async_http_request({url: "https://api.example.com/status", timeout: 10})
print await_any([get_task, request_task], 5000, nil).output
timer_task is async_sleep(250)
print async_await_timeout(timer_task, 10, "timer pending")

server is http_serve_static("public", 8080)
print http_server_url(server)
print http_server_stop(server)

routes is {}
routes["GET /health"] is {json: {ok: true}}
routes["POST /echo"] is {status: 201, echo: true}
api is http_serve_routes(routes, 8081)
print http_request_json({url: http_server_url(api) + "health"}).ok
print http_server_stop(api)

task is async_cmd("echo done")
print async_ready(task)
print async_await(task).trim()
sync_result is cmd_run(cmd_join(["echo", "sync done"]))
print sync_result.ok
print sync_result.output.trim()
checked_result is os.run_checked(os.cmd_join(["echo", "checked done"]))
print checked_result.output.trim()
slow is async_cmd("cmd /c ping -n 2 127.0.0.1 > nul && echo done")
print async_status(slow).running
print pluck(async_pending(), "id").join(",")
print async_await_timeout(slow, 10, "still running")
print await_timeout(slow, 5000, "timeout").trim()
done_without_output is async_cmd("echo cache-warm")
sleep_ms(500)
wait(50)
print async_forget(done_without_output)
print async_cleanup()
race_tasks is [async_cmd("echo fetch-a"), async_cmd("echo fetch-b")]
first is async_any(race_tasks, 5000, nil)
print first.output.trim()
tasks is [async_cmd("echo fetch-a"), async_cmd("echo fetch-b")]
print async_all(tasks).join("|")
slow_tasks is [async_cmd("cmd /c ping -n 2 127.0.0.1 > nul && echo a"), async_cmd("cmd /c ping -n 2 127.0.0.1 > nul && echo b")]
print async_all_timeout(slow_tasks, 10, ["still running"])
print await_all_timeout(slow_tasks, 5000, ["timeout"]).join("|")
```

`http_get()` supports `http://`, `https://`, and `file://` URLs. The `file://` form is useful for stable local tests. `http_request(spec)` accepts `method`, `url`, `query`, `headers`, exactly one of `body`/`json`/`form`, `content_type`, and `timeout`; `query` is a dictionary encoded with `query_build()` and appended before any URL fragment. `form` dictionaries are encoded as `application/x-www-form-urlencoded` bodies by default, while `form_build(params)` and `form_parse(body)` are available when you need to build or decode form bodies manually. `http.headers_redact(headers, [names], [mask])` returns a copy of request headers with sensitive values such as authorization, cookies, API keys, tokens, and caller-provided names masked before logging. `http_request_full(spec)` returns `{status, ok, body, headers, url}` with lower-case response header names; `http_request_retry(spec, attempts, delay_ms)` retries until a full response is ok and records `attempts`; `http_request_json(spec)` parses the response into Sura values, while `http_request_json_checked(spec)` first requires a 2xx response and fails fast on HTTP errors before parsing JSON; `http_request_retry_json(spec, attempts, delay_ms)` combines retry and JSON parsing for API clients; and `http_request_retry_json_checked(spec, attempts, delay_ms)` retries first, then requires the final response to be 2xx before parsing JSON. `cmd_quote(text)` quotes one shell argument, `cmd_join(args)` quotes and joins an argument array, `cmd_run(command)`/`os.run(command)` run a shell command synchronously and return `{command, exit_code, ok, output}`, and `cmd_run_checked(command)`/`os.run_checked(command)` fail fast on non-zero exit codes for CI-style automation scripts. `async_http_get(url)` and `async_http_request(spec)` run the same text-returning network work in the background, while `async_sleep(milliseconds)` starts a timer task with no output; all return task ids that work with `async_status`, `await_timeout`, `await_any`, and `await_all`. Use `async_forget(task_id)` to drop a completed task without reading output and `async_cleanup()` to remove all completed background tasks. These cleanup helpers do not stop running shell processes; they only remove tasks that are already ready. `http_serve_static(dir, port)` starts a local static server bound to `127.0.0.1`, returns a server dictionary with `pid`, `port`, `url`, and `directory`, and should be paired with `http_server_stop(server)` when the script is done. `http_serve_routes(routes, port)` starts a local mock API server from a route dictionary such as `"GET /health": {json: {ok: true}}`, `"POST /echo": {status: 201, echo: true}`, or `"/plain": "body"`; it is useful for webhook, agent, and API-client tests.

Automation and AI-adjacent helpers:

```sura
spec is {"name":"http_get", "url":"https://api.example.com/status"}
typed_spec is tool_spec("http_get", {url: "https://api.example.com/status"})
print tool_validate(typed_spec)
print tool_schema("http_get").required.join(",")
print tool_call(spec)
print tool(spec)
print llm_tools(["http_get"])[0].function.parameters.required.join(",")
print llm_request_tools_json("model", llm_messages("system", "Fetch status"), ["http_get"], 0)
print llm_request_tools_schema_json("model", llm_messages("system", "Fetch status"), ["http_get"], {summary: "string"}, 0, "tool_answer")
print tool http_get {url: "https://api.example.com/status"}
print tool {name: "http_get", url: "https://api.example.com/status"}

policy is {tools: ["http_get"], url_prefixes: ["https://api.example.com/"], allow_shell: false}
if tool_allowed(typed_spec, policy) then
  print tool_call_policy(typed_spec, policy)
end

request_spec is tool_spec("http_request", {method: "POST", url: "https://api.example.com/tasks", query: {trace: "sura"}, json: {task: "summarize"}})
request_policy is {tools: ["http_request"], url_prefixes: ["https://api.example.com/"], http_methods: ["GET", "POST"], allow_shell: false}
if tool_allowed(request_spec, request_policy) then
  print tool_call_policy(request_spec, request_policy)
end

model_response is {choices: [{message: {tool_calls: [{id: "call_1", function: {name: "http_get", arguments: json_stringify({url: "https://api.example.com/status"})}}]}}]}
call is llm_tool_calls(model_response)[0]
print tool_call_policy(call, policy)
print llm_tool_result(call, "ok").role
print llm_run_tools(model_response, policy)[0].role
print llm_next_messages(messages, model_response, policy).len()

print vec_dot([1, 2, 3], [4, 5, 6])
use python
print python_eval("print(6 * 7)")
print python.available()
print python.executable()
print python.call_json("math", "sqrt", [81])
print python.call_json("operator", "add", [20, 22])
```

The `tool <name> { ... }` form is dedicated tool-call syntax. It lowers to `tool_call({"name": "<name>", ...})`, so package and runtime security checks only need to cover one execution path. `tool_spec(name, args)` builds a typed, validated tool dictionary; `tool_validate(spec)` checks raw dictionaries from models before execution; and `tool_schema(name)` exposes required fields for prompting or docs. `llm_tools([names])`/`llm_tool_schemas([names])` convert those Sura tool schemas into OpenAI-compatible function tool definitions, while `llm_request_tools(model, messages, tool_names, [temperature])` and `llm_request_tools_json(...)` attach them to a chat request. `llm_request_tools_schema(...)` and `llm_request_tools_schema_json(...)` attach both OpenAI-compatible tools and JSON Schema structured-output `response_format` in one request for agents that need tool calls plus predictable final JSON. Use `tool_allowed(spec, policy)` and `tool_call_policy(spec, policy)` when an agent or package should restrict risky tools; policies can allow specific tool names, URL prefixes for `http_get`/`http_request`, HTTP methods for `http_request`, `allowed_headers`, exact-match `required_headers`, `max_body_bytes` across body/json/form payloads, `max_timeout`, and command prefixes for explicitly enabled `shell`. `llm_tool_calls(response)` extracts OpenAI-style model tool calls into executable Sura tool specs, `llm_tool_result(call, result)` creates the `tool` role message for the next model turn, `llm_run_tools(response, policy)` executes extracted calls through `tool_call_policy`, and `llm_next_messages(messages, response, policy)` appends the assistant response plus tool result messages. `llm_next_request(...)`/`llm_next_request_json(...)` and `llm_next_schema_request(...)`/`llm_next_schema_request_json(...)` wrap that completed tool turn directly into the next OpenAI-compatible tool-enabled request, with optional schema-constrained output.

For Python interop, `use python` exposes `python.available()`, `python.executable()`, `python.call(...)`, and `python.call_json(...)`; the direct builtins (`python_available`, `python_call_json`, and friends) remain available for older scripts. `python.call_json(module, function, args, kwargs)` imports Python modules/packages, calls the target function, and returns JSON-compatible Sura values. This bridge is for calling existing libraries from Sura, not for translating Python source into Sura source. Set `SURA_PYTHON` when a project needs a specific Python interpreter or virtual environment.

Tensor, stream, and LLM request helpers provide a small AI-agent foundation without forcing a heavy dependency. `use autograd` provides packed CPU dtypes plus real f32/f16/bf16 resident CUDA storage. Typed matmul reads 2-byte low storage and emits f32 output/gradient; low trainable parameters use f32 master SGD/Adam, and checkpoint v3 can restore CUDA optimizer-ready leaves exactly. Autocast still changes compute rather than storage, and the rest of the resident operator subset remains f32-only. Low Adam steady state is `18N` versus f32 `16N`, so this native subset does not claim PyTorch GPU-stack or Python AI-ecosystem parity:

```sura
score is vector_cosine([0.12, 0.88, 0.3], [0.1, 0.9, 0.25])
unit is vector_normalize([3, 4])
print text_chunks("Sura helps AI agents", 8, 2).join("|")
print string_lines("a\nb").join("|")
print string_words("ai agent tools").join(",")
print string_pad_left("7", 3, "0")
docs is [{title: "game", embedding: [1, 0]}, {title: "agent", embedding: [0.8, 0.2]}]
print vector_search([1, 0], docs, 1)[0].item.title
rag_docs is [{id: "game", title: "Games", text: "Sura is fast for games.", embedding: [1, 0]}, {id: "agent", title: "Agents", text: "Sura helps AI agents.", embedding: [0.8, 0.2]}]
knowledge_paths is file_walk("knowledge", ".md")
print rag_context([1, 0], rag_docs, 2)
prepared is rag_prepare("What should I use?", [1, 0], rag_docs, 2)
print prepared.sources[0].title
print prepared.context

t is tensor_matmul([[1,2],[3,4]], [[5,6],[7,8]])
print tensor_shape(t)
print tensor_flatten(t).join(",")
print tensor_clip(t, 0, 50)
print tensor_sum(t)
print tensor_mean(t)
print tensor_variance(t)
print tensor_std(t)
print tensor_min(t)
print tensor_max(t)
print tensor_argmin(t)
print tensor_argmax(t)
print tensor_zscore([1, 2, 3])
print tensor_softmax([1, 2, 3])

use autograd
weights is autograd.parameter([[0]])
features is autograd.tensor([[1], [2]])
targets is autograd.tensor([[3], [5]])
prediction is autograd.linear(features, weights)
loss is autograd.mse(prediction, targets)
autograd.backward(loss)
print autograd.grad(weights)
autograd.sgd([weights], 0.01)

stream is stream_from(["plan", "act", "observe"])
print stream_next(stream)
print stream_take(stream, 1).join(",")
print stream_count(stream)
print stream_join(stream, " -> ")
chunk_stream is stream_from(["a", "b", "c", "d", "e"])
print stream_batch(chunk_stream, 2)[0].join(",")
events is stream_from([{kind: "log", msg: "start"}, {kind: "metric", msg: "cpu"}, {kind: "log", msg: "stop"}])
print stream_join(stream_map(stream_filter(events, {kind: "log"}), "msg"), " | ")
print stream_window(stream_from(["a", "b", "c", "d"]), 3)[0].join(",")
metrics is stream_from([{ms: 10}, {ms: 20}, {ms: 30}])
print stream_sum(metrics, "ms")
print stream_avg(stream_from([2, 4, 6]))

messages is llm_messages("You are concise", "Summarize Sura")
rag_messages_body is prepared.messages
body is llm_request_json("openai-compatible-model", messages, 0.2)
rag_body is llm_request_json("openai-compatible-model", rag_messages_body, 0.2)
answer_schema is {type: "dict", properties: {answer: {type: "string", min_len: 1}, sources: {type: "array", items: "string"}}, additional: false}
structured_body is llm_request_schema_json("openai-compatible-model", messages, answer_schema, 0.2, "sura_answer")
tool_structured_body is llm_request_tools_schema("openai-compatible-model", messages, ["http_get"], answer_schema, 0.2, "sura_answer")
print body
print rag_body
print structured_body
print json_stringify(tool_structured_body)
# raw_response is llm_chat_request(endpoint, api_key, tool_structured_body)
mock_response is {choices: [{message: {content: "{\"answer\":\"Sura is ready\",\"sources\":[]}"}}]}
parsed_answer is llm_extract_json(mock_response, answer_schema)
print parsed_answer.answer
usage is llm_usage({usage:{prompt_tokens: 10, completion_tokens: 4}})
print usage.total_tokens
cost is llm_cost({usage:{input_tokens:1000000, output_tokens:500000}}, {input_per_million:2, output_per_million:8})
print cost.total_cost
budget is llm_budget({usage:{input_tokens:1000000, output_tokens:500000}}, {input_per_million:2, output_per_million:8}, 7)
print budget.within_budget

chunk is json_stringify({choices: [{delta: {content: "Hi"}}]})
sse is "data: " + chunk + "\n\ndata: [DONE]\n\n"
print sse_parse(sse)[0].data
print llm_stream_text(sse)
```

`llm_request_schema(model, messages, schema, [temperature], [name], [strict])` and `llm_request_schema_json(...)` convert Sura's schema shorthand into OpenAI-compatible `response_format: {type: "json_schema"}` requests, so agents can ask for structured JSON without hand-writing full JSON Schema. `llm_tools([names])` builds OpenAI-compatible `tools` definitions from Sura's runtime tool schema metadata, and `llm_request_tools(...)`/`llm_request_tools_json(...)` attach those tools to a chat request. `llm_request_tools_schema(...)`/`llm_request_tools_schema_json(...)` combine both in one OpenAI-compatible request body. After a tool-call round, `llm_next_request(...)` and `llm_next_schema_request(...)` append the assistant/tool messages and build the next tool-enabled request in one call. `llm_chat(endpoint, api_key, model, messages, [temperature])` calls an OpenAI-compatible chat endpoint from model/messages, while `llm_chat_request(endpoint, api_key, request)` posts a prebuilt request dictionary or JSON body produced by any request helper. Use `llm_extract_text(response)` to pull assistant text from OpenAI-style `choices`, and `llm_extract_json(response, [schema])` to parse that text as JSON and optionally fail fast when it does not match a Sura schema. `llm_usage(response)` normalizes `prompt_tokens`/`completion_tokens` and `input_tokens`/`output_tokens` usage fields into one dictionary, `llm_cost(response, pricing)` calculates input/output/total costs from a caller-supplied per-million pricing table, and `llm_budget(response, pricing, limit)` adds budget-limit, remaining, and within/over-budget fields. Use `sse_parse(text)`/`json.sse_parse(text)`, `sse_data(text, [parse_json])`/`json.sse_data(text, [parse_json])`, and `llm_stream_text(text_or_chunks)` for OpenAI-style streaming responses that arrive as Server-Sent Events.

Regex, time, crypto, small database, logging, CLI parsing, and serialization helpers are also built in:

```sura
print regex_replace("sura-2026", "\\d+", "lang")
print regex_split("red, blue;green", "\\s*[,;]\\s*").join("|")
literal is regex_escape("a+b? [x]")
print regex_match("a+b? [x]", "^" + literal + "$")
parts is regex_capture("user=kim score=42", "user=([A-Za-z]+) score=([0-9]+)")
print parts[1] + ":" + parts[2]
print regex.captures("a=1 b=22", "([a-z])=([0-9]+)").len()
print datetime_now()
ts is datetime_parse("2026-05-16T12:34:56")
print datetime_format(ts, "%Y-%m-%d %H:%M")
print datetime_utc_format(0, "%Y-%m-%dT%H:%M:%SZ")
print datetime_diff(datetime_add(ts, 86400), ts)
print datetime_parts(ts).year
print sha256("abc")
file_write("payload.txt", "abc")
print crypto.file_sha256("payload.txt")
print crypto.file_hmac_sha256("secret", "payload.txt")
print crypto.random_hex(16)
print secure_random_hex(16)
print hmac_sha256("secret", "request body")
print crypto.constant_time_eq(hmac_sha256("secret", "request body"), hmac_sha256("secret", "request body"))
print hex_encode("sura")
print hex_decode("73757261")
print base64_encode("sura")
print base64_decode("c3VyYQ==")
print base64_url_encode("sura")
print base64_url_decode("c3VyYQ")
print url_encode("q=sura agent")
print url_decode("q%3Dsura%20agent")
print url_parse("https://api.example.com/search?q=sura").host
print url_build({scheme: "https", host: "api.example.com", path: "search", params: {q: "sura"}})
print http_status_ok(204)
print http_status_text(404)
print http_retry_after({"retry-after": "3"}, 0)
print http_backoff_delays(3, 100, 2, 1000).join(",")
print headers_get({"Content-Type": "application/json"}, "content-type")
print headers_has({"Retry-After": "2"}, "retry-after")
print http_content_type({"Content-Type": "text/plain; charset=utf-8"})
print http_charset("application/json; charset=utf-8")
print http_is_json("application/problem+json")
print query_build({q: "sura agent", tags: ["ai", "tools"], page: 1})
print query_parse("?q=sura+agent&tags=ai&tags=tools").tags.join(",")
print form_build({q: "sura agent", tags: ["ai", "tools"]})
print form_parse("q=sura+agent&tags=ai&tags=tools").tags.join(",")

db_set("data.json", "name", "sura")
print db_get("data.json", "name")
print db_has("data.json", "name")
print db_keys("data.json").join(",")
print db_all("data.json").name
print db_delete("data.json", "name")
db_insert("rows.json", {id: 1, name: "sura", kind: "agent-lang"})
db_insert("rows.json", {id: 2, name: "lua", kind: "embed"})
print db_find("rows.json", {kind: "agent-lang"})[0].name
print db_query("rows.json", {}, {sort: "id", desc: true, limit: 1})[0].name
print db_update("rows.json", {name: "sura"}, {score: 10})
print db_count("rows.json", {score: 10})
print db_remove("rows.json", {kind: "embed"})

args is cli_parse("--fast --target wasm --name \"Sura Agent\" --tag ai --tag tools --no-cache app.sura", ["target", "name", "tag"])
log_info(args.target)
log_set_file("agent.jsonl", false)
log_set_json(true)
log_event("info", "agent step", {request_id: "req-1", status: "ok"})
log_set_level("WARN")
log_info("debug detail hidden below WARN")
log_warn("slow response")
print log_get_level()
log_level("DEBUG")
log_set_file("", true)
print args.name
print args.tag.join(",")
print args.cache
print serialize(deserialize("{\"ok\":true}"))
```

Pass `value_flags` as an array, comma-separated string, or dictionary when a flag should consume the next token. Without it, `cli_parse` stays conservative for compatibility: `--name=value` always creates a value, while `--flag value` is parsed as a boolean flag plus positional argument unless `flag` is listed as value-taking. `datetime_parse(text, [format])` turns local datetime text into a Unix timestamp, `datetime_format` renders local time, `datetime_utc_format` renders UTC, `datetime_add(timestamp, seconds)` shifts a timestamp, `datetime_diff(end, start)` returns seconds between timestamps, and `datetime_parts(timestamp, [utc])` returns structured fields for logs, schedulers, and API payloads. Logging writes text lines by default; `log_set_file(path, false)` starts a fresh UTF-8 log file, `log_set_json(true)` switches to JSON Lines, `log_event(level, message, fields)` records structured fields for agents, servers, and CI, and `log_set_level(level)`/`log_level([level])` set or read the minimum emitted level (`TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`, or `OFF`). The console API covers browser-style output (`console.log`, `console.warn`, `console.table`, `console.timeLog`, `console.profileEnd`, `console.readLine`) plus terminal helpers: `console.style(text, ["bold", "green"])`, `console.color(text, fg, [bg])`, `console.strip_ansi(text)`, `console.set_color(fg, [bg])`, `console.reset_color()`, `console.is_tty()`, `console.width()`, `console.height()`, and `console.size()`.

The same practical helpers are available as namespaced standard modules for easier discovery in larger scripts. `use array`, `use set`, `use math`, `use path`, `use string`, `use os`, `use regex`, `use datetime`, `use crypto`, `use cli`, `use log`, `use console`, `use json`, `use dict`, `use fs`, `use db`, `use http`, `use async`, `use test`, `use random`, `use python`, `use ffi`, `use plugin`, `use vector`, `use graphics3d`, `use rag`, `use tensor`, `use stream`, `use tool`, and `use llm` create lightweight module objects that call the matching built-ins, so scripts can write `array.slice(...)`, `array.sum(...)`, `array.min(...)`, `array.max(...)`, `array.flatten(...)`, `array.range(...)`, `array.chunk(...)`, `array.zip(...)`, `set.union(...)`, `set.difference(...)`, `math.clamp(...)`, `path.join(...)`, `string.lines(...)`, `string.words(...)`, `string.pad_left(...)`, `string.chunks(...)`, `os.env_load(...)`, `os.wait(...)`, `os.home_dir()`, `os.temp_dir()`, `os.name()`, `os.path_separator()`, `os.which(...)`, `os.cmd_exists(...)`, `os.cmd_join(...)`, `os.run(...)`, `os.run_checked(...)`, `cli.parse(...)`, `json.try_parse(...)`, `json.pretty(...)`, `json.path(...)`, `json.has_path(...)`, `json.merge_patch(...)`, `json.delete_path(...)`, `json.set_path(...)`, `json.schema_to_json_schema(...)`, `json.template_render(...)`, `json.count_by(...)`, `json.sse_data(...)`, `json.ini_parse(...)`, `dict.keys(...)`, `dict.merge(...)`, `fs.read(...)`, `fs.read_json(...)`, `fs.read_bytes(...)`, `fs.sha256(...)`, `fs.glob(...)`, `regex.capture(...)`, `regex.captures(...)`, `crypto.file_sha256(...)`, `crypto.file_hmac_sha256(...)`, `crypto.random_hex(...)`, `crypto.constant_time_eq(...)`, `db.find(...)`, `log.set_level(...)`, `log.get_level()`, `log.level(...)`, `console.style(...)`, `console.color(...)`, `console.strip_ansi(...)`, `console.set_color(...)`, `console.reset_color()`, `console.size()`, `http.url_parse(...)`, `http.url_build(...)`, `http.status_text(...)`, `http.status_retryable(...)`, `http.retry_after(...)`, `http.backoff_delays(...)`, `http.content_type(...)`, `http.charset(...)`, `http.is_json(...)`, `http.request_json_checked(...)`, `http.request_retry_json_checked(...)`, `http.headers_merge(...)`, `http.headers_get(...)`, `http.headers_has(...)`, `http.headers_redact(...)`, `http.form_build(...)`, `http.form_parse(...)`, `http.cookie_parse(...)`, `http.cookie_build(...)`, `http.cookie_get(...)`, `http.auth_bearer(...)`, `test.summary()`, `test.approx(...)`, `random.seed(...)`, `python.call_json(...)`, `ffi.call(...)`, `plugin.call(...)`, `vector.search(...)`, `graphics3d.cube(...)`, `graphics3d.project(...)`, `rag.prepare(...)`, `tensor.matmul(...)`, `stream.batch(...)`, `stream.sum(...)`, `tool.spec(...)`, `llm.tools(...)`, `llm.request_schema_json(...)`, `llm.request_tools_json(...)`, `llm.request_tools_schema_json(...)`, `llm.chat_request(...)`, `llm.extract_json(...)`, `llm.tool_calls(...)`, `llm.tool_result(...)`, `llm.run_tools(...)`, `llm.next_messages(...)`, `llm.next_request(...)`, `llm.next_schema_request(...)`, and `llm.budget(...)` without giving up the direct builtin form. Existing aliases such as `use logging`, `use filesystem`, `use web`, `use time`, `use rng`, `use py`, `use testing`, `use g3d`, and `use graphics` resolve to those same modules.

`use vector` includes 3D game/math helpers on top of the generic vector API: `vector.vec3`, `vector.add3`, `vector.sub3`, `vector.dot3`, `vector.cross`, `vector.scale3`, `vector.norm3`, `vector.normalize3`, `vector.distance3`, `vector.neg3`, `vector.lerp3`, `vector.midpoint3`, `vector.project3`, `vector.reject3`, `vector.reflect3`, `vector.angle3`, and `vector.transform4`. Direct aliases such as `vec3_lerp(...)`, `vec3_reflect(...)`, and `vec3_transform4(...)` are also available for scripts that do not use module objects.

`use graphics3d` adds 3D scene-data helpers that are shared by the native runtime and JS target: `graphics3d.identity`, `graphics3d.translate`, `graphics3d.scale`, `graphics3d.rotate_y`, `graphics3d.mul`, `graphics3d.cube`, `graphics3d.transform`, `graphics3d.bounds`, `graphics3d.face_normals`, and `graphics3d.project`. Direct built-ins (`mat4_identity`, `mat4_translate`, `mat4_scale`, `mat4_rotate_y`, `mat4_mul`, `mesh_cube`, `mesh_transform4`, `mesh_bounds`, `mesh_face_normals`, and `camera_project`) are available for compact scripts and tests.

## Package Manager

Build the package manager with:

```powershell
.\build.bat
```

Common commands:

```powershell
.\surapkg.exe init my_app --json init-report.json
.\surapkg.exe create math_extra --json create-report.json
.\surapkg.exe agent demo_agent
.\surapkg.exe embed game_host
.\surapkg.exe install .\math_extra
.\surapkg.exe outdated --json
.\surapkg.exe update math_extra
.\surapkg.exe lock
.\surapkg.exe verify
.\surapkg.exe sign .\math_extra
.\surapkg.exe verify .\math_extra
.\surapkg.exe sign-policy .\math_extra
.\surapkg.exe verify-policy .\math_extra
.\surapkg.exe trust-key maintainer-2026 .\keys\public.pem
.\surapkg.exe resolve --json
.\surapkg.exe index
.\surapkg.exe search math --json
.\surapkg.exe stats math_extra --json
.\surapkg.exe analytics math_extra --json
.\surapkg.exe registry-health --fail-on-warning --json
.\surapkg.exe recover-token alice <recovery-code> alice-new-token --json recover-token-report.json
.\surapkg.exe doctor
.\surapkg.exe list --json
.\surapkg.exe info math_extra
.\surapkg.exe format .\math_extra --check --json format-report.json
.\surapkg.exe check .\math_extra --json check-report.json
.\surapkg.exe lint .\math_extra --json lint-report.json
.\surapkg.exe profile .\math_extra --json profile.json
.\surapkg.exe bench .\math_extra --json bench-report.json --python bench_math_extra.py --min-speedup 1.1
.\surapkg.exe audit .\math_extra
.\surapkg.exe policy .\math_extra --json policy-report.json
.\surapkg.exe tool-log .\tool-audit.jsonl --fail-on-denied --json .\tool-log-report.json
.\surapkg.exe bind-c .\native.h --out .\native.ffi.sura --lib .\native.dll --prefix native_ --json bind-report.json
.\surapkg.exe docs docs_site --json docs-report.json
.\surapkg.exe test
.\surapkg.exe test . --json sura-test-report.json --junit sura-test-report.xml
.\surapkg.exe run . --json run-report.json -- --mode=fast input.txt
.\surapkg.exe ci .\math_extra
.\surapkg.exe protect .\math_extra --out .\dist\math_extra.sura.srp --key-file .\customer.key --license-file .\seat.license
.\surapkg.exe protect .\math_extra --closed-source --key-file .\customer.key --license-file .\seat.license --require-expires --expires 2027-12-31
.\surapkg.exe protect .\math_extra --out .\dist\math_extra.sura.srp --json .\dist\math_extra.protect.json
.\surapkg.exe protect .\math_extra --out .\dist\math_extra.sura.srp --exe .\dist\math_extra.exe
.\surapkg.exe protect-verify .\dist\math_extra.protect.json --require-closed-source --require-key --require-license --require-expires --require-target package --json .\dist\math_extra.protect-verify.json
.\surapkg.exe publish .\math_extra
.\surapkg.exe index --json registry-index-report.json
.\surapkg.exe verify-registry .\registry
.\surapkg.exe owners math_extra --json
.\surapkg.exe yanks math_extra --json
.\surapkg.exe yank math_extra@0.1.0 "bad release" --json
.\surapkg.exe unyank math_extra@0.1.0 --json
.\surapkg.exe report math_extra@0.1.0 "suspicious package behavior" --json
.\surapkg.exe reports open --json
.\surapkg.exe review-report <report-id> actioned "yanked after review" --json
.\surapkg.exe restore
.\surapkg.exe remove math_extra --json remove-report.json
```

`surapkg examples [query] [--json]` discovers the bundled `.sura` gallery from the checkout, installed runtime, or `SURA_EXAMPLES`, emits schema `sura.package.examples.v1`, and reports inferred optional requirements. `surapkg example <id> <project-directory> [--json report.json]` copies the selected source byte-for-byte into a runnable package with VS Code files and `sura.example.provenance.v1` SHA-256 provenance; its scaffold report uses schema `sura.package.example.v1`.

`surapkg` writes `sura.pkg.json`, installs packages under `packages/<name>`, records dependency state in `sura.lock.json`, verifies installed package hashes with `surapkg verify`, and can publish packages into a registry. Set `SURA_REGISTRY` to choose the registry root; otherwise `./registry` is used. Dependency specs support exact versions plus semver-style constraints such as `^1.2`, `~1.2`, and `>=1.0 <2.0`; `surapkg resolve`, `restore`, `lock`, `tree`, `why`, `outdated`, and `update` choose non-yanked registry versions that satisfy the constraint. `surapkg resolve [--json]` walks transitive dependencies, reports incompatible version ranges, and can emit a CI-readable graph with resolved packages, requirements, sources, and errors. `surapkg restore` installs the resolved direct and transitive package graph without adding transitive packages to the root manifest, `surapkg lock` records the full installed graph, `surapkg list [--json]` inventories stdlib modules including builtin module objects such as `cli`, `json`, and `fs`, installed packages, package-file modules, and manifest dependencies, `surapkg info cli/json/fs [--json]` exposes builtin module API metadata for editor and registry UIs, `surapkg tree [--json]` prints schema `sura.package.tree.v1` for the resolved direct/transitive dependency tree, and `surapkg why <name> [--json]` emits schema `sura.package.why.v1` for the root-to-package path of a direct or transitive dependency. `surapkg clean [path] [--dry-run] [--json clean-report.json]` safely removes known generated build/test logs, `sura_world_*` smoke temp files, and top-level `sura_walk_*` directories without touching source, tests, packages, or executables; the JSON report emits schema `sura.package.clean.v1`. `surapkg run [path] [--json run-report.json] [-- args...]` executes the package manifest's `main` file from the package root, enables JIT by default, exposes forwarded arguments through `argv()` and `argc()`, and emits schema `sura.package.run.v1` for CI execution dashboards. `surapkg bench [path]` runs the same manifest `main` through interpreter and JIT `--bench`, writes optional CI-readable JSON with `--json`, writes release-note-ready Markdown with `--summary`, forwards script arguments after `--`, can compare against a Python reference with `--python`, and can fail a performance gate with `--min-speedup`. `surapkg ci` and `surapkg release` automatically run that gate when the manifest declares `bench`, `bench_min_speedup`, `bench_python`, or `bench_report`, so packages can publish reproducible performance evidence; generated docs include a benchmark summary when the configured report exists. When the manifest declares `audit_report`, generated docs also include the saved security audit pass/finding summary and searchable audit findings. `surapkg info name@version [--json]` shows local package symbols, file-backed stdlib symbols, or builtin module symbols, and when `SURA_REGISTRY_URL` is set it can fetch remote registry detail for packages that are not installed locally, including owner, bundle path, API symbols, and benchmark evidence; `--json` emits schema `sura.package.info.v1` for registry UIs and dashboards. `surapkg search [query] [--json]` searches the local or HTTP registry index and can emit package metadata for registry UIs, `surapkg outdated [name] [--json]` compares installed packages against the registry index and can emit a CI-readable update report, while `surapkg update [name]` installs the latest allowed registry version while preserving range specs in `sura.pkg.json`. `surapkg quality [path]` scores package release readiness across manifest metadata, main/source layout, tests, README/docs, dependency lock/install state, security audit findings, and package signatures. `surapkg ci [path]` is the non-publishing CI gate: it generates package docs, runs package tests, runs any configured package benchmark gate, refreshes docs with benchmark evidence, audits risky APIs, verifies existing package and tool-policy signatures when present, then enforces the quality score. `surapkg release [path]` generates package docs, runs tests, runs any configured package benchmark gate, refreshes docs with benchmark evidence, signs the package, enforces the quality gate, and publishes only when every gate passes. `surapkg verify-registry [path]` checks local registry index hashes, package bundles, signatures, and owner/yank/report/advisory metadata references, and `--json registry-verify.json` writes package counts plus structured warning/error findings; with `SURA_REGISTRY_URL` and no path it verifies a remote HTTP registry's index, non-yanked bundles, signatures, and advisory references. `surapkg registry-health [path] [--fail-on-warning] [--json]` writes schema `sura.registry.health.v1` for local or HTTP registry operations, including package, owner, yank, report, advisory, active/critical advisory, stats, analytics, key-store, and endpoint check summaries; `--fail-on-warning` turns warning checks into a non-zero CI gate while keeping JSON output parseable. `surapkg owners [name] [--json]` lists package namespace ownership for local or HTTP registries, `surapkg yanks [name] [--json]` lists yanked package versions for dashboards and incident review, `surapkg advisory <name[@version]> --severity high --title "Unsafe API" --description "details" [--url URL] [--json]` creates local or HTTP registry security advisories with schema `sura.registry.advisory.v1`, `surapkg advisories [name[@version]] [--severity high] [--status active] [--json]` lists local or HTTP registry security advisories with schema `sura.registry.advisories.v1` plus active-advisory `next_actions` for CI and dashboard remediation, `surapkg yank <name@version> <reason> [--json]` and `surapkg unyank <name@version> [reason] [--json]` update local or HTTP registry yanked metadata and rebuild the index; `surapkg report <name[@version]> <reason> [--json]` writes local reports or submits HTTP registry abuse reports for moderation and emits schema `sura.registry.report.v1`; `surapkg reports [status] [--json]` and `surapkg review-report <id> <status> [note] [--json]` make the moderation queue scriptable for registry dashboards and admin automation.

The hosted registry API also exposes filtered admin queues with `/api/reports?status=open&limit=100`, per-status queue counts in `/health`, append-only `sura.registry.moderation_event.v1` JSONL audit events, and actioned-report yanks for abuse response.

For deployable registry service operations, `deploy/registry/` includes Docker, Compose, systemd, and environment templates. `tools/sura_registry_service_smoke.ps1` verifies `SURA_REGISTRY_HOST` startup, the `/health` schema, admin abuse-report queues, filtered review summaries, moderation audit logs, actioned-report yanks, and `registry-health --fail-on-warning` against the running service.

Remote HTTP registry package detail preserves configured `audit_report` artifacts alongside docs and benchmark artifacts. `GET /api/package/<name>/<version>` returns the parsed audit JSON, browser package pages render a Security Audit summary plus up to 50 findings, and `surapkg info name@version --json` exposes `audit.present`, `audit.passed`, and `audit.finding_count` for dashboards.

`surapkg version [path] [major|minor|patch|version] --json version-report.json` shows or updates `sura.pkg.json` package versions with schema `sura.package.version.v1`, so release scripts can bump package versions before publish.

During `install`, `restore`, and `update`, registry packages with active low/moderate/high advisories print warnings, while active critical advisories block installation by default. Set `SURA_ALLOW_CRITICAL_ADVISORY_INSTALL=1` only for an explicit emergency override. `surapkg audit` also checks installed manifest dependencies against active registry advisories: high and lower severities are reported as warnings, and critical dependency advisories fail the audit with kind `registry_advisory` in JSON/SARIF-style reports.

For CI and registry dashboards, `surapkg advisories [name[@version]] --fail-on high --json` exits non-zero when matching active high or critical advisories exist, while `--fail-on critical` only fails on active critical advisories and `--fail-on active` fails on any matching active advisory. The JSON report includes `passed`, `fail_on`, `failing_count`, `next_actions`, and the matched advisory records so release jobs can block and show the remediation path without scraping text output.

`surapkg doctor [path] --json doctor-report.json` checks platform, engine discovery, Windows and Unix-style native build readiness, available C++ compiler, PowerShell runner, stdlib files, package manifest/main/dependencies/lockfile, registry state, and editor/test helper availability. This is the quickest way to catch a machine that can run Sura scripts but cannot yet build native hosts, plugins, or cross-platform package workflows. `.github/workflows/cross-platform-smoke.yml` builds Sura on Ubuntu and macOS and runs stdlib, JS/WASM target, Python bridge, protected-release, package metadata, quality/CI/release/update package gates, plugin policy, and native embedding smokes.

`surapkg search` keeps registry package matches in `packages`/`count` and also returns builtin standard-library hits in `stdlib`/`stdlib_count`, so queries such as `cli.parse`, `json.path`, `json.pretty`, `json.sse_data`, `json.ini_parse`, `os.temp_dir`, `os.which`, `os.cmd_join`, `os.run`, `os.run_checked`, `fs.read`, `fs.read_json`, `fs.read_bytes`, `fs.sha256`, `fs.glob`, `regex.escape`, `regex.capture`, `crypto.file_sha256`, `crypto.file_hmac_sha256`, `crypto.random_hex`, `crypto.constant_time_eq`, `http.request_json_checked`, `http.status_text`, `plugin.call`, `vector.search`, `llm.request_json`, or `llm.budget` can be discovered from the package CLI without opening generated docs.

Use `surapkg install <spec> --json install-report.json` to write a UTF-8 JSON install report with package identity, resolved version, source, destination, registry/remote flags, and dependency-record status for CI artifacts.

Use `surapkg init [name] --json init-report.json` and `surapkg create <name> --json create-report.json` to record generated package scaffolds with schema, package name, root, manifest path, main source path, and generated file list.

Use `surapkg update [name] --json update-report.json` to write a UTF-8 JSON update report with each applied package update, previous/installed versions, dependency constraint, registry bundle, hash, and owner metadata.

Use `surapkg restore --json restore-report.json` to write a UTF-8 JSON restore report with every resolved direct/transitive dependency, source, destination, action, registry/file flags, and install status.

Use `surapkg remove <name> --json remove-report.json` to write a UTF-8 JSON removal report with package name, install state, removed entry count, manifest dependency removal state, and destination path.

Use `surapkg lock --json lock-report.json` to write a UTF-8 JSON lock report with the generated lockfile path, package count, resolved package versions, requirement summaries, sources, and package hashes.

Use `surapkg verify [path] --json verify-report.json` to write a UTF-8 JSON verification report for either the current `sura.lock.json` or a package signature, including expected/actual hashes and per-package status.

Use `surapkg docs [outdir] --json docs-report.json` to write a UTF-8 JSON docs report with package identity, generated HTML/API/search-index paths, file sizes, package symbol count, builtin stdlib module/API counts, search entry count, search entry type counts, tool/plugin policy presence, benchmark, audit, and quality-report presence, audit pass/finding counts, quality pass/score fields, and policy/benchmark/audit/quality search entry counts.

For closed-source distribution, `surapkg protect [path]` reads the package manifest's `main` file and compiles it to a protected `.sura.srp` release package under `dist/<name>-<version>.sura.srp` by default. New release artifacts use the hardened v5 container: source line/local/function/parameter symbol metadata is stripped, string constants live only inside the encoded bytecode payload, raw bytecode headers are sealed, each nonce is randomized so identical source produces different artifacts, and sealed payload checks reject tampered package bytes before execution. `protect` also runs a UTF-8-safe leak scan by default and writes `<artifact>.protect.json`; the scan fails the command if package source snippets, literal strings, release keys, or license values are found as raw bytes in the `.sura.srp` or generated launcher. Use `--closed-source` for customer/private builds: it refuses to build unless a non-empty key and license are supplied and the leak scan remains enabled. Add `--require-expires` when the build must include an expiration date. The protect report emits schema `sura.package.protect.v1` and records `passed`, `mode`, `keyed`, `licensed`, and `expires` so CI can prove the release was built with the expected controls. `surapkg protect-verify <protect-report.json> --require-closed-source --require-key --require-license --require-expires --require-target package --json protect-verify.json` rechecks that evidence later, verifies target files still exist, and emits schema `sura.package.protect_verify.v1` with failed-check `next_actions` for CI. Use `--json file.json` or legacy `--scan-report file.json` to choose the report path; use `--no-leak-scan` only for a trusted internal build. Protected package inspection is locked by default: `--dump`, `--trace`, and `--debug-protocol` are rejected for `.sura.srp` execution unless the owner explicitly sets `SURA_ALLOW_RELEASE_INSPECT=1` in a trusted debug environment. Add `--exe dist/app.exe` to build a stripped native Windows launcher whose embedded package bytes are obfuscated before compilation; ship `SuraLanguage.exe` next to the launcher or set `SURA_ENGINE` at runtime. It supports `--key`, `--key-file`, `--license`, `--license-file`, `--id`, and `--expires`; runtime loading accepts matching `--load-release-key-file` and `--load-release-license-file` as well as environment variables. This is practical source non-disclosure, not mathematical DRM; native tools can still be reverse engineered.

Package manifests can declare `protect_report` and optional `protect_verify_report` so `surapkg ci` and `surapkg release` automatically run `protect-verify` as a stage. Set `protect_require_closed_source`, `protect_require_key`, `protect_require_license`, `protect_require_expires`, and `protect_require_target` when the release gate must prove customer/private build controls before publish.

`surapkg agent <name> [--json agent-report.json]` creates a ready-to-run AI automation agent template with `src/<name>.sura`, `tests/agent_test.sura`, a source-grounded `knowledge/` folder, `sura.tools.json`, and package metadata. The optional JSON report uses schema `sura.package.agent.v1` and records the generated main/test/policy/knowledge files for CI scaffolding dashboards. The generated agent uses namespaced standard modules: `llm.messages`, `llm.request_tools_schema_json`, `llm.chat_request`, `llm.extract_json`, `llm.request_schema_json`, source-aware `rag.prepare`, JSON-schema-style `json.schema_errors`, `tool.spec("http_request", ...)`, `tool.call_policy`, `llm.next_messages`, and `llm.next_schema_request` against local `file://` knowledge documents so it works without an API key while keeping URL prefixes and HTTP methods explicit. Send the generated tool-enabled structured body with `llm.chat_request(endpoint, api_key, request)` when connecting a live OpenAI-compatible endpoint; use `llm.next_messages(messages, response, policy)` for tool-call turns or `llm.next_schema_request(...)` when you want the next tool-enabled structured request immediately; parse final structured output with `llm.extract_json(response, agent_output_schema())`; the template keeps a separate schema-constrained body example for structured output, or you can use `llm.chat(endpoint, api_key, model, messages)` for simple direct calls.

Publishing creates a `sura.pkg.sig` signature, an unpacked package directory, and an HTTP-friendly `package.surabundle.json`; use `surapkg publish [path] --dry-run --json publish-dry-run.json` to validate manifest copying, signing, and bundle generation without writing the registry or uploading, then `surapkg publish [path] --json publish-report.json` to write package identity, registry destination, bundle path, bundle hash, dry-run state, and remote upload status for CI artifacts. Set `SURA_SIGNING_PRIVATE_KEY` to an OpenSSL-compatible PEM private key before `surapkg sign`, `surapkg sign-policy`, `surapkg publish`, or `surapkg release` to create RSA-SHA256 public-key signatures; `surapkg sign [path] --json sign-report.json` and `surapkg sign-policy [path] --json sign-policy-report.json` record generated signature paths, algorithms, key ids, and hashes for CI artifacts. Set `SURA_SIGNING_PUBLIC_KEY` when verifying one key directly, or run `surapkg trust-key <key-id> <public-key.pem> --json trust-key-report.json` to store trusted keys under `SURA_SIGNING_PUBLIC_KEY_DIR` or `registry/keys/<key-id>.pem` while recording the trust store, destination, and PEM hash. `verify-registry` discovers local `registry/keys` and remote `<registry-url>/keys/<key-id>.pem`, so registries can rotate keys by publishing multiple PEM files and labeling packages with `SURA_SIGNING_KEY_ID`. Set `SURA_REQUIRE_PUBLIC_SIGNATURE=1` in CI when unsigned/keyed-only packages must fail. The older `SURA_SIGNING_KEY` keyed signature mode still works for local/private registries; without either key, signatures verify only integrity hashes. `surapkg sign-policy [path]` writes `sura.tools.sig` for `sura.tools.json`, and `surapkg verify-policy [path] --json verify-policy-report.json` checks that signed tool policies were not changed while writing the manifest/signature paths, algorithm, key id, expected/actual hashes, pass status, and message. `surapkg index [--json registry-index-report.json]` rebuilds `registry/index.json` and can emit schema `sura.registry.index.v1` with package/yank counts and indexed bundle metadata. `surapkg search [query] [--json]` searches the local or HTTP registry index, `surapkg stats [name] [--json]` shows download/publish counts from local `stats.json` or the HTTP registry stats API, `surapkg analytics [name] [--json]` shows top packages plus daily download/publish trends, and `surapkg doctor [path] --json doctor-report.json` diagnoses the local engine, stdlib, package manifest/main file, dependency install state, registry configuration, and optional tooling such as curl, Node, OpenSSL, and signature hash tools with a structured status report.

Run a local central-registry server:

```powershell
.\tools\sura_registry_server.ps1 -Root .\registry -Port 8765 -Token dev-token -AdminToken admin-token
$env:SURA_REGISTRY_URL = "http://localhost:8765"
$env:SURA_REGISTRY_TOKEN = "dev-token"
.\surapkg.exe publish .\math_extra
.\surapkg.exe install math_extra
```

When Node is available, `sura_registry_server.ps1` starts the tokenized registry API. It supports `POST /api/publish`, `POST /api/tokens`, `POST /api/tokens/recover`, `POST /api/yank`, `POST /api/report`, `GET /api/reports`, `POST /api/reports/review`, admin security advisories through `POST /api/advisories` and `surapkg advisory`, public advisory listings through `GET /api/advisories?name=<name>&version=<version>` and `surapkg advisories`, `GET /api/owners`, `GET /api/yanks`, `GET /index.json`, `GET /api/package/<name>`, `GET /api/package/<name>/<version>`, browser package listing/search at `GET /` and `GET /packages?q=<query>`, browser package pages at `GET /package/<name>` and `GET /package/<name>/<version>`, `GET /api/stats`, `GET /api/analytics`, hosted public keys under `/keys/<keyId>.pem`, and package bundle downloads. The package detail API exposes manifest, README, docs API/search-index metadata, configured benchmark report data, owner, hash, yank state, and security advisories for registry UIs and `surapkg info`; the HTML registry pages render searchable package lists, package identity, links, API symbols, README, manifest, benchmark evidence, yanked state, and active advisory counts for humans. Use the admin token to mint per-user publish tokens; the response includes a one-time recovery code that can rotate lost tokens:

```powershell
$body = @{ user = "alice"; token = "alice-token" } | ConvertTo-Json
Invoke-RestMethod http://localhost:8765/api/tokens -Method Post -Headers @{ Authorization = "Bearer admin-token" } -ContentType "application/json" -Body $body
$env:SURA_REGISTRY_TOKEN = "alice-token"
.\surapkg.exe publish .\math_extra
.\surapkg.exe recover-token alice <recovery-code> alice-new-token --json recover-token-report.json
```

The first successful publish claims the package owner. Later publishes of the same package name must use the owner's token or an admin token, so another account cannot overwrite the package namespace. `surapkg recover-token` posts a one-time recovery code to `/api/tokens/recover`, revokes that user's old tokens, returns a replacement token, issues the next recovery code, and can write a UTF-8 JSON recovery report with `--json` for CI vault handoff. Versioned installs use `name@version`; unversioned installs use the registry `latest` alias. Use `-Static` to serve a read-only registry with Python.

Admins can yank a bad package version. Yanked versions remain visible in `index.json` and `/api/yanks`, but bundle downloads return HTTP `410`, so `surapkg install name@version` fails instead of installing known-bad content:

```powershell
$env:SURA_REGISTRY_TOKEN = "admin-token"
.\surapkg.exe yank math_extra@0.1.0 "bad release" --json
.\surapkg.exe yanks math_extra --json
.\surapkg.exe unyank math_extra@0.1.0 --json
```

Without `SURA_REGISTRY_URL`, the same commands update local `SURA_REGISTRY` yanked metadata and rebuild `index.json`.

Users can report suspicious packages, and registry admins can review the queue:

```powershell
.\surapkg.exe report math_extra@0.1.0 "suspicious package behavior"
$env:SURA_REGISTRY_TOKEN = "admin-token"
.\surapkg.exe reports open --json
.\surapkg.exe review-report <report-id> actioned "yanked after review" --json
```

`surapkg audit` scans package source for high-risk APIs such as shell execution, file deletion, Python bridging, native FFI/plugin calls, network calls, and direct unpolicyed tool calls; use `surapkg audit . --json audit-report.json` to write a CI-readable UTF-8 JSON report with `passed`, `finding_count`, and structured finding records, or `surapkg audit . --sarif audit.sarif` to emit SARIF 2.1.0 for GitHub/code-scanning security dashboards. It validates `sura.tools.json` package tool policies: allowed tool names, URL prefixes for `http_get`/`http_request`, `http_methods`, `allowed_headers`, `required_headers`, `max_body_bytes`, `max_timeout` for `http_request`, approval gates via `approval`, optional `approval_token`/`approval_message`, explicit `allow_shell`, command prefixes for shell tools, and `sura.tools.sig` integrity/keyed signatures when present. `surapkg policy [path] --json policy-report.json` scans policy-aware `tool_spec(...)` and `tool.spec(...)` usage for `http_get`/`http_request`, writes a starter `sura.tools.json`, and records inferred tools, URL prefixes, HTTP methods, headers, and shell-manual-review state; shell tool policies still require manual `command_prefixes` so broad command execution is never auto-enabled. At runtime, `tool_call_policy(spec, policy)` and `tool.call_policy(spec, policy)` enforce `approval: true` before executing the tool: use a matching `SURA_TOOL_APPROVAL_TOKEN`, `SURA_TOOL_APPROVAL=allow` for tokenless policies, `SURA_TOOL_APPROVAL_REQUEST_FILE` plus `SURA_TOOL_APPROVAL_RESPONSE_FILE` for editor/embedded UI JSON approval bridges, `SURA_TOOL_APPROVAL_COMMAND` for a host approval command that receives `SURA_TOOL_APPROVAL_TOOL`, `SURA_TOOL_APPROVAL_TARGET`, `SURA_TOOL_APPROVAL_MESSAGE`, and `SURA_TOOL_APPROVAL_TOKEN_CONFIGURED`, `SURA_TOOL_INTERACTIVE_APPROVAL=1` for a prompt, or `SURA_TOOL_AUTO_APPROVE=1` in trusted CI. The file bridge writes a UTF-8 JSON request containing `requestId`, `tool`, `target`, `message`, and `approvalTokenConfigured`, then waits for a matching response JSON containing the same `requestId` plus `decision: "allow"` or `allow: true`; `SURA_TOOL_APPROVAL_FILE_TIMEOUT_MS` controls the wait. Set `SURA_TOOL_AUDIT_LOG=path/to/tool-audit.jsonl` to append UTF-8 JSONL events for `policy_denied`, `policy_allowed`, `approval_denied`, `approval_granted`, `executed`, and `execution_failed` without logging approval tokens; `surapkg tool-log <file> [--tail n] [--fail-on-denied] [--json tool-log-report.json]` summarizes those logs, writes a CI-readable UTF-8 JSON summary with event/tool counts and recent events, and can fail CI when denied or failed events are present while still leaving the report artifact. It also validates package-level `sura.plugins.json` plugin policies plus `*.sura-plugin.json` manifests for relative in-package library paths, required name/version/SHA256/export fields, matching plugin file hashes, and non-empty export allow-lists; raw `plugin_load`/`plugin.load` and unlisted `plugin_load_manifest`/`plugin.load_manifest` calls fail audit. `surapkg docs [outdir] --json docs-report.json` creates a searchable HTML docs site from package metadata, an API Reference table for `func`/`class`/`struct`/`enum` declarations and top-level literal constants with signatures and source locations, a Standard Library Modules table for builtin module APIs such as `cli.parse`, `json.path`, `fs.read`, `plugin.call`, `vector.search`, `rag.prepare`, `tool.spec`, and `llm.request_json`, a rendered Search Index table for symbols/stdlib/policies/benchmarks/tests/audit/quality findings, any package tool and plugin policy summaries, any configured benchmark report summary, any configured `test_report` summary, any configured `audit_report` security summary, any configured `quality_report` readiness summary, `docs/api.json` for registry UIs/LSP/CI, `docs/search-index.json` for package docs search, and a CI-readable UTF-8 docs artifact report. `surapkg quality` is a CI-friendly release gate that fails packages under 80/100 or with required-check failures; `surapkg quality [path] --json quality-report.json` also writes a UTF-8 JSON report with schema, score, grade, pass/fail status, warning/error counts, category/action metadata for each item, and `next_actions` for dashboards or CI artifacts. `surapkg ci` combines docs, tests, configured benchmark gates, audit, existing package/tool-policy signature verification, and quality scoring without publishing. `surapkg release` combines docs, tests, configured benchmark gates, signing, quality scoring, and publishing into one guarded release path.

Plugin policies can also narrow native access with package-level `allowed_exports`, `host_capabilities`, `max_memory_bytes`, and `max_call_ms`; `surapkg audit` checks literal `plugin.call` export names, host capability declarations, and resource quota fields so packaged native plugins only expose the entry points, host callbacks, host allocator budget, and native call duration budget they actually need.

Use `surapkg ci [path] --json ci-report.json` to write a UTF-8 JSON CI report with `schema`, package identity, overall `passed` status, failure-specific `next_actions`, and stage records for docs, format, check, lint, tests, test-report capture, benchmark gates, configured protect verification, audit, signature verification, and quality. Unsigned CI runs also persist `artifacts/ci-test.json`, configured `artifacts/ci-protect-verify.json`, `artifacts/ci-audit.json`, and `artifacts/ci-quality.json` and refresh generated docs so the HTML, `api.json`, and `search-index.json` include Test, Security Audit, and Quality summaries; signed CI packages use temporary test/audit/quality/protect verification reports to preserve the verified package hash.

Use `surapkg release [path] --dry-run --json release-dry-run.json` to run the full release gate and validate publish staging without writing the registry or uploading. Use `surapkg release [path] --json release-report.json` to write a UTF-8 JSON release report with package identity, overall `passed` status, `dry_run` state, failure-specific `next_actions`, and stage records for docs, format, check, lint, tests, test-report capture, benchmark gates, configured protect verification, audit, signing, quality, docs audit/quality refreshes, final signing, and publishing. Release runs persist `artifacts/release-test.json`, configured `artifacts/release-protect-verify.json`, `artifacts/release-audit.json`, and `artifacts/release-quality.json`, refresh generated docs with test, security, and readiness summaries, and then refresh the package signature so the published bundle includes the final release evidence.

## Developer Tools

The `tools` directory contains focused scripts for day-to-day language work:

```powershell
.\SuraLanguage.exe --check .\tests
.\SuraLanguage.exe --lint .\tests
.\SuraLanguage.exe --format-check .\tests
.\SuraLanguage.exe --format .\examples\hello.sura
.\tools\sura_format.ps1 -Path .\tests\test_world_features.sura -Check
.\tools\sura_lint.ps1 -Path .\tests\test_world_features.sura
.\tools\sura_test.ps1 -Path . -Report sura-test-report.json -TimeoutSeconds 120
.\tools\sura_test.ps1 -Path . -Report sura-test-report.json -JUnit sura-test-report.xml -TimeoutSeconds 120
.\tools\sura_aot.ps1 -Source .\examples\hello.sura
.\tools\sura_to_js.ps1 -Source .\examples\hello.sura -Out hello.js
.\tools\sura_to_wasm.ps1 -Source .\examples\hello.sura -Out hello.wat
.\tools\bindgen_c.ps1 -Header native.h -Out native.ffi.sura
.\tools\sura_bench_dashboard.ps1 -Out bench_dashboard.html -SummaryOut bench_summary.md -ReleaseNotesOut bench_release_notes.md -NativePerfIn artifacts\native_perf.json
.\tools\sura_bench_dashboard_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_jit_mod_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_registry_server.ps1 -Root .\registry -Port 8765
.\tools\sura_doctor_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_policy_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_quality_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_ci_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_release_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_release_pack_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_pkg_format_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_scaffold_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_check_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_lint_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_profile_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_bench_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_tree_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_restore_lock_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_docs_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_pkg_search_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_test_runner_smoke.ps1 -Surapkg .\surapkg.exe -Engine .\SuraLanguage.exe
.\tools\sura_pkg_run_smoke.ps1 -Surapkg .\surapkg.exe -Engine .\SuraLanguage.exe
.\tools\sura_http_server_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_stdlib_modules_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_random_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_agent_smoke.ps1 -Surapkg .\surapkg.exe -Engine .\SuraLanguage.exe
.\tools\sura_profile_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_python_bridge_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_check_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_engine_lint_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_engine_format_smoke.ps1 -Engine .\SuraLanguage.exe
.\tools\sura_registry_client_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_registry_account_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_registry_report_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_registry_service_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_registry_verify_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_public_signature_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_update_smoke.ps1 -Surapkg .\surapkg.exe
.\tools\sura_bind_c_smoke.ps1 -Surapkg .\surapkg.exe -Engine .\SuraLanguage.exe
.\tools\sura_embed_smoke.ps1
.\tools\sura_ffi_safety_smoke.ps1
```

Runtime and package type checking are strict by default: diagnostics stop the
program before its body runs. `--legacy-types` is an explicit migration-only
warning-and-run mode, and `--compile`/protected `--release` artifacts never permit that bypass.
`surapkg check` follows the same strict default and records the selected mode in
its JSON report.

Each PowerShell test runner applies a 120-second default deadline per `.sura` file. `-TimeoutSeconds` overrides it. A timeout terminates the engine process tree, uses exit code `124`, records `timedOut: true` in JSON, and writes an explicit JUnit timeout failure. `run_stable_tests.ps1` also accepts `-TestPath` and `-Engine` for isolated regression checks.

`SuraLanguage.exe --check` parses and typechecks files or directories without running them. Parser diagnostics recover at line boundaries, so check mode can report multiple syntax errors from the same file before returning failure. `SuraLanguage.exe --lint` checks block balance and warns on risky APIs such as network calls, deletion, Python bridging, FFI, direct tool calls, weak tool policies with empty bodies or HTTP tools without `url_prefixes`, and unredacted sensitive HTTP headers being printed or logged. `SuraLanguage.exe --format` formats files in place, while `--format-check` verifies formatting without writing. `surapkg format [path]` formats package sources, supports `--check`, writes optional CI-readable JSON with `--json`, and is part of `surapkg ci` and `surapkg release`. `surapkg check [path]` runs package-level parse/typecheck, writes optional CI-readable JSON with `--json`, supports `--strict`, and is part of `surapkg ci` and `surapkg release`. `surapkg lint [path]` runs package-level lint, writes optional CI-readable JSON with `--json`, can fail on warnings with `--fail-on-warning`, detects unredacted sensitive-header logging as `unredacted_sensitive_headers` and weak empty or prefixless HTTP tool policies as `weak_tool_policy`, and is part of `surapkg ci` and `surapkg release`. `surapkg profile [path]` runs the package main through the runtime profiler, supports `--json`, `--no-jit`, and forwarded script arguments. `surapkg bench [path]` runs package-level interpreter/JIT benchmarks, supports `--json`, `--summary`, `--no-jit`, `--python`, `--min-speedup`, and forwarded script arguments. `surapkg clean [path]` safely removes known generated logs and temporary smoke outputs, supports `--dry-run`, and writes schema `sura.package.clean.v1` with `--json`. `sura_test.ps1` discovers `tests/*.sura` or `test_*.sura` files, runs them through the engine with JIT by default, and writes `sura-test-report.json` plus optional JUnit XML with `-JUnit` for CI. `surapkg test` provides the same package-level workflow without leaving the package manager; use `--json` or legacy `--report`, `--junit`, and `--no-jit` to choose report paths or compare interpreter/JIT behavior; JSON reports emit schema `sura.package.test.v1`. `sura_pkg_scaffold_smoke.ps1` verifies `surapkg init/create --json` scaffold reports and generated package files. `sura_pkg_format_smoke.ps1` verifies package format checks, JSON reports, rewrite mode, and post-format pass. `sura_pkg_check_smoke.ps1` verifies clean package checks, strict-mode JSON reporting, and broken source failures. `sura_pkg_lint_smoke.ps1` verifies clean package lint, risky-API warnings including unredacted sensitive-header logging, weak tool-policy warnings, JSON reports, structural failures, and warning-as-error mode. `sura_pkg_profile_smoke.ps1` verifies package-level text and JSON profiling. `sura_pkg_bench_smoke.ps1` verifies package-level text/JSON/Markdown benchmarking, no-JIT mode, optional Python comparison output, and the minimum-speedup gate. `sura_pkg_list_smoke.ps1`, `sura_pkg_info_smoke.ps1`, `sura_pkg_docs_smoke.ps1`, and `sura_pkg_search_smoke.ps1` verify package inventory, builtin stdlib metadata, generated docs/search artifacts, and registry/stdlib search output. `sura_pkg_tree_smoke.ps1` verifies resolved dependency tree text and JSON output for direct plus transitive packages. `sura_pkg_restore_lock_smoke.ps1` verifies JSON restore reports, direct/transitive package restoration, idempotent restore, JSON lock reports, lockfile hashes, and lockfile verification reports. `sura_pkg_remove_smoke.ps1` verifies JSON package removal reports, manifest dependency cleanup, package directory deletion, and no-op missing-package removal. `sura_clean_smoke.ps1` verifies safe generated-log and `sura_walk_*` cleanup, dry-run behavior, no-op cleanup, source preservation, and JSON reports. `sura_stdlib_modules_smoke.ps1` verifies namespaced `use` modules for array, path, math, string, OS environment helpers, regex, datetime, crypto, CLI, logging, JSON, filesystem, database, random, and testing helpers. `sura_test_runner_smoke.ps1` verifies schema-versioned JSON/JUnit test reports, failure reporting, no-JIT mode, and UTF-8 test output. `sura_engine_test_smoke.ps1` verifies engine-level `--test` and `--test-report` behavior. `sura_async_smoke.ps1` verifies namespaced `async` HTTP tasks, task status/pending inspection, timeout waits, `any`/`all`, cleanup, and forgotten completed tasks. `sura_random_smoke.ps1` verifies namespaced `random` seeding, deterministic int/float/bool/choice/shuffle behavior, random byte arrays, UUID access, and direct builtin aliases. `sura_doctor_smoke.ps1` verifies the environment doctor success and failure paths. `sura_policy_smoke.ps1` verifies that `surapkg policy` infers starter HTTP tool policies, writes JSON policy reports, refuses to overwrite existing policies, and requires manual shell command prefixes. `sura_quality_smoke.ps1` checks the package quality score gate for complete, broken, and security-risk package layouts. `sura_ci_smoke.ps1` verifies that `surapkg ci` generates searchable docs, runs tests, runs configured benchmark gates, audits, verifies existing package/tool-policy signatures, quality-gates good packages, and refuses packages with failing tests. `sura_release_smoke.ps1` verifies that `surapkg release` generates docs, runs configured benchmark gates, signs, runs tests, enforces quality, publishes good packages, and refuses packages with failing tests. `sura_release_pack_smoke.ps1` verifies hardened v5 `--release` protected package creation, source-string hiding, raw bytecode header sealing, release inspection locking, UTF-8 output, direct `.sura.srp` execution, key/license-file loading, and `--load-release`. `sura_pkg_protect_smoke.ps1` verifies `surapkg protect` source/secret leak reports, `next_actions`, closed-source enforcement, tamper rejection, protected launcher output, and UTF-8 runtime output. `sura_http_server_smoke.ps1` starts Sura static and route-based mock API servers, fetches files and JSON through the HTTP client helpers, verifies echo routes, and stops the servers. `sura_agent_smoke.ps1` verifies that `surapkg agent` generates a runnable, testable, auditable AI agent template using `json.schema_errors`, source-grounded `knowledge/` RAG documents, and policy-constrained `http_request`. `sura_profile_smoke.ps1` checks the human-readable profiler report and machine-readable profile JSON. `sura_python_bridge_smoke.ps1` verifies interpreter discovery, Python package/module calls, positional and keyword arguments, JSON return conversion, and UTF-8-safe output handling. `sura_lsp_smoke.js` verifies LSP incremental `didChange` edits along with global and namespaced stdlib module completions, reassignment-aware type hover, signature help, symbols, semantic tokens, references, rename, recovering parser diagnostics, and quick fixes including missing `end` insertion, legacy `http_get`/`http_json`/`http_post` conversions to policy-friendly request specs, legacy `async_http_get`/`async_http_request` conversions to namespaced `async.*` calls, and direct Python/FFI/plugin runtime-call conversions to namespaced `python.*`/`ffi.*`/`plugin.*` calls. `sura_debug_smoke.js`, `sura_debug_locals_smoke.js`, and `sura_debug_exception_smoke.js` verify the VS Code DAP bridge for conditional breakpoints, stepping, locals/scopes, watch evaluation, expandable values, and exception stops. `sura_jit_mod_smoke.ps1` verifies that modulo-heavy hot functions compile to native JIT code and keep correct results. `sura_check_smoke.ps1` verifies the engine check mode success, failure, and multi-error parser-recovery paths. `sura_engine_lint_smoke.ps1` verifies lint pass, risky API plus weak tool-policy and unredacted sensitive-header warnings, and structural failure paths. `sura_engine_format_smoke.ps1` verifies engine format check, rewrite, and post-format pass paths. `sura_registry_client_smoke.ps1` publishes a temporary package, checks registry search results, and verifies download/publish stats plus daily analytics output. `sura_registry_account_smoke.ps1` verifies admin token creation, one-time recovery codes, old-token revocation, and publishing with recovered tokens. `sura_registry_report_smoke.ps1` starts the HTTP registry API, submits CLI and API reports, verifies public analytics, verifies admin review, and checks UTF-8 Korean report text persistence. `sura_registry_verify_smoke.ps1` checks that registry verification passes local and HTTP registry package/signature/metadata state and fails tampered index hashes or missing signatures. `sura_public_signature_smoke.ps1` creates an RSA keypair, signs/verifies packages and tool policies, checks strict public-key verification, verifies local/remote trusted key discovery, and confirms tampered packages fail. `sura_update_smoke.ps1` publishes two versions plus a yanked higher version, checks `surapkg outdated`, updates a project dependency, verifies resolver/restore constraint handling, and confirms the installed package moved to the latest non-yanked version while the dependency range stayed intact. `sura_bind_c_smoke.ps1` builds a native C library, verifies `surapkg bind-c` wrapper generation, JSON bind reports, parses the generated Sura file, and runs numeric plus string FFI calls. `sura_embed_smoke.ps1` compiles a native C++ host against `sura_ffi.cpp`, injects globals, runs scripts, reads outputs, verifies persistent host state, and checks error reporting. `sura_aot.ps1` wraps `--compile` output with a launcher, and `sura_aot_smoke.ps1` verifies bytecode load plus launcher execution. `sura_to_js.ps1` lowers the portable automation subset to runnable JavaScript: `use` module lines, typed and untyped functions, `if`/`elif`/`else`, `while`, `repeat`, range and array `for`, assignments, compound assignments, arrays, dictionaries, string interpolation, assertions, common string/array helpers, portable `array`/`math`/`string`/`json`/`stream` module objects, and stream helpers. `sura_js_target_smoke.ps1` transpiles `test_js_target.sura` and runs the generated JavaScript with Node. `sura_to_wasm.ps1` provides a numeric WAT output path with lowered assertion checks for CI/proof-of-pipeline while the full runtime still runs through the VM/JIT. `sura_wasm_target_smoke.ps1` verifies the generated WAT and optionally emits `.wasm` when `wat2wasm` is installed. `bindgen_c.ps1` and `surapkg bind-c` emit Sura FFI stubs from simple C/C++ C-ABI headers.

`sura_ci_coverage_gate.ps1` verifies that goal-critical CI coverage remains present across the Windows benchmark and Ubuntu/macOS cross-platform workflows, writes schema `sura.ci.coverage_gate.v1`, and fails when required smoke scripts, workflow references, runner coverage, or path filters disappear. Its smoke test also checks the failure path against a deliberately incomplete fake repo. `sura_target_lowering_audit.ps1` writes schema `sura.target.lowering_audit.v1` plus Markdown evidence that maps every `ast.hpp` `NK` node to JS and WASM lowering status, records whether the targets still use source-line frontends instead of AST/bytecode input, and keeps the full-lowering blocker visible in release evidence without treating subset smoke tests as proof of completion. The WASM smoke evidence now includes direct/alias promoted function `to_str(function)`, visible/ternary array/dict literal `to_str(value)`, exact numeric-index variable mixed primitive array element `type(value)`/`length(value)`/`to_bool(value)`/`to_str(value)`, exact string-key variable mixed primitive dict element `type(value)`/`length(value)`/`to_bool(value)`/`to_str(value)`, branch-sensitive mixed ternary `type(value)`/`length(value)`/`to_str(value)`, JSON escaped string code point lowering, and promoted function values flowing through constructor-derived class fields, method returns, context-param-derived nested function call string returns, function-local and if/match-merged `new` object method-call string returns, same-class if-returned and wildcard-covered match-returned object method receiver hints, direct and `super.init(...)`-inherited constructor object arguments feeding class-field receiver hints, dynamic string-key dict object assignments feeding method/field/index receiver hints, same-variable dynamic string-key dict assignments feeding exact num/string/bool/nil/array/dict/function/object hints in mixed dictionaries, same-variable dynamic numeric array-index assignments feeding num/string/bool/nil value type hints plus object method/field/index receiver hints, array/dict-stored object receiver hints including homogeneous dynamic index/key receivers and foreach loop values, inherited `super.method(...)` calls, current local/param arg-derived `super.method(...)` string returns, equality/truthiness/type checks, function-valued `if`/`while`/logical conditions, array/dictionary `for` loops, dynamic string-key dictionary updates, object field reassignment, method-thrown string/function payload catches, and method-local `super.method(...)` throw payload catches through the exception side channel.

Latest WASM receiver-flow evidence also covers homogeneous object array/dict `for` returns feeding function-call receiver hints, object-typed function/method arguments feeding method, field, and string-key index return hints, constructor object arguments feeding class-field receiver hints through direct construction plus inherited `super.init(...)` chaining, dynamic string-key dict object assignments preserving receiver hints, same-variable dynamic string-key dict assignments preserving exact num/string/bool/nil/array/dict/function/object hints in mixed dictionaries, and same-variable dynamic numeric array-index assignments preserving receiver plus num/string/bool/nil type hints, so methods called on those returned/stored objects and returned strings/numbers from object-parameter functions/methods lower to generated class method dispatch or dict-backed field lookup.

Latest WASM arithmetic, bitwise, boolean, equality, and comparison evidence also covers `+`/`-`/`*`/`/`/`%`/`&`/`|`/`^`/`<<`/`>>`/`~`/`<`/`<=`/`>`/`>=`/`==`/`!=`/`and`/`or`/`not` over exact numeric-index array and exact string-key dictionary dynamic-key value access. Direct i32 operations handle the non-trapping integer subset; guarded helpers handle modulo and shifts; `/` always uses tagged f64 real-number division; tagged Value helpers handle dynamic type checks, equality, and truthiness. Numeric type failures, zero divisors, and invalid shift counts enter the same catchable exception side channel used by lowered calls instead of escaping as raw WebAssembly traps. The 16-byte tagged Value ABI has a tag-10 f64 representation for fractional literals, f64 `+`/`-`/`*`, all division results, mixed-type function parameters, and returns; numeric equality, `type`, truthiness, stringification, interpolation, and collection storage accept both i32 and f64 number tags. Guarded helpers also cover runtime-selected f64 `abs`, `sqrt`, `floor`, `ceil`, `round`, `sign`, `sin`, `cos`, `tan`, variadic `min`/`max`, `clamp`, and integer or fractional `pow`. Integer exponents use exponentiation by squaring; positive-base fractional exponents use pure-WASM log/exp helpers. `tests/wasm_static_f64_arithmetic.sura`, `tests/wasm_dynamic_decimal_value.sura`, and `tests/wasm_real_division.sura` prove these paths through the VM, JIT entry path, and emitted WebAssembly. A native raw-f64 export ABI, additional public math intrinsics, and exact native-libm parity for very large trigonometric arguments remain incomplete.

`sura_release_evidence_gate.ps1` verifies the final uploaded benchmark/release evidence bundle, writes schema `sura.release.evidence_gate.v1`, records SHA256 hashes for the required HTML/JSON/Markdown/history/coverage/goal-audit artifacts, checks required benchmark and Python-comparison coverage, verifies that the dashboard surfaces native C++ baseline evidence when `native_perf.json` is present, and fails when release notes, dashboard JSON, history, native-performance display, or CI coverage evidence is missing.

`sura_native_perf_baseline.ps1` compiles C++ `bench_physics` and `bench_physics3d` references, runs the matching Sura JIT benchmarks multiple times, reads Sura's script-level physics-loop timing before falling back to whole-script execute timing, and writes schema `sura.native.performance.v1` plus Markdown ratio evidence, including per-run Sura timings and timing-source metadata. Native-performance gaps are visible in CI instead of being implied by Python comparisons alone, and the 2D Vec2 and 3D Vec3/cross-product Sura/native ratios are reported separately.

The WASM target exports top-level numeric functions such as `square`, `sum_to`, `fib`, numeric-array helpers such as `sum3`, and numeric-array-returning helpers such as `make_values` from generated WAT, so host runtimes can call Sura numeric functions directly instead of only invoking the generated `main` wrapper. Numeric array literals lower to linear memory, local array values hold element pointers, `arr[index]` lowers to `i32.load`, `arr[index] is value` lowers to `i32.store`, `arr.len()` and `array.len(arr)` read the stored length header, `array.sum(arr)`/`array_sum(arr)` lower to a WASM reduction helper, non-empty integer `array.avg(arr)`/`array.average(arr)`/`array_avg(arr)` lower to integer division of sum by length, non-empty integer `array.min(arr)`/`array_min(arr)` and `array.max(arr)`/`array_max(arr)` lower to WASM reduction helpers, `array.range(end)`/`array.range(start, end, step)`/`array_range(...)` lower to a WASM range-array helper, `array.index_of(arr, value)`/`array.index(arr, value)` return the numeric index or `-1`, `array.contains(arr, value)` lowers to a boolean numeric scan, inline numeric array literals such as `sum3([8, 9, 10])` are valid call arguments, function-call result indexing such as `make_values()[0]` stays as separate WAT instructions, indexed numeric array loops such as `for idx, item in values do` lower both the index and element locals, and object values stored in array indices, dictionary fields, dot assignments, string-key indexes, homogeneous dynamic array indexes, homogeneous dynamic dictionary keys, or homogeneous array/dictionary `for` loop values retain receiver hints for direct method dispatch.

`sura_goal_audit.ps1` maps the active world-class Sura goal to concrete repo evidence, writes schema `sura.goal.audit.v1`, emits a progress percentage, and records remaining work. It also treats the roadmap's `Non-Negotiable Next Steps` as explicit `world_class_frontier` blockers, so the audit can no longer report 100% while full JS/WASM lowering or any other explicitly listed frontier work remains open. It is intentionally allowed to report `INCOMPLETE` without failing CI unless `-FailOnIncomplete` is used, so release evidence and Discord progress updates can show native-speed and frontier blockers honestly while the work continues. `sura_discord_goal_status.ps1` reads `goal_audit.json` plus optional `native_perf.json` and posts the same overall progress, blocker count, native ratio, and "전체 골까지 남은 작업" list to Discord through `SURA_DISCORD_WEBHOOK`, encoded as UTF-8 JSON to keep Korean text readable.

`sura_lsp_smoke.js` also covers `headers_redact(...)` sensitive-header quick fixes, `tool_call_policy(...)`/`tool.call_policy(...)` direct tool-call quick fixes, the `http_get`/`http_json` to policy-friendly request-spec quick fixes, and `async_http_get`/`async_http_request` namespaced quick fixes.

`sura_pkg_version_smoke.ps1` verifies `surapkg version` queries, patch/minor/major bumps, explicit version setting, missing-version insertion, invalid-version rejection, and JSON reports.

The JavaScript target also includes stable-test runtime parity for 13 selected existing Sura scripts (`tests/01_basic.sura`, `tests/02_variables.sura`, `tests/03_control.sura`, `tests/04_functions.sura`, `tests/05_arrays.sura`, `tests/06_math.sura`, `tests/08_exceptions.sura`, `tests/09_string_interpolation.sura`, `test_stdlib.sura`, `test_methods_chain.sura`, `test_null_optional.sura`, `test_when_match.sura`, and `test_for_in_improved.sura`), direct `wait`/`sleep_ms` runtime shims, direct stdlib aliases such as `clamp`, `startsWith`, `substring`, and `concat`, lambda expressions lowered to arrow functions, inline `func(...) do return ... end` callbacks lowered to JavaScript function expressions, block function expressions lowered to JavaScript function expressions, nested function closures, indexed `for index, value in items` loops, indexed `for key, value in dict` loops, nil/dict-safe `for ... in` helpers, repeat and range loop limits snapshotted at loop entry, tilde range `for n in 1 ~ 3` loops, tilde range `when ... in 1 ~ 3 then` arms, first-else `when` arms, nonterminal default `else`/`_` arms where later exact arms can still override, block-style `match` arms without `then`, first-wildcard `match` arms, `break`/`continue` loop control, space-form assertions such as `assert value > 0` and `assert_eq actual, expected`, ternary expressions, optional chaining/null coalescing expressions including undeclared optional roots, division-by-zero exception parity, Sura-style method aliases such as `index_of`, `starts_with`, `ends_with`, `sub`, `to_num`, `keys`, `values`, and `delete`, unary bitwise-not expressions, PATH-based `which`/`cmd_exists` command discovery, portable `cmd_quote`/`cmd_join` command builders, class/`extends` lowering, `super.init(...)` to `super(...)` constructor lowering, `self` to `this` method lowering, `when`/`is`/`in`/`else` branch lowering, `try`/`catch`/`finally` plus `throw` lowering, and a portable `os` module object for `os.wait`, `os.sleep_ms`, `os.cwd`, `os.argv`, `os.argc`, `os.script_name`, `os.home_dir`, `os.temp_dir`, `os.path_separator`, `os.name`, `os.is_windows`, `os.which`, `os.cmd_exists`, `os.cmd_quote`, `os.cmd_join`, and explicit `os.run`/`os.run_checked` shell-execution stubs that fail on the JS target. `tools/sura_js_target_smoke.ps1` now verifies those class, `when`, exception, method-chain, and optional-root paths by executing Sura-generated JavaScript through Node after transpilation.

The JavaScript target expands `import "file.sura"` recursively before source lowering, resolving relative paths from the importing file and skipping already-loaded modules to match the native compiler's module-cache behavior. `test_js_target.sura` imports `tests/js_import_fixture.sura`, and `tools/sura_js_target_smoke.ps1` verifies that imported functions and top-level values are present in the generated JavaScript.

The JavaScript target keeps direct `random(...)` calls while also exposing reproducible `random.seed`, `random.int`, `random.float`, `random.bool`, `random.choice`, `random.shuffle`, `random.bytes`, and `random.uuid` helpers for portable automation and game scripts.

The JavaScript target includes Node-backed `file_read`, `file_write`, `file_read_json`, `file_write_json`, `file_read_bytes`, `file_write_bytes`, `file_sha256`, `file_append`, `file_exists`, `file_delete`, `file_remove_tree`, `file_lines`, `file_list`, `file_walk`, `file_glob`, `mkdir`, file metadata, copy, move, and legacy direct aliases such as `read_file`/`write_file`/`append_file`/`delete_file`/`exists`, plus portable `fs.read`, `fs.write`, `fs.read_json`, `fs.write_json`, `fs.read_bytes`, `fs.write_bytes`, `fs.sha256`, `fs.append`, `fs.exists`, `fs.delete`, `fs.remove`, `fs.remove_tree`, `fs.list`, `fs.walk`, `fs.glob`, `fs.info`, `fs.size`, `fs.copy`, `fs.move`, `fs.lines`, and `fs` path aliases for small automation scripts.

The JavaScript target also includes Node-backed `path_join`, `path_basename`, `path_dirname`, `path_ext`, `path_stem`, `path_normalize`, `path_abs`, `path_relative`, and portable `path.*` module helpers.

The JavaScript target includes `argv`, `argc`, `script_name`, `cli_parse`, and portable `cli.parse` helpers for shell-like flags, quoted values, repeated flags, `--no-*`, short options, and value-taking flag lists.

The JavaScript target also includes `env_get`, `env_require`, `env_set`, `env_load`, and portable `os.env_*` helpers backed by `process.env`.

The JavaScript target includes Node-backed `sha256`, `file_sha256`, `hmac_sha256`, `file_hmac_sha256`, `hmac_sha256_file`, `crypto_random_bytes`, `crypto_random_hex`, `constant_time_eq`, `hex_encode`, `hex_decode`, `base64_encode`, `base64_decode`, unpadded `base64_url_encode`, `base64_url_decode`, `url_encode`, `url_decode`, and portable `crypto.file_sha256`/`crypto.file_hmac_sha256`/`crypto.random_hex`/`crypto.constant_time_eq`/`crypto.*` helpers for API clients and automation scripts.

The JavaScript target includes `regex_match`, `regex_replace`, `regex_find_all`, `regex_escape`, `regex_capture`, `regex_captures`, `regex_split`, portable `regex.*` helpers, `datetime_parse`, `datetime_format`, `datetime_utc_format`, `datetime_add`, `datetime_diff`, `datetime_parts`, `datetime_now`, `timestamp`, and portable `datetime.*` helpers for logs, schedulers, and API payloads.

The JavaScript target includes `log_set_file`, `log_set_json`, `log_event`, `log_debug`, `log_info`, `log_warn`, `log_error`, portable `log.*` helpers, throwing assertions including `assert_ne`/`assert_not_contains`/`assert_type`/`assert_len`/`assert_between`/`assert_approx`, non-throwing `check`/`check_eq`/`check_match`, `test_summary`, `test_report`, and portable `test.*` helpers for script smoke checks.

The JavaScript target includes file-backed `db_set`, `db_get`, `db_has`, `db_delete`, `db_keys`, `db_all`, `db_insert`, `db_find`, `db_count`, `db_update`, `db_remove`, `db_query` with sort/offset/limit options, portable `db.*` helpers, `file://` plus Node/curl-backed `http://` and `https://` support for `http_get`, `http_request`, `http_request_full`, JSON checked/retry request helpers, form request bodies via `form` specs, Node-backed `http_serve_static`, `http_serve_routes`, `http_server_url`, `http_server_stop`, policy-aware HTTP tool calls, and pure HTTP helper shims for `url_parse`, `url_build`, `query_build`, `query_parse`, `form_build`, `form_parse`, `auth_bearer`, `auth_basic`, `headers_merge`, `headers_get`, `headers_has`, `headers_redact`, `http_content_type`, `http_charset`, `http_is_json`, `http_status_*`, `http_retry_after`, `http_backoff_delays`, and `http.*`.

The JavaScript target includes `file://` plus Node/curl-backed `http://` and `https://` support for `async_http_get` and `async_http_request`, timer tasks through `async_sleep`, status/pending inspection, timeout-aware await helpers, any/all task joins, cleanup/forget helpers, and portable `async.*` task helpers for local automation smoke tests.

The native `async` runtime uses a bounded worker pool instead of creating one thread per task. The default worker count is capped at eight and the default pending queue at 1024; `async.limits()` reports the active bounds and `async.configure(max_workers, max_queue)` can replace them whenever no tasks or scopes are live. Tasks expose explicit `queued`, `running`, `succeeded`, `failed`, and `cancelled` states through `async.status(task_id)`. `async.cancel(task_id)` interrupts timers and terminates async command/curl child processes cooperatively. Structured lifetimes use `scope is async.scope()`, an optional scope argument on `async.cmd`, `async.http_get`, `async.http_request`, and `async.sleep`, plus `async.scope_attach`, `async.scope_cancel`, `async.scope_status`, `async.scope_join`, and `async.scope_close`. Join waits for existing children; close requests cancellation first. Both remove child handles and propagate retained child failures after cleanup. A timed join/close returns `closed: false` and leaves a closing scope that rejects new children until a later join/close completes it.

The JavaScript target also defines native interop module objects for `python`, `ffi`, and `plugin` so cross-target scripts fail predictably: `python.available()` returns `false`, `python.executable()` returns an empty string, and native-only calls such as `python.call_json`, `ffi.call`, and `plugin.call` throw a clear message that they require the Sura native runtime.

The JavaScript target includes AI-native pure helpers for normalized `string.lines`, whitespace `string.words`, `string.pad_left`/`string.pad_right`, overlap-aware `text_chunks`/`text_chunk` and `string.chunks`, `vector_add`, `vector_dot`, `vector_scale`, `vector_norm`, `vector_cosine`, `vector_normalize`, dedicated 3D vector helpers (`vec3`, `vec3_add`, `vec3_sub`, `vec3_dot`, `vec3_cross`, `vec3_scale`, `vec3_norm`, `vec3_normalize`, `vec3_distance`, `vec3_neg`, `vec3_lerp`, `vec3_midpoint`, `vec3_project`, `vec3_reject`, `vec3_reflect`, `vec3_angle`, `vec3_transform4`), 3D scene-data helpers (`mat4_identity`, `mat4_translate`, `mat4_scale`, `mat4_rotate_y`, `mat4_mul`, `mesh_cube`, `mesh_transform4`, `mesh_bounds`, `mesh_face_normals`, `camera_project`), `vector_search`, `rag_context`, `rag_sources`, `rag_prepare`, `rag_messages`, `tensor_shape`, `tensor_zeros`, `tensor_fill`, `tensor_add`, `tensor_mul`, `tensor_clip`, `tensor_flatten`, `tensor_sum`, `tensor_mean`, `tensor_variance`, `tensor_std`, `tensor_min`, `tensor_max`, `tensor_argmin`, `tensor_argmax`, `tensor_zscore`, `tensor_softmax`, `tensor_transpose`, `tensor_matmul`, and portable `vector.*`, `graphics3d.*`, `rag.*`, and `tensor.*` modules.

The JavaScript target includes JSON-schema conversion/validation helpers, pretty JSON formatting, JSON path lookup/existence/set/delete/merge-patch helpers, JSON-path collection helpers, `template_render`, portable `json.*` including `json.pretty`, `json.sse_parse`/`json.sse_data`, and `json.ini_parse`/`json.ini_stringify`, tool spec/schema/policy helpers via `tool.*`, OpenAI-style request/message/tool-call builders via `llm.*`, `llm.next_messages` tool-turn assembly, `llm.next_request`/`llm.next_schema_request` next-turn request builders, `llm.usage` token accounting, `llm.cost` cost estimation, `llm.budget` budget checks, and `llm.chat` file-backed mock responses so agent scripts can build and smoke-test validated LLM flows without leaving the JS target.

The JavaScript target also includes direct and portable JSON/data helpers for `json_path`/`dict_get_path`/`json_has_path`/`json_merge_patch`/`json_delete_path`/`json_set_path`, `array.min`, `array.max`, `array.range`, `array.chunk`, `array.zip`, `array.repeat`, `dict.keys`, `dict.values`, `dict.items`, `dict.merge`, `dict.pick`, `dict.omit`, `set.union`, `set.intersection`, `set.difference`, `set.symmetric_difference`, `set.is_subset`, `set.is_superset`, `pluck`, `count_by`, `group_by`, `sort_by`, JSON Lines, and CSV parsing/stringifying, including `json.get_path`, `json.has_path`, `json.merge_patch`, `json.delete_path`, `json.set_path`, `json.pluck`, `json.count_by`, `json.group_by`, `json.sort_by`, `json.jsonl_*`, and `json.csv_*`.

The native runtime executes forward and reverse stepped range `for` loops, including negative steps used by standard-library helpers. The JavaScript target lowers block and inline control-flow parity for `if`/`elif`/`else`, `match` exact-pattern arms, block-style `match` arms without `then`, first-wildcard `match` arms, `match` wildcard arms, nonterminal default `else`/`_` arms where later exact arms can still override, indexed `for key, value in dict` loops, nil/dict-safe `for ... in`, stepped range `for`, tilde range `for n in 1 ~ 3`, tilde range `when ... in 1 ~ 3 then` arms, first-else `when` arms, JS target snapshots for repeat and range loop limits, `break`/`continue` loop control, enum declarations, struct declarations with factory calls, `print_n value`/`print_no_nl value` space-form output, lambda expressions, block function expressions, nested function closures, ternary expressions, optional chaining/null coalescing expressions, unary bitwise-not expressions, and inline `elif ... then` plus inline `else then` statements, so portable scripts can share the same branch shape across the native, JS, and WASM smoke paths. The WASM target now exports normal `.sura` input to `sura.ast.v1` before lowering, also accepts prebuilt AST JSON with `-AstJson`, expands AST `import "file.sura"` recursively, resolves relative paths from the importing file, skips already-loaded modules, lowers `nil` as an i32 zero sentinel in the numeric subset, lowers string literals as length-prefixed i32 code arrays for array-like `len()` coverage including JSON escaped string code points, lowers inline branch-local declarations, fixed-count `repeat`, block-style numeric `match` arms without `then`, exact string/bool/nil/value-candidate `match` arms through tagged Value equality, string/bool/nil/value-candidate `while` conditions through tagged Value truthiness, static/hinted and branch-sensitive mixed ternary `type(value)`/`length(value)`/`to_str(value)`, primitive `to_str(value)`, direct/alias promoted function `to_str(function)`, visible/ternary array/dict literal `to_str(value)`, homogeneous array variable `to_str(value)`, and value-aware `to_bool(value)`, first-wildcard numeric `match` arms, first-else numeric `when` arms, nonterminal default `else`/`_` arms where later exact arms can still override, tilde range `for n in 1 ~ 3`, tilde range `when ... in 30 ~ 35 then` arms, numeric enum declarations with explicit numeric member values, numeric array literals/indexing/`len()` through linear memory, `array.len`/`array.sum`/`array.avg`/`array.min`/`array.max`/`array.range`/`array.index_of`/`array.contains` numeric helpers, inline numeric array literals as call arguments, numeric array returns plus function-call result indexing, array/dict-stored object receiver method hints including homogeneous dynamic index/key receivers and foreach loop values, numeric array `for ... in` and indexed `for index, value in ...` loops, array pointer arguments for numeric functions, integer 3D vector helpers (`vec3`, `vec3_add`, `vec3_sub`, `vec3_dot`, `vec3_cross`, `vec3_scale`, `vec3_norm`, `vec3_distance`, and `vector.*3` aliases), numeric ternary expressions, bitwise expressions, shift expressions, unary bitwise-not expressions, AST expression-call assertions/prints, module method intrinsics such as `array.len(...)`/`math.floor(...)`/`vector.dot3(...)`, decimal literal folding, and nil-returning method calls with side-effect-preserving lowering into constants, memory operations, and integer instructions for the portable WASM subset. The target-lowering audit keeps a separate AST-node matrix for these targets so remaining dynamic Value/function/class/exception partial WASM coverage remains visible until full dynamic lowering is done.

The JavaScript target also includes `sse_parse`, `sse_data`, portable `json.sse_parse`/`json.sse_data`, `llm_stream_text`, and portable `llm.stream_text` helpers for OpenAI-style Server-Sent Events streaming text.

The WebAssembly target accepts `is` assignment/reassignment and portable `use math`/`use vector` lines, then lowers recursive numeric functions, inline `if ... then return`, inline `elif ... then` and inline `else then` statements, inline branch-local declarations, numeric ternary expressions, numeric `match` exact-pattern arms and wildcard arms, block-style numeric `match` arms without `then`, first-wildcard numeric `match` arms, first-else numeric `when` arms, nonterminal default `else`/`_` arms where later exact arms can still override, stepped range `for`, tilde range `for n in 1 ~ 3`, tilde range `when ... in 30 ~ 35 then` arms, `throw` trap lowering, integer-safe raw-i32 math, and tagged f64 math directly to WAT. Runtime-selected f64 Values are supported by `abs`/`math.abs`, `sqrt`/`math.sqrt`, `floor`/`math.floor`, `ceil`/`math.ceil`, `round`/`math.round`, `sign`/`math.sign`, `sin`/`math.sin`, `cos`/`math.cos`, `tan`/`math.tan`, variadic `min`/`math.min`, variadic `max`/`math.max`, `clamp`/`math.clamp`, and integer or fractional `pow`/`math.pow`; numeric type failures use the catchable exception side channel. Direct `to_int`, `to_float`, `to_str`, and `to_bool` calls preserve the supported primitive/Value semantics. Decimal literal calls such as `floor(3.9)`, `ceil(3.1)`, `round(3.5)`, `math.floor(2.9)`, and signed decimal literal calls such as `round(-3.5)` remain folded to integer WAT constants; `round` uses native-compatible round-half-away-from-zero behavior. The trigonometric and fractional-power paths are pure WASM and do not import JavaScript math functions; exact native-libm parity is not claimed for very large trigonometric arguments. The target also lowers integer 3D vector construction, add/sub/dot/cross/scale/norm/distance, and `vector.vec3`/`vector.dot3`/`vector.cross`/`vector.distance3` aliases through the same linear-memory numeric arrays used for portable array code. It folds decimal literal comparisons such as `math.pi > 3.14`, `math.pi < 3.15`, and `3.14 < math.pi` to boolean integer constants.

The VS Code extension contributes a `sura` Debug Adapter Protocol launcher. Add this to `.vscode/launch.json` to debug the active file:

```json
{
  "type": "sura",
  "request": "launch",
  "name": "Debug Sura File",
  "program": "${file}",
  "enginePath": "${config:sura.enginePath}",
  "cwd": "${workspaceFolder}",
  "stopOnEntry": false
}
```

The adapter launches `SuraLanguage.exe --debug-protocol` and supports VM-backed line breakpoints, conditional breakpoints, hit conditions, runtime exception breakpoints, stop-on-entry, `continue`, `next`/step requests, call-stack snapshots, stack frames, per-frame global/local scopes, named function/method local snapshots, expandable array/dict/instance variables, watch/evaluate requests for variables, field and index access, comparisons, arithmetic, grouped expressions, safe read-only helper calls (`len`, `type`, `str`, `num`, `bool`, `keys`, `contains`), clear watch-expression diagnostics, and expandable array/dict/instance watch results, output streaming, and process termination. Breakpoints are validated against executable-looking source lines before launch.

Validate the JS target with:

```powershell
.\tools\sura_js_target_smoke.ps1
```

Validate the numeric WASM/WAT target with:

The WASM target also keeps side-effect-preserving `throw` expression evaluation before trap lowering: `throw expr` in main/top-level code lowers `expr`, drops its value, and then emits `unreachable`. Inside generated functions, uncaught `throw` now writes the payload to a WASM exception side-channel and returns a sentinel value, allowing direct function, method, and supported `super.method(...)` calls inside an enclosing AST `try` assignment or expression statement to bridge that payload into the local catch block. The smoke checks `throw_value_source(41)` in generated WAT so the trap path cannot skip expression calls, and also checks string, numeric, promoted function-valued, method-thrown, and super-method-thrown payloads caught by `try/catch`. Registered dict-backed instances now cross function boundaries as guarded tag-6 Values; a catch whose throw flow resolves to multiple known classes keeps their union and calls the matching method through the runtime class id. Fully general instance exceptions beyond statically discoverable registered classes remain a target-lowering blocker.

The WASM AST path lowers enum member access as constants across numeric, string, and bare enum forms. Bare members default to their own name string, so `Mode.READY` can lower as `"READY"` while `Mode.SCORE is 11` still lowers as an `i32.const`. It also represents promoted top-level function names and aliases as tag-7 function Values, so `type(function_name)`, `to_bool(function_name)`, string interpolation of those results, homogeneous array/dict function lookups, observed function-valued parameters and returns, context-param-derived nested function returns, function and method throw payload catches, and function Value equality are smoke-covered without claiming closure or fully dynamic function-call lowering.

The WASM target lowers common string helpers in direct, namespaced, and receiver forms: `contains`, `indexOf`/`index_of`, `startsWith`/`starts_with`, `endsWith`/`ends_with`, `upper`, `lower`, `trim`, `substring`, `slice`, and `sub` compile to WAT loops over Sura's length-prefixed string representation.

```powershell
.\tools\sura_wasm_target_smoke.ps1
```

The generated module exports `main` as an `i32` result. The target supports numeric functions and locals with or without Sura type annotations, promoted top-level function values in the tagged Value ABI, homogeneous function array/dict hints, function-valued parameter and return hints, `is` assignment/reassignment, `true`/`false` bool literals, `return`, `throw` trap lowering, `print`, `print(...)`, `print_n(...)`, `print_no_nl(...)`, and `print_no_nl value` as the main result, `assert`/`assert_eq`/`assert_ne`/`assert_neq`/`assert_between`/`assert_approx` lowered to WAT traps, space-form numeric assertions such as `assert result == 32` and `assert_eq result, 32`, `if`/`elif`/`else`, inline branch-local declarations, fixed-count `repeat`, numeric ternary expressions, numeric `when`/`is`/`in`/`else` arms including tilde ranges, first-else arms, and nonterminal default `else`/`_` arms where later exact arms can still override, numeric `match` arms with or without `then`, first-wildcard numeric `match` arms, `while`, `repeat`, range `for`, named-label `break`/`continue` lowering inside nested branches, function calls, arithmetic, integer comparisons, numeric `and`/`or`/`not` conditions plus function-valued `if`/`while`/logical truthiness through the tagged Value ABI, bitwise/shift and unary bitwise-not expressions, integer-safe `math.*` helpers, static/hinted and branch-sensitive mixed ternary `type(value)`/`length(value)`/`to_str(value)`, primitive `to_str(value)`, direct/alias promoted function `to_str(function)`, visible/ternary array/dict literal `to_str(value)`, homogeneous array variable `to_str(value)`, value-aware `to_bool(value)`, JSON escaped string code point lowering, function Value equality for promoted functions, and direct numeric aliases such as `to_int`, `to_float`, `to_bool`, `abs`, `sqrt`, `min`, `max`, `clamp`, and `sign`. If `wat2wasm` is installed, `sura_wasm_target_smoke.ps1` also emits a binary module.

## C ABI And Plugins

Sura exposes two C-facing surfaces:

- `sura_ffi.hpp` embeds Sura inside a host app and exposes `sura_abi_version()`.
- `sura_plugin.h` is the versioned native plugin ABI for exposing C functions back to Sura.

Minimal host embedding:

```cpp
#include "sura_ffi.hpp"
#include <iostream>

int main() {
    SuraHandle h = sura_new();
    sura_set_number(h, "base_price", 100);
    sura_set_number(h, "demand", 0.8);
    int rc = sura_run(h, "price is base_price * (1 + demand * 0.5)\n");
    if (rc != SURA_OK) {
        std::cerr << sura_last_error(h) << "\n";
        return 1;
    }
    std::cout << sura_get_number(h, "price") << "\n";
    sura_free(h);
}
```

Verify the native embedding path:

```powershell
.\tools\sura_embed_smoke.ps1
.\tools\sura_ffi_safety_smoke.ps1
.\tools\sura_embed_template_smoke.ps1 -Surapkg .\surapkg.exe
```

Embedding ABI 1.2 rejects type errors with `SURA_ERR_TYPE` before executing
source by default. A host can temporarily call
`sura_set_legacy_types(handle, 1)` while migrating older scripts. The public
header is valid C as well as C++; exported calls do not let native exceptions
cross the C boundary.

Handles are monotonic opaque tokens backed by per-call leases. A stale,
forged, or repeatedly freed token is rejected. `sura_free(handle)` marks the
handle closed and defers destruction while a call is active, so overlapping
free/call and callback re-entry do not dereference released context memory. If
another host thread re-enters the same handle while `sura_run` owns it,
`sura_run` returns `SURA_ERR_BUSY`; getters return their documented default and
`sura_last_error` reports the busy condition on that thread.

Independent handles may be called from different host threads, but complete
VM/GC operations are serialized because the managed heap is process-global.
Each handle retains its own error state and its cross-run-safe globals. Numbers,
booleans, nil, strings, tensors, and arrays/dictionaries containing only those
values persist. Functions, captured upvalues, and class instances use metadata
owned by one execution image, so successful runs omit top-level globals whose
reachable graph contains those values. If a failed run inserts a run-bound
value into an existing persistent container, the handle clears its persistent
globals before returning the error. A globals update builds the candidate map
and every persistent GC root before swapping them into the context, so
allocation failure leaves the previous values and roots paired unless the
failed run contaminated an existing container as described above.
String and error getters return a host-thread-specific OS TLS/FLS buffer that is
valid until the next string/error getter on the same host thread.

Generate a ready-to-build native host scaffold with `surapkg embed`; add `--json embed-report.json` to capture the generated manifest, host, script, PowerShell build/run files, and `CMakeLists.txt` for CI artifacts. The generated PowerShell build script works on Windows, Linux, and macOS with PowerShell 7, auto-detects `c++`/`g++`/`clang++`, uses `.exe` only on Windows, and links `-ldl` on Linux when needed. The CMake scaffold is useful for IDEs and CI systems that already standardize on CMake:

```powershell
.\surapkg.exe embed game_host --json embed-report.json
cd game_host
.\build.ps1 -SuraRoot ..
.\run.ps1
cmake -S . -B build-cmake -DSURA_ROOT=..
cmake --build build-cmake --config Release
```

Simple dynamic-library calls are available from Sura:

```sura
use ffi
lib is ffi.load("native.dll")
print ffi.call(lib, "add", "int(int,int)", 2, 3)
print ffi.call(lib, "mul", "double(double,double)", 2.0, 4.0)
```

Generate wrappers for simple C/C++ C-ABI headers with `surapkg bind-c`:

```powershell
.\surapkg.exe bind-c .\native.h --out .\native.ffi.sura --lib .\native.dll --prefix native_ --json bind-report.json
.\tools\bindgen_c.ps1 -Header .\native.h -Out .\native.ffi.sura -Lib .\native.dll -Prefix native_ -Json bind-report.json
```

The generator supports simple C ABI prototypes with 0-4 integer or `char*`/`const char*` arguments, plus numeric, `void`, or `char*` returns. It also accepts common C++ header surface syntax for C ABI declarations, including `extern "C"` blocks, API macros, `noexcept`, default arguments, `std::size_t`, fixed-width integer aliases, `char const*`, and simple `typedef`/`using` aliases that resolve to supported primitive, string, or floating-point ABI types; C++ class/struct method bodies are skipped. Simple numeric `#define` constants such as decimal, hex, parenthesized, and signed values are emitted as prefixed Sura values, and simple `#define` string constants are emitted when they use supported escapes such as `\n`, `\t`, `\"`, and `\\`; function-like macros are skipped. Simple `enum` constants are emitted too, including implicit C-style increments after supported integer values. Double/float arguments are supported when all arguments are floating-point and the return is floating-point or `void`. Unsupported pointer shapes and unsupported mixed string/floating-point ABI shapes are left as skip comments so bindings do not silently call `ffi_call` with the wrong ABI. The standalone `tools/bindgen_c.ps1` supports the same `-Lib`, `-Prefix`, and `-Json` report workflow for environments where you want a script-level generator.

Versioned Sura plugins can be loaded through the plugin ABI and called by export name:

```sura
use plugin
plug is plugin.load("sura_sample_plugin.dll")
info is plugin.info(plug)
print info.name
print plugin.call(plug, "native_add", 2, 3)
plugin.unload(plug)
```

For packaged plugins, prefer manifest loading so Sura verifies the library name, version, SHA256 hash, and export allow-list before any native call:

```json
{
  "path": "sura_sample_plugin.dll",
  "name": "sura_sample_plugin",
  "version": "0.1.0",
  "sha256": "<64-char sha256>",
  "exports": ["native_add"],
  "host_capabilities": ["memory", "cancel"],
  "max_memory_bytes": 64,
  "max_call_ms": 100
}
```

Packages that ship native plugins also declare a package-level `sura.plugins.json` policy. This keeps all plugin manifests discoverable by audit and blocks accidental raw `plugin_load`/`plugin.load` use:

```json
{
  "version": 1,
  "sandbox": "manifest-locked",
  "manifests": ["native/sura_sample_plugin.sura-plugin.json"],
  "allowed_exports": ["native_add"],
  "host_capabilities": ["memory", "cancel"],
  "max_memory_bytes": 64,
  "max_call_ms": 100
}
```

```sura
use plugin
plug is plugin.load_manifest("native/sura_sample_plugin.sura-plugin.json")
print plugin.call(plug, "native_add", 2, 3)
```

Set `SURA_PLUGIN_POLICY=manifest-locked` when running packaged plugin code to enforce that same allow-list at runtime. In that mode direct `plugin.load(...)` calls are blocked, and `plugin.load_manifest(...)` only accepts manifest paths listed in the current package root's `sura.plugins.json`. When the package policy includes `allowed_exports`, Sura narrows the plugin manifest's own export allow-list further, so a package can expose only the native entry points it actually needs. `host_capabilities` can also narrow host callbacks: `memory` enables host allocation/free callbacks, `log` enables host logging callbacks, and `cancel` enables cooperative `ctx->host->should_cancel()` polling inside long native exports. `max_memory_bytes` limits bytes allocated through the plugin host allocator; if both the plugin manifest and package policy set it, Sura applies the smaller non-zero limit. `max_call_ms` records each native export call's wall-clock duration and fails the call after it returns if the budget was exceeded; with the `cancel` capability enabled, native code can stop earlier by returning `SURA_PLUGIN_CANCELLED` when `should_cancel()` becomes true. It uses the same smaller non-zero manifest/package-policy rule.

Plugins can keep per-plugin native state by filling `SuraPluginDescriptor.user_data` during `sura_plugin_init`. Sura passes that pointer back through `SuraPluginContext.user_data` on every `plugin.call`, reports it as `plugin.info(plug).has_state`, and invokes `destroy_user_data` before `plugin.unload` closes the library. Native libraries can also export optional `sura_plugin_on_load(SuraPluginContext*)` and `sura_plugin_on_unload(SuraPluginContext*)` hooks; Sura runs `on_load` after descriptor validation and runs `on_unload` before state destruction. `plugin.info` reports those hooks as `has_on_load` and `has_on_unload`.

`examples/sura_plugin_sample.c` shows the plugin descriptor shape, manifest allow-list behavior, optional lifecycle hooks, cooperative cancellation polling, and stateful `user_data` lifecycle. `tools/sura_plugin_smoke.ps1` builds that sample as a native library and verifies namespaced direct loading, manifest loading, SHA256 validation, state persistence across calls, `plugin.info`, lifecycle hook discovery/execution, `plugin.call`, `plugin.unload`, manifest export blocking, runtime `SURA_PLUGIN_POLICY=manifest-locked` enforcement, package-level `allowed_exports` narrowing, `host_capabilities` memory/cancel gating, `max_memory_bytes` quota enforcement, `max_call_ms` duration enforcement, and cooperative native cancellation. `tools/bindgen_c.ps1` and `surapkg bind-c` read simple C/C++ C-ABI headers and emit Sura wrappers that include the `ffi_call` signature string plus numeric/string `#define`, enum constants, and supported `typedef`/`using` aliases; both generators can record emitted/skipped functions and emitted constants in `sura.bind_c.v1` JSON reports, and `tools/sura_bind_c_smoke.ps1` covers standalone and package-manager bindgen output plus direct `ffi_call` wrappers and namespaced `ffi.call(...)`.

## Debugging And Profiling

The runtime exposes bytecode dumps, instruction tracing, and type-feedback profiling:

```powershell
.\SuraLanguage.exe --dump app.sura
.\SuraLanguage.exe --trace app.sura
.\SuraLanguage.exe --profile app.sura
.\SuraLanguage.exe --profile-json profile.json app.sura
.\surapkg.exe profile . --json profile.json -- --mode=fast
.\SuraLanguage.exe --debug app.sura
.\SuraLanguage.exe --debug-protocol app.sura
```

`--profile` prints a stable text report for call sites, arithmetic type feedback, branch bias, and JIT opportunities. `--profile-json <path>` enables profiling and writes the same data as JSON for CI dashboards or tooling. `surapkg profile` runs the package manifest's `main` file from the package root, enables JIT by default, forwards script arguments after `--`, and can write the same profile JSON with `--json`. `--debug` combines bytecode dump, VM instruction trace, and profiler output. Trace mode runs through the interpreter so each bytecode instruction is visible. `--debug-protocol` is the line-stop protocol used by the VS Code DAP adapter; it emits debugger events on stderr and waits for continue/step commands on stdin.

## VS Code

The extension in `sura-vscode` provides:

- Syntax highlighting for `.sura`
- Run/Debug CodeLens at the top of Sura files
- Editor title Run menu plus play/debug buttons for `.sura` files
- Command palette and editor context menu commands for run, JIT run, debug, profile, trace, and REPL
- Identifier completions from the first typed character, so `i` offers `if`, `import`, `in`, `is`, `input`, built-ins, modules, and matching current-file/workspace symbols such as `init`, `index`, or `item`
- Focused module-member completions after `console.`, including `log`, `warn`, `timeLog`, `timeEnd`, `countReset`, `groupCollapsed`, `profileEnd`, `readLine`, `style`, `color`, `stripAnsi`, `setColor`, `resetColor`, `isTTY`, `width`, `height`, `size`, and related hover/signature help
- Sura-only completion hygiene for `.sura` files: inline prose suggestions, Copilot inline completions, Copilot next-edit suggestions, word-based suggestions, raw text entries, and GitHub Copilot language auto-completions are disabled by default so long AI-style sentences do not appear in the normal Sura suggestion list
- Hover and signature help for built-ins and indexed project symbols
- The extension starts the runtime-backed `SuraLanguage.exe --lsp` server by default for diagnostics, navigation, rename, formatting, semantic tokens, and code actions; when the executable is unavailable or startup fails, it reports the failure and retains its basic in-extension completion/hover/signature providers

Build it with:

```powershell
cd sura-vscode
npm run compile
```

The default engine path is the installed `sura` command. Source checkouts can set `sura.enginePath` to `SuraLanguage.exe`; `sura.language` controls `--lang auto|en|ko`, `sura.languageServer.enabled` controls the engine-backed server, `Sura: Restart Language Server` restarts it after engine/configuration changes, and `sura.showRunCodeLens` toggles the top-of-file run/debug actions.

The engine also exposes a framed JSON-RPC/LSP endpoint:

```powershell
.\SuraLanguage.exe --lsp
```

It handles `initialize`, `textDocument/didOpen`, `textDocument/didChange`, `textDocument/didClose`, `textDocument/completion`, `textDocument/hover`, `textDocument/signatureHelp`, `textDocument/documentSymbol`, `workspace/symbol`, `textDocument/semanticTokens/full`, `textDocument/definition`, `textDocument/references`, `textDocument/formatting`, `textDocument/rename`, `textDocument/codeAction`, `shutdown`, and `exit`. On `initialize`, `rootUri` is indexed for `.sura` files, skipping common generated/vendor folders such as `.git`, `node_modules`, `sura_packages`, `dist`, and `build`; open documents override indexed disk contents. Completion items are generated from the runtime stdlib registry, so new built-ins appear without manually copying the list into the server, and dot completions for namespaced modules such as `cli.`, `json.`, `fs.`, `llm.`, and `vector.` return the matching module members instead of the whole global list. The LSP preserves dotted module calls for hover and signature help, so `cli.parse(...)`, `json.path(...)`, and `llm.request_json(...)` show module-aware signatures and active parameters. The LSP publishes block-structure, lexer, recovering parser, typechecker, and unredacted sensitive-header diagnostics, exposes function/class/enum/struct document symbols, returns semantic highlighting tokens, supports regex-backed go-to-definition/references across indexed and open documents, returns hover and signature help for user functions plus stdlib built-ins, shows inferred hover types for simple `name is expr` bindings (`number`, `string`, `bool`, `nil`, `array`, and `dict`), returns whole-document formatting edits, can rename identifiers across indexed and open documents, and offers quick fixes for missing `end` blocks, unredacted sensitive-header print/log output via `headers_redact(...)`, legacy `http_get`/`http_json`/`http_post` request helpers, and `async_http_get`/`async_http_request` helpers.

The LSP also surfaces direct `tool_call(...)` and `tool.call(...)` policy diagnostics, warns when `tool_call_policy(...)` or `tool.call_policy(...)` receives an empty policy or an HTTP policy without `url_prefixes`, and offers quick fixes that wrap single-argument direct calls with `tool_call_policy(..., policy)` or `tool.call_policy(..., policy)` and replace weak tool policies with a restricted starter policy.

## Tests

Run the stable suite:

```powershell
.\run_tests.bat
```

The suite runs `tests/*.sura` plus focused feature tests for optional chaining, `when`, improved `for in`, method chaining, Phase 9 ops, and practical stdlib APIs.

Run package tests and write a machine-readable report:

```powershell
.\SuraLanguage.exe --test .
.\SuraLanguage.exe --test-report sura-test-report.json .
.\surapkg.exe test
.\surapkg.exe test . --json sura-test-report.json --junit sura-test-report.xml
.\tools\sura_test.ps1 -Path . -Report sura-test-report.json -TimeoutSeconds 120
.\tools\sura_test.ps1 -Path . -Report sura-test-report.json -JUnit sura-test-report.xml -TimeoutSeconds 120
```

The engine and package runners look for `tests/*.sura` first. If there is no `tests` directory, they fall back to `test_*.sura`, `*_test.sura`, and `*.test.sura`.

Run the LSP protocol smoke test after changing editor tooling:

```powershell
node .\tools\sura_lsp_smoke.js .\SuraLanguage.exe
node .\tools\sura_debug_smoke.js .\SuraLanguage.exe
node .\tools\sura_debug_locals_smoke.js .\SuraLanguage.exe
node .\tools\sura_debug_exception_smoke.js .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_plugin_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_plugin_manifest_audit_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_tool_policy_audit_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_audit_report_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_policy_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_doctor_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_quality_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_ci_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_release_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_pkg_run_smoke.ps1 -Surapkg .\surapkg.exe -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_cli_args_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_undefined_variable_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_utf8_path_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_http_server_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_agent_smoke.ps1 -Surapkg .\surapkg.exe -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_profile_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_python_bridge_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_engine_test_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_check_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_engine_lint_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_engine_format_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_registry_client_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_registry_account_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_registry_report_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_registry_service_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_registry_verify_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_public_signature_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_update_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_pkg_bench_smoke.ps1 -Surapkg .\surapkg.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_bench_gate_smoke.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_bench_dashboard_smoke.ps1 -Engine .\SuraLanguage.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_ci_coverage_gate_smoke.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_release_evidence_gate_smoke.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_goal_audit_smoke.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_native_perf_baseline_smoke.ps1 -Engine .\SuraLanguage.exe
```

## Benchmarks

Run the full local benchmark dashboard immediately from the repository root:

```bat
bench.bat
```

This calls `tools/sura_bench_now.ps1`, regenerates `artifacts/native_perf.json`, `artifacts/native_perf.md`, `artifacts/bench_dashboard.html`, `artifacts/bench_dashboard.json`, `artifacts/bench_summary.md`, `artifacts/bench_release_notes.md`, and `artifacts/bench_history.json`, then opens the HTML dashboard. The native C++ baseline uses a fair-scope check: both Sura and C++ time only the same `physics step 100k` and `physics 3d step 100k` inner loops, use the same final-position checks, and record the compiler flags plus timed region in `native_perf.json` so the dashboard cannot imply a C++ comparison from mismatched measurement ranges.

Run interpreter and native-JIT benchmark passes:

```powershell
powershell -ExecutionPolicy Bypass -File .\run_benchmarks.ps1
.\surapkg.exe bench . --json package-bench.json --summary package-bench.md --python bench_reference.py --min-speedup 1.1 -- --mode=fast
powershell -ExecutionPolicy Bypass -File .\tools\sura_bench_dashboard.ps1 -Out bench_dashboard.html -JsonOut bench_dashboard.json -SummaryOut bench_summary.md -ReleaseNotesOut bench_release_notes.md -HistoryOut bench_history.json -NativePerfIn artifacts\native_perf.json
powershell -ExecutionPolicy Bypass -File .\tools\sura_bench_gate.ps1 -Report bench_dashboard.json -History bench_history.json -MaxRegressionPercent 35 -RequiredBenchmarks bench_fib.sura,bench_ai_schema.sura -RequiredPythonComparisons "fib(30)","AI JSON/schema validation"
```

Use `--bench` to get parse/typecheck/compile/execute timing and `--jit --bench` to verify native JIT behavior on the same script. Windows and Linux x86-64 share an exception-free x64 baseline (entered through the Win64 or System V convention respectively), and little-endian Windows, Linux, and macOS ARM64 have an exception-free AAPCS64 baseline. Both baseline tiers handle constant loads, moves, statically proven numeric `+`/`-`/`*`/`/` and unary negation, the six comparisons, loops and returns, and both compile guarded global reads (identity guards for function bindings, tag guards for numbers) and native-to-native direct calls between pure numeric functions, including recursion, with a depth budget that hands deep recursion back to the register VM for its own `[E500]`. On Windows x64 the baseline is tried first for replayable closures and everything it refuses goes to the existing full partial compiler. Unsupported bytecode continues in the register VM. macOS x86-64 and other ABIs currently use the register VM. Top-level JIT keeps ordinary global loads on the native path, inlines definitely-initialized global loads, and direct-calls cached named user functions after warmup, which reduces helper overhead in hot loops such as the physics benchmark. The Vec2/Vec3 physics benchmarks can use a deliberately narrow strict counted-loop shortcut instead of calling `step`/`step3` once per iteration. The shortcut is admitted only after compile-time and runtime checks prove the recognized top-level loop, function closure identity, exact `Vec2`/`Vec3` field layout and field-copy constructor, exact `add`/`scale`/`cross` and `step`/`step3` bytecode graphs, numeric inputs, and the required non-aliasing/escape conditions. Any failed proof or runtime guard leaves the original VM/native bytecode path as the fallback. When at least one iteration runs, the shortcut stores a fresh final vector instance rather than mutating the input position, so aliases to the original object keep their original fields; a zero-iteration loop keeps the original identity. Physics performance claims therefore describe this guarded benchmark-specific path and use the reproducible fair-scope benchmark contract and final-position checks above; they are not evidence of equivalent performance for arbitrary user loops. Small struct/class instances keep their first four fields inline before spilling to heap storage, so common records such as 2D and 3D vectors avoid a per-instance field-vector allocation; JIT-created instances can keep a stable class-name reference instead of copying the name per object, and `GCInstance` objects also use a slab/free-list allocator so repeated constructors avoid per-instance `new` calls. `tools/sura_instance_fields_smoke.ps1` covers inline and overflow field storage, shallow clone behavior, JIT-created instance type names, and pooled instance reuse after GC sweep. `surapkg bench` applies benchmarking to a package manifest's `main` file, runs from the package root, forwards script arguments after `--`, writes a package-scoped JSON report with `--json`, writes a Markdown release-note summary with `--summary`, can compare the JIT execute time against a Python reference script with `--python`, and can fail CI when JIT speedup is below `--min-speedup`. Python comparison scripts should print `avg (N runs): X ms` for precise apples-to-apples timing; otherwise `surapkg bench` records Python process wall time and marks the `time_source` field. `surapkg bench-dashboard --out bench_dashboard.html --json bench_dashboard.json --summary bench_summary.md --release-notes bench_release_notes.md --history-out bench_history.json --native-perf native_perf.json` wraps the dashboard generator from the package manager. The dashboard writes a human-readable HTML page with SVG bar charts for JIT speedups and Sura/Python ratios, native C++ baseline rows for Vec2 and 3D Vec3 when `native_perf.json` is available, a summary table, a history trend chart, a machine-readable JSON report with `-JsonOut`, a Markdown benchmark summary with `-SummaryOut`, a release-note-ready evidence file with `-ReleaseNotesOut`, and an append-only history file with `-HistoryIn`/`-HistoryOut` for trend tracking. The JSON report includes `summary`, `release_notes`, and optional `native_performance` objects for CI gates and release notes. `sura_pkg_bench_smoke.ps1` verifies package-level JSON and Markdown benchmarks, no-JIT mode, optional Python comparison output, and minimum-speedup failures. `sura_bench_dashboard_smoke.ps1` and `sura_bench_dashboard_cli_smoke.ps1` verify the generated HTML charts, summary metrics, native C++ Vec2 and 3D Vec3 baseline evidence, JSON report, Markdown summary, release-note evidence, history append path, API log ETL benchmark coverage, AI schema validation coverage, RAG vector ranking coverage, AI tool-policy gate benchmark coverage, AI guardrail event-scoring coverage, dependency resolver benchmark coverage, automation workflow scheduler benchmark coverage, order/CSV ETL benchmark coverage, telemetry rolling-window benchmark coverage, payment fraud-scoring coverage, feature-flag rollout coverage, and in-place and 3D game-physics benchmark coverage. `sura_bench_gate.ps1` compares the current JSON report against the previous history entry and fails when JIT timings regress beyond `-MaxRegressionPercent`; `-MinJitSpeedup` and `-MinPythonFasterBy` enforce minimum current-run performance even before a baseline exists, while `-RequiredBenchmarks` and `-RequiredPythonComparisons` fail if critical benchmark evidence disappears from the dashboard report. `sura_native_perf_baseline.ps1` compiles native C++ Vec2 and Vec3 physics-loop references and records `native_perf.json`/`native_perf.md`; the dashboard surfaces the Sura/native ratios directly so C++-class performance work is visible beside Python comparisons. `sura_goal_audit.ps1` reads native-performance evidence, repo/CI evidence, and open `Non-Negotiable Next Steps`, then records `goal_audit.json`/`goal_audit.md` with overall progress and remaining frontier work instead of treating present-day feature evidence as goal completion. `sura_release_evidence_gate.ps1` validates the final upload bundle, writes JSON/Markdown release evidence reports, records SHA256 hashes, and fails when dashboard, release-note, history, native-performance, goal-audit, required benchmark/Python comparison, or CI coverage evidence is incomplete. The dashboard also runs Python comparison cases when Python is available, including `fib(30)`, `bench_agent_scoring` for branch-heavy AI automation scoring, `bench_ai_schema` for schema-constrained agent output validation, `bench_api_log_etl` for API/tool-call log aggregation, `bench_rag_vector` for RAG-style vector ranking, `bench_tool_routing` for an AI tool/action scheduler hot loop, `bench_policy_gate` for policy-gated model tool calls, `bench_guardrail` for agent guardrail scoring, `bench_dependency_resolver` for package dependency resolver hot loops, `bench_workflow_scheduler` for dependency-ordered automation workflow scheduling, `bench_order_etl` for order/CSV normalization ETL, `bench_telemetry_window` for real-time telemetry anomaly windowing, `bench_fraud_scoring` for payment fraud risk scoring, `bench_feature_flags` for feature-flag rollout targeting, `bench_physics` for allocation-heavy Vec2 updates, `bench_physics_inplace` for in-place game-style Vec2 updates, `bench_physics3d` for 3D game-physics Vec3/cross-product updates, and `bench_market` for an object-heavy shop simulation. `.github/workflows/bench-dashboard.yml` builds Sura on Windows, runs stable tests plus benchmark gate/dashboard smoke tests, generates HTML/JSON/Markdown/release-notes/history/native-performance/goal-audit artifacts, checks regression, required benchmark coverage, CI coverage, and release evidence gates, uploads the artifacts as `sura-benchmark-dashboard`, and deploys the dashboard directory to GitHub Pages on `main` or `master`.

In that strict shortcut description, `Vec2`/`Vec3` and `step`/`step3` name the two proven formula families; they are not required source identifiers. Any user-defined class and function names are eligible when the exact x/y(/z) layout, constructor, method graph, loop shape, closure, numeric, and escape guards match.

Package manifests can opt into CI/release benchmark and protected-release evidence gates:

```json
{
  "bench": true,
  "bench_min_speedup": "1.1",
  "bench_python": "bench_reference.py",
  "bench_report": "artifacts/package-bench.json",
  "audit_report": "artifacts/audit-report.json",
  "protect_report": "artifacts/app.protect.json",
  "protect_verify_report": "artifacts/app.protect-verify.json",
  "protect_require_closed_source": true,
  "protect_require_key": true,
  "protect_require_license": true,
  "protect_require_expires": true,
  "protect_require_target": "package"
}
```

Protected source-free release packages use the runtime bytecode loader without shipping the original `.sura` file:

```powershell
.\SuraLanguage.exe --release .\app.sura
.\SuraLanguage.exe --load-release .\app.sura.srp
.\SuraLanguage.exe .\app.sura.srp
.\SuraLanguage.exe --release .\app.sura --release-key "customer-key"
.\SuraLanguage.exe --load-release .\app.sura.srp --load-release-key "customer-key"
.\SuraLanguage.exe --release .\app.sura --release-license "seat-license-42"
.\SuraLanguage.exe --load-release .\app.sura.srp --load-release-license "seat-license-42"
.\SuraLanguage.exe --release .\app.sura --release-id "customer-42" --release-expires 2027-12-31
.\surapkg.exe protect . --exe .\dist\app.exe
.\tools\sura_aot.ps1 -Source .\app.sura -Release
```

`--release` strips line/local/function/parameter debug metadata and writes a protected `.sura.srp` package. The encoded payload includes bytecode, literals, and constants behind a randomized nonce, so two builds of the same source do not produce the same package bytes, and sealed payload checks reject tampered packages before any user code runs. `SuraLanguage.exe` blocks `--dump`, `--trace`, and `--debug-protocol` for protected packages by default; set `SURA_ALLOW_RELEASE_INSPECT=1` only on a trusted owner machine when you intentionally need to inspect a release artifact. `surapkg protect --json` writes schema `sura.package.protect.v1` with leak-scan results and `next_actions` for CI/release dashboards, including ship-only guidance, key/license runtime handling, expiry suggestions, and remediation for any source or secret bytes found in protected artifacts. `surapkg protect --exe` embeds that protected package into a native Windows launcher and forwards runtime arguments to `SuraLanguage.exe`. `--release-key` makes the package require the matching `--load-release-key` or `SURA_RELEASE_KEY` environment variable at runtime. `--release-license` adds a hashed license check and mixes the license into package decoding; use `--load-release-license` or `SURA_RELEASE_LICENSE` to run it. `--release-id` records a customer/build id and `--release-expires YYYY-MM-DD` blocks execution after the expiry date. This hides source text from normal distribution and casual file scanning; it is not a substitute for server-side secrets or legal/licensing controls when an attacker has the runtime and package.
