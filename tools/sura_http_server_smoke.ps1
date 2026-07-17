param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_http_server_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Get-FreePort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return $listener.LocalEndpoint.Port
    }
    finally {
        $listener.Stop()
    }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $port = Get-FreePort
    $routePort = Get-FreePort
    $echoPort = Get-FreePort
    $script = Join-Path $temp "http_server_smoke.sura"
    $echoServer = Join-Path $temp "echo_server.js"
    $root = (Join-Path $temp "static") -replace "\\", "/"

    Write-Text $echoServer @"
const http = require("http");
let flakyCount = 0;
let flakyCheckedCount = 0;
const server = http.createServer((req, res) => {
  let body = "";
  req.setEncoding("utf8");
  req.on("data", chunk => body += chunk);
  req.on("end", () => {
    res.setHeader("Content-Type", "application/json");
    if (req.url === "/flaky") {
      flakyCount += 1;
      if (flakyCount === 1) {
        res.statusCode = 503;
        res.end(JSON.stringify({method: req.method, path: req.url, attempt: flakyCount, retry: true}));
        return;
      }
    }
    if (req.url === "/checked-fail") {
      res.statusCode = 503;
      res.end(JSON.stringify({method: req.method, path: req.url, checked: false}));
      return;
    }
    if (req.url === "/flaky-checked") {
      flakyCheckedCount += 1;
      if (flakyCheckedCount === 1) {
        res.statusCode = 503;
        res.end(JSON.stringify({method: req.method, path: req.url, attempt: flakyCheckedCount, retry: true}));
        return;
      }
      res.end(JSON.stringify({method: req.method, path: req.url, attempt: flakyCheckedCount, checked: true}));
      return;
    }
    if (req.url === "/always-fail") {
      res.statusCode = 503;
      res.end(JSON.stringify({method: req.method, path: req.url, checked: false}));
      return;
    }
    res.end(JSON.stringify({
      method: req.method,
      path: req.url,
      agent: req.headers["x-agent"] || "",
      contentType: req.headers["content-type"] || "",
      body
    }));
  });
});
server.listen($echoPort, "127.0.0.1");
"@
    $echoProcess = Start-Process -FilePath "node" -ArgumentList "`"$echoServer`"" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 600

    Write-Text $script @"
use http

root is "$root"
file_write(root + "/index.txt", "sura http server ok")
file_write(root + "/data.json", json_stringify({ok: true, name: "sura"}))

server is http_serve_static(root, $port)
sleep_ms(900)

assert_eq(server.type, "http_server")
assert_eq(server.port, $port)
assert(contains(http_server_url(server), "http://127.0.0.1:$port/"))

body is http_get(http_server_url(server) + "index.txt").trim()
assert_eq(body, "sura http server ok")
request_body is http_request({url: http_server_url(server) + "index.txt", headers: {"X-Agent": "sura-smoke"}}).trim()
assert_eq(request_body, "sura http server ok")
full_response is http_request_full({url: http_server_url(server) + "index.txt", headers: {"X-Agent": "sura-smoke"}})
assert_eq(full_response.status, 200)
assert(full_response.ok)
assert_eq(full_response.body.trim(), "sura http server ok")
data is http_request_json({url: http_server_url(server) + "data.json"})
assert(data.ok)
assert_eq(data.name, "sura")
checked_data is http_request_json_checked({url: http_server_url(server) + "data.json"})
assert(checked_data.ok)
assert_eq(checked_data.name, "sura")

echo is http_request_json({method: "POST", url: "http://127.0.0.1:$echoPort/echo", headers: {"X-Agent": "sura-smoke"}, json: {ok: true, name: "sura"}, timeout: 10})
echo_checked is http.request_json_checked({method: "POST", url: "http://127.0.0.1:$echoPort/echo", headers: {"X-Agent": "sura-smoke"}, json: {ok: true, name: "sura"}, timeout: 10})
echo_full is http_request_full({method: "POST", url: "http://127.0.0.1:$echoPort/echo", headers: {"X-Agent": "sura-smoke"}, json: {ok: true, name: "sura"}, timeout: 10})
assert_eq(echo_full.status, 200)
assert(echo_full.ok)
assert(contains(echo_full.headers["content-type"], "application/json"))
echo_full_body is json_parse(echo_full.body)
assert_eq(echo_full_body.method, "POST")
query_echo is http_request_json({url: "http://127.0.0.1:$echoPort/echo", query: {q: "sura agent", tags: ["ai", "tools"]}, timeout: 10})
assert_eq(query_echo.path, "/echo?q=sura%20agent&tags=ai&tags=tools")
aq_task is async_http_request({url: "http://127.0.0.1:$echoPort/echo", query: {mode: "async", q: "sura agent"}, timeout: 10})
aq_text is async_await(aq_task)
aq_echo is json_parse(aq_text)
assert_eq(aq_echo.path, "/echo?mode=async&q=sura%20agent")
flaky is http_request_retry({url: "http://127.0.0.1:$echoPort/flaky", timeout: 10}, 3, 10)
assert_eq(flaky.status, 200)
assert(flaky.ok)
assert_eq(flaky.attempts, 2)
flaky_body is json_parse(flaky.body)
assert_eq(flaky_body.path, "/flaky")
flaky_checked is http.request_retry_json_checked({url: "http://127.0.0.1:$echoPort/flaky-checked", timeout: 10}, 3, 10)
assert_eq(flaky_checked.path, "/flaky-checked")
assert_eq(flaky_checked.attempt, 2)
assert_eq(echo.method, "POST")
assert_eq(echo.path, "/echo")
assert_eq(echo_checked.method, "POST")
assert_eq(echo_checked.path, "/echo")
assert_eq(echo.agent, "sura-smoke")
assert(contains(echo.contentType, "application/json"))
sent is json_parse(echo.body)
assert(sent.ok)
assert_eq(sent.name, "sura")

assert(http_server_stop(server))

routes is {}
routes["GET /health"] is {json: {ok: true, name: "sura"}}
routes["POST /echo"] is {status: 201, echo: true, headers: {"X-Sura": "routes"}}
routes["/plain"] is "plain route"
route_server is http_serve_routes(routes, $routePort)
sleep_ms(900)
assert_eq(route_server.type, "http_routes_server")
assert_eq(route_server.port, $routePort)
health is http_request_json({url: http_server_url(route_server) + "health"})
assert(health.ok)
assert_eq(health.name, "sura")
plain is http_get(http_server_url(route_server) + "plain").trim()
assert_eq(plain, "plain route")
route_echo_full is http_request_full({method: "POST", url: http_server_url(route_server) + "echo?q=sura&q=agent", json: {message: "한글"}})
assert_eq(route_echo_full.status, 201)
assert_eq(route_echo_full.headers["x-sura"], "routes")
route_echo is json_parse(route_echo_full.body)
assert_eq(route_echo.method, "POST")
assert_eq(route_echo.path, "/echo")
assert_eq(route_echo.query.q[0], "sura")
assert_eq(route_echo.query.q[1], "agent")
route_echo_body is json_parse(route_echo.body)
assert_eq(route_echo_body.message, "한글")
missing_route is http_request_full({url: http_server_url(route_server) + "missing"})
assert_eq(missing_route.status, 404)
assert(http_server_stop(route_server))

print "http_server_smoke: PASS"
"@

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & $EnginePath --jit $script 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    $text = $out -join "`n"
    if ($code -ne 0 -or $text -notmatch "http_server_smoke: PASS") {
        Write-Output $text
        throw "expected http server smoke to pass"
    }

    $failScript = Join-Path $temp "http_checked_fail.sura"
    Write-Text $failScript @"
http_request_json_checked({url: "http://127.0.0.1:$echoPort/checked-fail", timeout: 10})
"@
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $failOut = & $EnginePath --jit $failScript 2>&1 | ForEach-Object { "$_" }
        $failCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    $failText = $failOut -join "`n"
    if ($failCode -eq 0 -or $failText -notmatch "HTTP status 503") {
        Write-Output $failText
        throw "expected checked JSON request to fail on HTTP 503"
    }

    $retryFailScript = Join-Path $temp "http_retry_checked_fail.sura"
    Write-Text $retryFailScript @"
http_request_retry_json_checked({url: "http://127.0.0.1:$echoPort/always-fail", timeout: 10}, 2, 10)
"@
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $retryFailOut = & $EnginePath --jit $retryFailScript 2>&1 | ForEach-Object { "$_" }
        $retryFailCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    $retryFailText = $retryFailOut -join "`n"
    if ($retryFailCode -eq 0 -or $retryFailText -notmatch "HTTP status 503") {
        Write-Output $retryFailText
        throw "expected checked retry JSON request to fail on HTTP 503"
    }

    "http_server_smoke: PASS"
}
finally {
    if ($echoProcess -and -not $echoProcess.HasExited) {
        Stop-Process -Id $echoProcess.Id -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
