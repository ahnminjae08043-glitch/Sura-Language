param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_search_" + [System.Guid]::NewGuid().ToString("N"))

function Run-Pkg {
    param([string[]]$PkgArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    Push-Location $temp
    try {
        $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
        $ErrorActionPreference = $old
    }
    return @{ Code = $code; Output = ($out -join "`n") }
}

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $env:SURA_REGISTRY = Join-Path $temp "registry"
    $env:SURA_REGISTRY_URL = $null

    $text = Run-Pkg -PkgArgs @("search", "cli.parse")
    if ($text.Code -ne 0 -or
        $text.Output -notmatch "Standard library" -or
        $text.Output -notmatch "cli\.parse\s+function\s+cli\.parse\(text, \[value_flags\]\)") {
        Write-Output $text.Output
        throw "expected search text output to include stdlib cli.parse"
    }

    $json = Run-Pkg -PkgArgs @("search", "cli.parse", "--json")
    if ($json.Code -ne 0) {
        Write-Output $json.Output
        throw "expected search cli.parse --json to pass"
    }
    $report = $json.Output | ConvertFrom-Json
    $stdlib = @($report.stdlib)
    $parse = $stdlib | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "cli" -and
        $_.name -eq "cli.parse" -and
        $_.signature -eq "cli.parse(text, [value_flags])" -and
        $_.source -eq "builtin:cli"
    } | Select-Object -First 1
    if ($report.schema -ne "sura.registry.search.v1" -or
        $report.query -ne "cli.parse" -or
        $report.count -ne 0 -or
        $report.stdlib_count -lt 1 -or
        -not $parse) {
        Write-Output $json.Output
        throw "expected search JSON to include stdlib cli.parse metadata"
    }

    $moduleJson = Run-Pkg -PkgArgs @("search", "json", "--json")
    if ($moduleJson.Code -ne 0) {
        Write-Output $moduleJson.Output
        throw "expected search json --json to pass"
    }
    $moduleReport = $moduleJson.Output | ConvertFrom-Json
    $jsonModule = @($moduleReport.stdlib) | Where-Object {
        $_.type -eq "module" -and $_.name -eq "json" -and $_.signature -eq "use json"
    } | Select-Object -First 1
    $jsonPath = @($moduleReport.stdlib) | Where-Object {
        $_.type -eq "function" -and $_.name -eq "json.path"
    } | Select-Object -First 1
    if ($moduleReport.stdlib_count -lt 2 -or -not $jsonModule -or -not $jsonPath) {
        Write-Output $moduleJson.Output
        throw "expected search JSON to include json module and json.path metadata"
    }

    $coreJson = Run-Pkg -PkgArgs @("search", "math.pow", "--json")
    if ($coreJson.Code -ne 0) {
        Write-Output $coreJson.Output
        throw "expected search math.pow --json to pass"
    }
    $coreReport = $coreJson.Output | ConvertFrom-Json
    $mathPow = @($coreReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "math" -and
        $_.name -eq "math.pow" -and
        $_.signature -eq "math.pow(base, exponent)"
    } | Select-Object -First 1
    if ($coreReport.stdlib_count -lt 1 -or -not $mathPow) {
        Write-Output $coreJson.Output
        throw "expected search JSON to include math.pow metadata"
    }

    $arrayJson = Run-Pkg -PkgArgs @("search", "array.range", "--json")
    if ($arrayJson.Code -ne 0) {
        Write-Output $arrayJson.Output
        throw "expected search array.range --json to pass"
    }
    $arrayReport = $arrayJson.Output | ConvertFrom-Json
    $arrayRange = @($arrayReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "array" -and
        $_.name -eq "array.range" -and
        $_.signature -eq "array.range(end) | array.range(start, end, [step])"
    } | Select-Object -First 1
    if ($arrayReport.stdlib_count -lt 1 -or -not $arrayRange) {
        Write-Output $arrayJson.Output
        throw "expected search JSON to include array.range metadata"
    }

    $stringJson = Run-Pkg -PkgArgs @("search", "string.chunks", "--json")
    if ($stringJson.Code -ne 0) {
        Write-Output $stringJson.Output
        throw "expected search string.chunks --json to pass"
    }
    $stringReport = $stringJson.Output | ConvertFrom-Json
    $stringChunks = @($stringReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "string" -and
        $_.name -eq "string.chunks" -and
        $_.signature -eq "string.chunks(text, [max_chars], [overlap])"
    } | Select-Object -First 1
    if ($stringReport.stdlib_count -lt 1 -or -not $stringChunks) {
        Write-Output $stringJson.Output
        throw "expected search JSON to include string.chunks metadata"
    }

    $stringLinesJson = Run-Pkg -PkgArgs @("search", "string.lines", "--json")
    if ($stringLinesJson.Code -ne 0) {
        Write-Output $stringLinesJson.Output
        throw "expected search string.lines --json to pass"
    }
    $stringLinesReport = $stringLinesJson.Output | ConvertFrom-Json
    $stringLines = @($stringLinesReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "string" -and
        $_.name -eq "string.lines" -and
        $_.signature -eq "string.lines(text)"
    } | Select-Object -First 1
    if ($stringLinesReport.stdlib_count -lt 1 -or -not $stringLines) {
        Write-Output $stringLinesJson.Output
        throw "expected search JSON to include string.lines metadata"
    }

    $setJson = Run-Pkg -PkgArgs @("search", "set.union", "--json")
    if ($setJson.Code -ne 0) {
        Write-Output $setJson.Output
        throw "expected search set.union --json to pass"
    }
    $setReport = $setJson.Output | ConvertFrom-Json
    $setUnion = @($setReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "set" -and
        $_.name -eq "set.union" -and
        $_.signature -eq "set.union(array, ...)"
    } | Select-Object -First 1
    if ($setReport.stdlib_count -lt 1 -or -not $setUnion) {
        Write-Output $setJson.Output
        throw "expected search JSON to include set.union metadata"
    }

    $regexCaptureJson = Run-Pkg -PkgArgs @("search", "regex.capture", "--json")
    if ($regexCaptureJson.Code -ne 0) {
        Write-Output $regexCaptureJson.Output
        throw "expected search regex.capture --json to pass"
    }
    $regexCaptureReport = $regexCaptureJson.Output | ConvertFrom-Json
    $regexCapture = @($regexCaptureReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "regex" -and
        $_.name -eq "regex.capture" -and
        $_.signature -eq "regex.capture(text, pattern)"
    } | Select-Object -First 1
    if ($regexCaptureReport.stdlib_count -lt 1 -or -not $regexCapture) {
        Write-Output $regexCaptureJson.Output
        throw "expected search JSON to include regex.capture metadata"
    }

    $regexEscapeJson = Run-Pkg -PkgArgs @("search", "regex.escape", "--json")
    if ($regexEscapeJson.Code -ne 0) {
        Write-Output $regexEscapeJson.Output
        throw "expected search regex.escape --json to pass"
    }
    $regexEscapeReport = $regexEscapeJson.Output | ConvertFrom-Json
    $regexEscape = @($regexEscapeReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "regex" -and
        $_.name -eq "regex.escape" -and
        $_.signature -eq "regex.escape(text)"
    } | Select-Object -First 1
    if ($regexEscapeReport.stdlib_count -lt 1 -or -not $regexEscape) {
        Write-Output $regexEscapeJson.Output
        throw "expected search JSON to include regex.escape metadata"
    }

    $dbQueryJson = Run-Pkg -PkgArgs @("search", "db.query", "--json")
    if ($dbQueryJson.Code -ne 0) {
        Write-Output $dbQueryJson.Output
        throw "expected search db.query --json to pass"
    }
    $dbQueryReport = $dbQueryJson.Output | ConvertFrom-Json
    $dbQuery = @($dbQueryReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "db" -and
        $_.name -eq "db.query" -and
        $_.signature -eq "db.query(path, [criteria], [options])"
    } | Select-Object -First 1
    if ($dbQueryReport.stdlib_count -lt 1 -or -not $dbQuery) {
        Write-Output $dbQueryJson.Output
        throw "expected search JSON to include db.query metadata"
    }

    $logLevelJson = Run-Pkg -PkgArgs @("search", "log.level", "--json")
    if ($logLevelJson.Code -ne 0) {
        Write-Output $logLevelJson.Output
        throw "expected search log.level --json to pass"
    }
    $logLevelReport = $logLevelJson.Output | ConvertFrom-Json
    $logLevel = @($logLevelReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "log" -and
        $_.name -eq "log.level" -and
        $_.signature -eq "log.level([level])"
    } | Select-Object -First 1
    if ($logLevelReport.stdlib_count -lt 1 -or -not $logLevel) {
        Write-Output $logLevelJson.Output
        throw "expected search JSON to include log.level metadata"
    }

    $osJson = Run-Pkg -PkgArgs @("search", "os.env_load", "--json")
    if ($osJson.Code -ne 0) {
        Write-Output $osJson.Output
        throw "expected search os.env_load --json to pass"
    }
    $osReport = $osJson.Output | ConvertFrom-Json
    $osEnvLoad = @($osReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "os" -and
        $_.name -eq "os.env_load" -and
        $_.signature -eq "os.env_load(path, [override])"
    } | Select-Object -First 1
    if ($osReport.stdlib_count -lt 1 -or -not $osEnvLoad) {
        Write-Output $osJson.Output
        throw "expected search JSON to include os.env_load metadata"
    }

    $osTempDirJson = Run-Pkg -PkgArgs @("search", "os.temp_dir", "--json")
    if ($osTempDirJson.Code -ne 0) {
        Write-Output $osTempDirJson.Output
        throw "expected search os.temp_dir --json to pass"
    }
    $osTempDirReport = $osTempDirJson.Output | ConvertFrom-Json
    $osTempDir = @($osTempDirReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "os" -and
        $_.name -eq "os.temp_dir" -and
        $_.signature -eq "os.temp_dir()"
    } | Select-Object -First 1
    if ($osTempDirReport.stdlib_count -lt 1 -or -not $osTempDir) {
        Write-Output $osTempDirJson.Output
        throw "expected search JSON to include os.temp_dir metadata"
    }

    $osWhichJson = Run-Pkg -PkgArgs @("search", "os.which", "--json")
    if ($osWhichJson.Code -ne 0) {
        Write-Output $osWhichJson.Output
        throw "expected search os.which --json to pass"
    }
    $osWhichReport = $osWhichJson.Output | ConvertFrom-Json
    $osWhich = @($osWhichReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "os" -and
        $_.name -eq "os.which" -and
        $_.signature -eq "os.which(command)"
    } | Select-Object -First 1
    if ($osWhichReport.stdlib_count -lt 1 -or -not $osWhich) {
        Write-Output $osWhichJson.Output
        throw "expected search JSON to include os.which metadata"
    }

    $osCmdJoinJson = Run-Pkg -PkgArgs @("search", "os.cmd_join", "--json")
    if ($osCmdJoinJson.Code -ne 0) {
        Write-Output $osCmdJoinJson.Output
        throw "expected search os.cmd_join --json to pass"
    }
    $osCmdJoinReport = $osCmdJoinJson.Output | ConvertFrom-Json
    $osCmdJoin = @($osCmdJoinReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "os" -and
        $_.name -eq "os.cmd_join" -and
        $_.signature -eq "os.cmd_join(args)"
    } | Select-Object -First 1
    if ($osCmdJoinReport.stdlib_count -lt 1 -or -not $osCmdJoin) {
        Write-Output $osCmdJoinJson.Output
        throw "expected search JSON to include os.cmd_join metadata"
    }

    $osRunJson = Run-Pkg -PkgArgs @("search", "os.run", "--json")
    if ($osRunJson.Code -ne 0) {
        Write-Output $osRunJson.Output
        throw "expected search os.run --json to pass"
    }
    $osRunReport = $osRunJson.Output | ConvertFrom-Json
    $osRun = @($osRunReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "os" -and
        $_.name -eq "os.run" -and
        $_.signature -eq "os.run(command)"
    } | Select-Object -First 1
    if ($osRunReport.stdlib_count -lt 1 -or -not $osRun) {
        Write-Output $osRunJson.Output
        throw "expected search JSON to include os.run metadata"
    }

    $osRunCheckedJson = Run-Pkg -PkgArgs @("search", "os.run_checked", "--json")
    if ($osRunCheckedJson.Code -ne 0) {
        Write-Output $osRunCheckedJson.Output
        throw "expected search os.run_checked --json to pass"
    }
    $osRunCheckedReport = $osRunCheckedJson.Output | ConvertFrom-Json
    $osRunChecked = @($osRunCheckedReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "os" -and
        $_.name -eq "os.run_checked" -and
        $_.signature -eq "os.run_checked(command)"
    } | Select-Object -First 1
    if ($osRunCheckedReport.stdlib_count -lt 1 -or -not $osRunChecked) {
        Write-Output $osRunCheckedJson.Output
        throw "expected search JSON to include os.run_checked metadata"
    }

    $osWaitJson = Run-Pkg -PkgArgs @("search", "os.wait", "--json")
    if ($osWaitJson.Code -ne 0) {
        Write-Output $osWaitJson.Output
        throw "expected search os.wait --json to pass"
    }
    $osWaitReport = $osWaitJson.Output | ConvertFrom-Json
    $osWait = @($osWaitReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "os" -and
        $_.name -eq "os.wait" -and
        $_.signature -eq "os.wait(milliseconds)"
    } | Select-Object -First 1
    if ($osWaitReport.stdlib_count -lt 1 -or -not $osWait) {
        Write-Output $osWaitJson.Output
        throw "expected search JSON to include os.wait metadata"
    }

    $asyncSleepJson = Run-Pkg -PkgArgs @("search", "async.sleep", "--json")
    if ($asyncSleepJson.Code -ne 0) {
        Write-Output $asyncSleepJson.Output
        throw "expected search async.sleep --json to pass"
    }
    $asyncSleepReport = $asyncSleepJson.Output | ConvertFrom-Json
    $asyncSleep = @($asyncSleepReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "async" -and
        $_.name -eq "async.sleep" -and
        $_.signature -eq "async.sleep(milliseconds, [scope_id])"
    } | Select-Object -First 1
    if ($asyncSleepReport.stdlib_count -lt 1 -or -not $asyncSleep) {
        Write-Output $asyncSleepJson.Output
        throw "expected search JSON to include async.sleep metadata"
    }

    $httpCheckedJson = Run-Pkg -PkgArgs @("search", "http.request_json_checked", "--json")
    if ($httpCheckedJson.Code -ne 0) {
        Write-Output $httpCheckedJson.Output
        throw "expected search http.request_json_checked --json to pass"
    }
    $httpCheckedReport = $httpCheckedJson.Output | ConvertFrom-Json
    $httpChecked = @($httpCheckedReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.request_json_checked" -and
        $_.signature -eq "http.request_json_checked(spec)"
    } | Select-Object -First 1
    if ($httpCheckedReport.stdlib_count -lt 1 -or -not $httpChecked) {
        Write-Output $httpCheckedJson.Output
        throw "expected search JSON to include http.request_json_checked metadata"
    }

    $httpRetryCheckedJson = Run-Pkg -PkgArgs @("search", "http.request_retry_json_checked", "--json")
    if ($httpRetryCheckedJson.Code -ne 0) {
        Write-Output $httpRetryCheckedJson.Output
        throw "expected search http.request_retry_json_checked --json to pass"
    }
    $httpRetryCheckedReport = $httpRetryCheckedJson.Output | ConvertFrom-Json
    $httpRetryChecked = @($httpRetryCheckedReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.request_retry_json_checked" -and
        $_.signature -eq "http.request_retry_json_checked(spec, [attempts], [delay_ms])"
    } | Select-Object -First 1
    if ($httpRetryCheckedReport.stdlib_count -lt 1 -or -not $httpRetryChecked) {
        Write-Output $httpRetryCheckedJson.Output
        throw "expected search JSON to include http.request_retry_json_checked metadata"
    }

    $httpHeadersJson = Run-Pkg -PkgArgs @("search", "http.headers_merge", "--json")
    if ($httpHeadersJson.Code -ne 0) {
        Write-Output $httpHeadersJson.Output
        throw "expected search http.headers_merge --json to pass"
    }
    $httpHeadersReport = $httpHeadersJson.Output | ConvertFrom-Json
    $httpHeaders = @($httpHeadersReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.headers_merge" -and
        $_.signature -eq "http.headers_merge(headers, ...)"
    } | Select-Object -First 1
    if ($httpHeadersReport.stdlib_count -lt 1 -or -not $httpHeaders) {
        Write-Output $httpHeadersJson.Output
        throw "expected search JSON to include http.headers_merge metadata"
    }

    $httpHeadersGetJson = Run-Pkg -PkgArgs @("search", "http.headers_get", "--json")
    if ($httpHeadersGetJson.Code -ne 0) {
        Write-Output $httpHeadersGetJson.Output
        throw "expected search http.headers_get --json to pass"
    }
    $httpHeadersGetReport = $httpHeadersGetJson.Output | ConvertFrom-Json
    $httpHeadersGet = @($httpHeadersGetReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.headers_get" -and
        $_.signature -eq "http.headers_get(headers, name, [default])"
    } | Select-Object -First 1
    if ($httpHeadersGetReport.stdlib_count -lt 1 -or -not $httpHeadersGet) {
        Write-Output $httpHeadersGetJson.Output
        throw "expected search JSON to include http.headers_get metadata"
    }

    $httpHeadersRedactJson = Run-Pkg -PkgArgs @("search", "http.headers_redact", "--json")
    if ($httpHeadersRedactJson.Code -ne 0) {
        Write-Output $httpHeadersRedactJson.Output
        throw "expected search http.headers_redact --json to pass"
    }
    $httpHeadersRedactReport = $httpHeadersRedactJson.Output | ConvertFrom-Json
    $httpHeadersRedact = @($httpHeadersRedactReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.headers_redact" -and
        $_.signature -eq "http.headers_redact(headers, [names], [mask])"
    } | Select-Object -First 1
    if ($httpHeadersRedactReport.stdlib_count -lt 1 -or -not $httpHeadersRedact) {
        Write-Output $httpHeadersRedactJson.Output
        throw "expected search JSON to include http.headers_redact metadata"
    }

    $httpCookieParseJson = Run-Pkg -PkgArgs @("search", "http.cookie_parse", "--json")
    if ($httpCookieParseJson.Code -ne 0) {
        Write-Output $httpCookieParseJson.Output
        throw "expected search http.cookie_parse --json to pass"
    }
    $httpCookieParseReport = $httpCookieParseJson.Output | ConvertFrom-Json
    $httpCookieParse = @($httpCookieParseReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.cookie_parse" -and
        $_.signature -eq "http.cookie_parse(header_or_headers)"
    } | Select-Object -First 1
    if ($httpCookieParseReport.stdlib_count -lt 1 -or -not $httpCookieParse) {
        Write-Output $httpCookieParseJson.Output
        throw "expected search JSON to include http.cookie_parse metadata"
    }

    $httpFormBuildJson = Run-Pkg -PkgArgs @("search", "http.form_build", "--json")
    if ($httpFormBuildJson.Code -ne 0) {
        Write-Output $httpFormBuildJson.Output
        throw "expected search http.form_build --json to pass"
    }
    $httpFormBuildReport = $httpFormBuildJson.Output | ConvertFrom-Json
    $httpFormBuild = @($httpFormBuildReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.form_build" -and
        $_.signature -eq "http.form_build(params)"
    } | Select-Object -First 1
    if ($httpFormBuildReport.stdlib_count -lt 1 -or -not $httpFormBuild) {
        Write-Output $httpFormBuildJson.Output
        throw "expected search JSON to include http.form_build metadata"
    }

    $httpContentTypeJson = Run-Pkg -PkgArgs @("search", "http.content_type", "--json")
    if ($httpContentTypeJson.Code -ne 0) {
        Write-Output $httpContentTypeJson.Output
        throw "expected search http.content_type --json to pass"
    }
    $httpContentTypeReport = $httpContentTypeJson.Output | ConvertFrom-Json
    $httpContentType = @($httpContentTypeReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.content_type" -and
        $_.signature -eq "http.content_type(headers_or_value, [default])"
    } | Select-Object -First 1
    if ($httpContentTypeReport.stdlib_count -lt 1 -or -not $httpContentType) {
        Write-Output $httpContentTypeJson.Output
        throw "expected search JSON to include http.content_type metadata"
    }

    $httpIsJsonJson = Run-Pkg -PkgArgs @("search", "http.is_json", "--json")
    if ($httpIsJsonJson.Code -ne 0) {
        Write-Output $httpIsJsonJson.Output
        throw "expected search http.is_json --json to pass"
    }
    $httpIsJsonReport = $httpIsJsonJson.Output | ConvertFrom-Json
    $httpIsJson = @($httpIsJsonReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.is_json" -and
        $_.signature -eq "http.is_json(headers_or_value)"
    } | Select-Object -First 1
    if ($httpIsJsonReport.stdlib_count -lt 1 -or -not $httpIsJson) {
        Write-Output $httpIsJsonJson.Output
        throw "expected search JSON to include http.is_json metadata"
    }

    $httpUrlParseJson = Run-Pkg -PkgArgs @("search", "http.url_parse", "--json")
    if ($httpUrlParseJson.Code -ne 0) {
        Write-Output $httpUrlParseJson.Output
        throw "expected search http.url_parse --json to pass"
    }
    $httpUrlParseReport = $httpUrlParseJson.Output | ConvertFrom-Json
    $httpUrlParse = @($httpUrlParseReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.url_parse" -and
        $_.signature -eq "http.url_parse(url)"
    } | Select-Object -First 1
    if ($httpUrlParseReport.stdlib_count -lt 1 -or -not $httpUrlParse) {
        Write-Output $httpUrlParseJson.Output
        throw "expected search JSON to include http.url_parse metadata"
    }

    $httpStatusTextJson = Run-Pkg -PkgArgs @("search", "http.status_text", "--json")
    if ($httpStatusTextJson.Code -ne 0) {
        Write-Output $httpStatusTextJson.Output
        throw "expected search http.status_text --json to pass"
    }
    $httpStatusTextReport = $httpStatusTextJson.Output | ConvertFrom-Json
    $httpStatusText = @($httpStatusTextReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.status_text" -and
        $_.signature -eq "http.status_text(status)"
    } | Select-Object -First 1
    if ($httpStatusTextReport.stdlib_count -lt 1 -or -not $httpStatusText) {
        Write-Output $httpStatusTextJson.Output
        throw "expected search JSON to include http.status_text metadata"
    }

    $httpRetryAfterJson = Run-Pkg -PkgArgs @("search", "http.retry_after", "--json")
    if ($httpRetryAfterJson.Code -ne 0) {
        Write-Output $httpRetryAfterJson.Output
        throw "expected search http.retry_after --json to pass"
    }
    $httpRetryAfterReport = $httpRetryAfterJson.Output | ConvertFrom-Json
    $httpRetryAfter = @($httpRetryAfterReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.retry_after" -and
        $_.signature -eq "http.retry_after(headers_or_value, [default_ms])"
    } | Select-Object -First 1
    if ($httpRetryAfterReport.stdlib_count -lt 1 -or -not $httpRetryAfter) {
        Write-Output $httpRetryAfterJson.Output
        throw "expected search JSON to include http.retry_after metadata"
    }

    $httpBackoffDelaysJson = Run-Pkg -PkgArgs @("search", "http.backoff_delays", "--json")
    if ($httpBackoffDelaysJson.Code -ne 0) {
        Write-Output $httpBackoffDelaysJson.Output
        throw "expected search http.backoff_delays --json to pass"
    }
    $httpBackoffDelaysReport = $httpBackoffDelaysJson.Output | ConvertFrom-Json
    $httpBackoffDelays = @($httpBackoffDelaysReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "http" -and
        $_.name -eq "http.backoff_delays" -and
        $_.signature -eq "http.backoff_delays(attempts, [base_ms], [factor], [max_ms])"
    } | Select-Object -First 1
    if ($httpBackoffDelaysReport.stdlib_count -lt 1 -or -not $httpBackoffDelays) {
        Write-Output $httpBackoffDelaysJson.Output
        throw "expected search JSON to include http.backoff_delays metadata"
    }

    $arrayJson = Run-Pkg -PkgArgs @("search", "array.slice", "--json")
    if ($arrayJson.Code -ne 0) {
        Write-Output $arrayJson.Output
        throw "expected search array.slice --json to pass"
    }
    $arrayReport = $arrayJson.Output | ConvertFrom-Json
    $arraySlice = @($arrayReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "array" -and
        $_.name -eq "array.slice" -and
        $_.signature -eq "array.slice(array, start, [end])"
    } | Select-Object -First 1
    if ($arrayReport.stdlib_count -lt 1 -or -not $arraySlice) {
        Write-Output $arrayJson.Output
        throw "expected search JSON to include array.slice metadata"
    }

    $pathJson = Run-Pkg -PkgArgs @("search", "path.join", "--json")
    if ($pathJson.Code -ne 0) {
        Write-Output $pathJson.Output
        throw "expected search path.join --json to pass"
    }
    $pathReport = $pathJson.Output | ConvertFrom-Json
    $pathJoin = @($pathReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "path" -and
        $_.name -eq "path.join" -and
        $_.signature -eq "path.join(part, ...)"
    } | Select-Object -First 1
    if ($pathReport.stdlib_count -lt 1 -or -not $pathJoin) {
        Write-Output $pathJson.Output
        throw "expected search JSON to include path.join metadata"
    }

    $pythonJson = Run-Pkg -PkgArgs @("search", "python.call_json", "--json")
    if ($pythonJson.Code -ne 0) {
        Write-Output $pythonJson.Output
        throw "expected search python.call_json --json to pass"
    }
    $pythonReport = $pythonJson.Output | ConvertFrom-Json
    $pythonCallJson = @($pythonReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "python" -and
        $_.name -eq "python.call_json" -and
        $_.signature -eq "python.call_json(module, function, [args], [kwargs])"
    } | Select-Object -First 1
    if ($pythonReport.stdlib_count -lt 1 -or -not $pythonCallJson) {
        Write-Output $pythonJson.Output
        throw "expected search JSON to include python.call_json metadata"
    }

    $ffiJson = Run-Pkg -PkgArgs @("search", "ffi.call", "--json")
    if ($ffiJson.Code -ne 0) {
        Write-Output $ffiJson.Output
        throw "expected search ffi.call --json to pass"
    }
    $ffiReport = $ffiJson.Output | ConvertFrom-Json
    $ffiCall = @($ffiReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "ffi" -and
        $_.name -eq "ffi.call" -and
        $_.signature -eq "ffi.call(lib, symbol, signature, ...args)"
    } | Select-Object -First 1
    if ($ffiReport.stdlib_count -lt 1 -or -not $ffiCall) {
        Write-Output $ffiJson.Output
        throw "expected search JSON to include ffi.call metadata"
    }

    $pluginJson = Run-Pkg -PkgArgs @("search", "plugin.call", "--json")
    if ($pluginJson.Code -ne 0) {
        Write-Output $pluginJson.Output
        throw "expected search plugin.call --json to pass"
    }
    $pluginReport = $pluginJson.Output | ConvertFrom-Json
    $pluginCall = @($pluginReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "plugin" -and
        $_.name -eq "plugin.call" -and
        $_.signature -eq "plugin.call(plugin, export, ...args)"
    } | Select-Object -First 1
    if ($pluginReport.stdlib_count -lt 1 -or -not $pluginCall) {
        Write-Output $pluginJson.Output
        throw "expected search JSON to include plugin.call metadata"
    }

    $llmJson = Run-Pkg -PkgArgs @("search", "llm.request_json", "--json")
    if ($llmJson.Code -ne 0) {
        Write-Output $llmJson.Output
        throw "expected search llm.request_json --json to pass"
    }
    $llmReport = $llmJson.Output | ConvertFrom-Json
    $llmRequestJson = @($llmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.request_json" -and
        $_.signature -eq "llm.request_json(model, messages, [temperature])"
    } | Select-Object -First 1
    if ($llmReport.stdlib_count -lt 1 -or -not $llmRequestJson) {
        Write-Output $llmJson.Output
        throw "expected search JSON to include llm.request_json metadata"
    }

    $schemaLlmJson = Run-Pkg -PkgArgs @("search", "llm.request_schema_json", "--json")
    if ($schemaLlmJson.Code -ne 0) {
        Write-Output $schemaLlmJson.Output
        throw "expected search llm.request_schema_json --json to pass"
    }
    $schemaLlmReport = $schemaLlmJson.Output | ConvertFrom-Json
    $llmRequestSchemaJson = @($schemaLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.request_schema_json" -and
        $_.signature -eq "llm.request_schema_json(model, messages, schema, [temperature], [name], [strict])"
    } | Select-Object -First 1
    if ($schemaLlmReport.stdlib_count -lt 1 -or -not $llmRequestSchemaJson) {
        Write-Output $schemaLlmJson.Output
        throw "expected search JSON to include llm.request_schema_json metadata"
    }

    $tensorMeanJson = Run-Pkg -PkgArgs @("search", "tensor.mean", "--json")
    if ($tensorMeanJson.Code -ne 0) {
        Write-Output $tensorMeanJson.Output
        throw "expected search tensor.mean --json to pass"
    }
    $tensorMeanReport = $tensorMeanJson.Output | ConvertFrom-Json
    $tensorMean = @($tensorMeanReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.mean" -and
        $_.signature -eq "tensor.mean(tensor)"
    } | Select-Object -First 1
    if ($tensorMeanReport.stdlib_count -lt 1 -or -not $tensorMean) {
        Write-Output $tensorMeanJson.Output
        throw "expected search JSON to include tensor.mean metadata"
    }

    $tensorClipJson = Run-Pkg -PkgArgs @("search", "tensor.clip", "--json")
    if ($tensorClipJson.Code -ne 0) {
        Write-Output $tensorClipJson.Output
        throw "expected search tensor.clip --json to pass"
    }
    $tensorClipReport = $tensorClipJson.Output | ConvertFrom-Json
    $tensorClip = @($tensorClipReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.clip" -and
        $_.signature -eq "tensor.clip(tensor, min, max)"
    } | Select-Object -First 1
    if ($tensorClipReport.stdlib_count -lt 1 -or -not $tensorClip) {
        Write-Output $tensorClipJson.Output
        throw "expected search JSON to include tensor.clip metadata"
    }

    $tensorVarianceJson = Run-Pkg -PkgArgs @("search", "tensor.variance", "--json")
    if ($tensorVarianceJson.Code -ne 0) {
        Write-Output $tensorVarianceJson.Output
        throw "expected search tensor.variance --json to pass"
    }
    $tensorVarianceReport = $tensorVarianceJson.Output | ConvertFrom-Json
    $tensorVariance = @($tensorVarianceReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.variance" -and
        $_.signature -eq "tensor.variance(tensor)"
    } | Select-Object -First 1
    if ($tensorVarianceReport.stdlib_count -lt 1 -or -not $tensorVariance) {
        Write-Output $tensorVarianceJson.Output
        throw "expected search JSON to include tensor.variance metadata"
    }

    $tensorStdJson = Run-Pkg -PkgArgs @("search", "tensor.std", "--json")
    if ($tensorStdJson.Code -ne 0) {
        Write-Output $tensorStdJson.Output
        throw "expected search tensor.std --json to pass"
    }
    $tensorStdReport = $tensorStdJson.Output | ConvertFrom-Json
    $tensorStd = @($tensorStdReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.std" -and
        $_.signature -eq "tensor.std(tensor)"
    } | Select-Object -First 1
    if ($tensorStdReport.stdlib_count -lt 1 -or -not $tensorStd) {
        Write-Output $tensorStdJson.Output
        throw "expected search JSON to include tensor.std metadata"
    }

    $tensorMaxJson = Run-Pkg -PkgArgs @("search", "tensor.max", "--json")
    if ($tensorMaxJson.Code -ne 0) {
        Write-Output $tensorMaxJson.Output
        throw "expected search tensor.max --json to pass"
    }
    $tensorMaxReport = $tensorMaxJson.Output | ConvertFrom-Json
    $tensorMax = @($tensorMaxReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.max" -and
        $_.signature -eq "tensor.max(tensor)"
    } | Select-Object -First 1
    if ($tensorMaxReport.stdlib_count -lt 1 -or -not $tensorMax) {
        Write-Output $tensorMaxJson.Output
        throw "expected search JSON to include tensor.max metadata"
    }

    $tensorArgminJson = Run-Pkg -PkgArgs @("search", "tensor.argmin", "--json")
    if ($tensorArgminJson.Code -ne 0) {
        Write-Output $tensorArgminJson.Output
        throw "expected search tensor.argmin --json to pass"
    }
    $tensorArgminReport = $tensorArgminJson.Output | ConvertFrom-Json
    $tensorArgmin = @($tensorArgminReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.argmin" -and
        $_.signature -eq "tensor.argmin(tensor)"
    } | Select-Object -First 1
    if ($tensorArgminReport.stdlib_count -lt 1 -or -not $tensorArgmin) {
        Write-Output $tensorArgminJson.Output
        throw "expected search JSON to include tensor.argmin metadata"
    }

    $tensorArgmaxJson = Run-Pkg -PkgArgs @("search", "tensor.argmax", "--json")
    if ($tensorArgmaxJson.Code -ne 0) {
        Write-Output $tensorArgmaxJson.Output
        throw "expected search tensor.argmax --json to pass"
    }
    $tensorArgmaxReport = $tensorArgmaxJson.Output | ConvertFrom-Json
    $tensorArgmax = @($tensorArgmaxReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.argmax" -and
        $_.signature -eq "tensor.argmax(tensor)"
    } | Select-Object -First 1
    if ($tensorArgmaxReport.stdlib_count -lt 1 -or -not $tensorArgmax) {
        Write-Output $tensorArgmaxJson.Output
        throw "expected search JSON to include tensor.argmax metadata"
    }

    $tensorZscoreJson = Run-Pkg -PkgArgs @("search", "tensor.zscore", "--json")
    if ($tensorZscoreJson.Code -ne 0) {
        Write-Output $tensorZscoreJson.Output
        throw "expected search tensor.zscore --json to pass"
    }
    $tensorZscoreReport = $tensorZscoreJson.Output | ConvertFrom-Json
    $tensorZscore = @($tensorZscoreReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.zscore" -and
        $_.signature -eq "tensor.zscore(tensor)"
    } | Select-Object -First 1
    if ($tensorZscoreReport.stdlib_count -lt 1 -or -not $tensorZscore) {
        Write-Output $tensorZscoreJson.Output
        throw "expected search JSON to include tensor.zscore metadata"
    }

    $tensorSoftmaxJson = Run-Pkg -PkgArgs @("search", "tensor.softmax", "--json")
    if ($tensorSoftmaxJson.Code -ne 0) {
        Write-Output $tensorSoftmaxJson.Output
        throw "expected search tensor.softmax --json to pass"
    }
    $tensorSoftmaxReport = $tensorSoftmaxJson.Output | ConvertFrom-Json
    $tensorSoftmax = @($tensorSoftmaxReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "tensor" -and
        $_.name -eq "tensor.softmax" -and
        $_.signature -eq "tensor.softmax(tensor)"
    } | Select-Object -First 1
    if ($tensorSoftmaxReport.stdlib_count -lt 1 -or -not $tensorSoftmax) {
        Write-Output $tensorSoftmaxJson.Output
        throw "expected search JSON to include tensor.softmax metadata"
    }

    $toolsSchemaLlmJson = Run-Pkg -PkgArgs @("search", "llm.request_tools_schema_json", "--json")
    if ($toolsSchemaLlmJson.Code -ne 0) {
        Write-Output $toolsSchemaLlmJson.Output
        throw "expected search llm.request_tools_schema_json --json to pass"
    }
    $toolsSchemaLlmReport = $toolsSchemaLlmJson.Output | ConvertFrom-Json
    $llmRequestToolsSchemaJson = @($toolsSchemaLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.request_tools_schema_json" -and
        $_.signature -eq "llm.request_tools_schema_json(model, messages, tool_names, schema, [temperature], [name], [strict])"
    } | Select-Object -First 1
    if ($toolsSchemaLlmReport.stdlib_count -lt 1 -or -not $llmRequestToolsSchemaJson) {
        Write-Output $toolsSchemaLlmJson.Output
        throw "expected search JSON to include llm.request_tools_schema_json metadata"
    }

    $chatRequestLlmJson = Run-Pkg -PkgArgs @("search", "llm.chat_request", "--json")
    if ($chatRequestLlmJson.Code -ne 0) {
        Write-Output $chatRequestLlmJson.Output
        throw "expected search llm.chat_request --json to pass"
    }
    $chatRequestLlmReport = $chatRequestLlmJson.Output | ConvertFrom-Json
    $llmChatRequest = @($chatRequestLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.chat_request" -and
        $_.signature -eq "llm.chat_request(endpoint, api_key, request)"
    } | Select-Object -First 1
    if ($chatRequestLlmReport.stdlib_count -lt 1 -or -not $llmChatRequest) {
        Write-Output $chatRequestLlmJson.Output
        throw "expected search JSON to include llm.chat_request metadata"
    }

    $extractJsonLlmJson = Run-Pkg -PkgArgs @("search", "llm.extract_json", "--json")
    if ($extractJsonLlmJson.Code -ne 0) {
        Write-Output $extractJsonLlmJson.Output
        throw "expected search llm.extract_json --json to pass"
    }
    $extractJsonLlmReport = $extractJsonLlmJson.Output | ConvertFrom-Json
    $llmExtractJson = @($extractJsonLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.extract_json" -and
        $_.signature -eq "llm.extract_json(response, [schema])"
    } | Select-Object -First 1
    if ($extractJsonLlmReport.stdlib_count -lt 1 -or -not $llmExtractJson) {
        Write-Output $extractJsonLlmJson.Output
        throw "expected search JSON to include llm.extract_json metadata"
    }

    $toolCallsLlmJson = Run-Pkg -PkgArgs @("search", "llm.tool_calls", "--json")
    if ($toolCallsLlmJson.Code -ne 0) {
        Write-Output $toolCallsLlmJson.Output
        throw "expected search llm.tool_calls --json to pass"
    }
    $toolCallsLlmReport = $toolCallsLlmJson.Output | ConvertFrom-Json
    $llmToolCalls = @($toolCallsLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.tool_calls" -and
        $_.signature -eq "llm.tool_calls(response)"
    } | Select-Object -First 1
    if ($toolCallsLlmReport.stdlib_count -lt 1 -or -not $llmToolCalls) {
        Write-Output $toolCallsLlmJson.Output
        throw "expected search JSON to include llm.tool_calls metadata"
    }

    $toolResultLlmJson = Run-Pkg -PkgArgs @("search", "llm.tool_result", "--json")
    if ($toolResultLlmJson.Code -ne 0) {
        Write-Output $toolResultLlmJson.Output
        throw "expected search llm.tool_result --json to pass"
    }
    $toolResultLlmReport = $toolResultLlmJson.Output | ConvertFrom-Json
    $llmToolResult = @($toolResultLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.tool_result" -and
        $_.signature -eq "llm.tool_result(call_or_id, result)"
    } | Select-Object -First 1
    if ($toolResultLlmReport.stdlib_count -lt 1 -or -not $llmToolResult) {
        Write-Output $toolResultLlmJson.Output
        throw "expected search JSON to include llm.tool_result metadata"
    }

    $runToolsLlmJson = Run-Pkg -PkgArgs @("search", "llm.run_tools", "--json")
    if ($runToolsLlmJson.Code -ne 0) {
        Write-Output $runToolsLlmJson.Output
        throw "expected search llm.run_tools --json to pass"
    }
    $runToolsLlmReport = $runToolsLlmJson.Output | ConvertFrom-Json
    $llmRunTools = @($runToolsLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.run_tools" -and
        $_.signature -eq "llm.run_tools(response, policy)"
    } | Select-Object -First 1
    if ($runToolsLlmReport.stdlib_count -lt 1 -or -not $llmRunTools) {
        Write-Output $runToolsLlmJson.Output
        throw "expected search JSON to include llm.run_tools metadata"
    }

    $nextMessagesLlmJson = Run-Pkg -PkgArgs @("search", "llm.next_messages", "--json")
    if ($nextMessagesLlmJson.Code -ne 0) {
        Write-Output $nextMessagesLlmJson.Output
        throw "expected search llm.next_messages --json to pass"
    }
    $nextMessagesLlmReport = $nextMessagesLlmJson.Output | ConvertFrom-Json
    $llmNextMessages = @($nextMessagesLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.next_messages" -and
        $_.signature -eq "llm.next_messages(messages, response, policy)"
    } | Select-Object -First 1
    if ($nextMessagesLlmReport.stdlib_count -lt 1 -or -not $llmNextMessages) {
        Write-Output $nextMessagesLlmJson.Output
        throw "expected search JSON to include llm.next_messages metadata"
    }

    $nextRequestLlmJson = Run-Pkg -PkgArgs @("search", "llm.next_request", "--json")
    if ($nextRequestLlmJson.Code -ne 0) {
        Write-Output $nextRequestLlmJson.Output
        throw "expected search llm.next_request --json to pass"
    }
    $nextRequestLlmReport = $nextRequestLlmJson.Output | ConvertFrom-Json
    $llmNextRequest = @($nextRequestLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.next_request" -and
        $_.signature -eq "llm.next_request(model, messages, response, policy, tool_names, [temperature])"
    } | Select-Object -First 1
    if ($nextRequestLlmReport.stdlib_count -lt 1 -or -not $llmNextRequest) {
        Write-Output $nextRequestLlmJson.Output
        throw "expected search JSON to include llm.next_request metadata"
    }

    $nextSchemaRequestLlmJson = Run-Pkg -PkgArgs @("search", "llm.next_schema_request", "--json")
    if ($nextSchemaRequestLlmJson.Code -ne 0) {
        Write-Output $nextSchemaRequestLlmJson.Output
        throw "expected search llm.next_schema_request --json to pass"
    }
    $nextSchemaRequestLlmReport = $nextSchemaRequestLlmJson.Output | ConvertFrom-Json
    $llmNextSchemaRequest = @($nextSchemaRequestLlmReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.next_schema_request" -and
        $_.signature -eq "llm.next_schema_request(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])"
    } | Select-Object -First 1
    if ($nextSchemaRequestLlmReport.stdlib_count -lt 1 -or -not $llmNextSchemaRequest) {
        Write-Output $nextSchemaRequestLlmJson.Output
        throw "expected search JSON to include llm.next_schema_request metadata"
    }

    $llmToolsJson = Run-Pkg -PkgArgs @("search", "llm.tools", "--json")
    if ($llmToolsJson.Code -ne 0) {
        Write-Output $llmToolsJson.Output
        throw "expected search llm.tools --json to pass"
    }
    $llmToolsReport = $llmToolsJson.Output | ConvertFrom-Json
    $llmTools = @($llmToolsReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "llm" -and
        $_.name -eq "llm.tools" -and
        $_.signature -eq "llm.tools([names])"
    } | Select-Object -First 1
    if ($llmToolsReport.stdlib_count -lt 1 -or -not $llmTools) {
        Write-Output $llmToolsJson.Output
        throw "expected search JSON to include llm.tools metadata"
    }

    $jsonSseJson = Run-Pkg -PkgArgs @("search", "json.sse_data", "--json")
    if ($jsonSseJson.Code -ne 0) {
        Write-Output $jsonSseJson.Output
        throw "expected search json.sse_data --json to pass"
    }
    $jsonSseReport = $jsonSseJson.Output | ConvertFrom-Json
    $jsonSseData = @($jsonSseReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "json" -and
        $_.name -eq "json.sse_data" -and
        $_.signature -eq "json.sse_data(text, [parse_json])"
    } | Select-Object -First 1
    if ($jsonSseReport.stdlib_count -lt 1 -or -not $jsonSseData) {
        Write-Output $jsonSseJson.Output
        throw "expected search JSON to include json.sse_data metadata"
    }

    $jsonIniJson = Run-Pkg -PkgArgs @("search", "json.ini_parse", "--json")
    if ($jsonIniJson.Code -ne 0) {
        Write-Output $jsonIniJson.Output
        throw "expected search json.ini_parse --json to pass"
    }
    $jsonIniReport = $jsonIniJson.Output | ConvertFrom-Json
    $jsonIniParse = @($jsonIniReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "json" -and
        $_.name -eq "json.ini_parse" -and
        $_.signature -eq "json.ini_parse(text)"
    } | Select-Object -First 1
    if ($jsonIniReport.stdlib_count -lt 1 -or -not $jsonIniParse) {
        Write-Output $jsonIniJson.Output
        throw "expected search JSON to include json.ini_parse metadata"
    }

    $jsonPrettyJson = Run-Pkg -PkgArgs @("search", "json.pretty", "--json")
    if ($jsonPrettyJson.Code -ne 0) {
        Write-Output $jsonPrettyJson.Output
        throw "expected search json.pretty --json to pass"
    }
    $jsonPrettyReport = $jsonPrettyJson.Output | ConvertFrom-Json
    $jsonPretty = @($jsonPrettyReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "json" -and
        $_.name -eq "json.pretty" -and
        $_.signature -eq "json.pretty(value, [indent])"
    } | Select-Object -First 1
    if ($jsonPrettyReport.stdlib_count -lt 1 -or -not $jsonPretty) {
        Write-Output $jsonPrettyJson.Output
        throw "expected search JSON to include json.pretty metadata"
    }

    $fsReadJsonJson = Run-Pkg -PkgArgs @("search", "fs.read_json", "--json")
    if ($fsReadJsonJson.Code -ne 0) {
        Write-Output $fsReadJsonJson.Output
        throw "expected search fs.read_json --json to pass"
    }
    $fsReadJsonReport = $fsReadJsonJson.Output | ConvertFrom-Json
    $fsReadJson = @($fsReadJsonReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "fs" -and
        $_.name -eq "fs.read_json" -and
        $_.signature -eq "fs.read_json(path)"
    } | Select-Object -First 1
    if ($fsReadJsonReport.stdlib_count -lt 1 -or -not $fsReadJson) {
        Write-Output $fsReadJsonJson.Output
        throw "expected search JSON to include fs.read_json metadata"
    }

    $fsReadBytesJson = Run-Pkg -PkgArgs @("search", "fs.read_bytes", "--json")
    if ($fsReadBytesJson.Code -ne 0) {
        Write-Output $fsReadBytesJson.Output
        throw "expected search fs.read_bytes --json to pass"
    }
    $fsReadBytesReport = $fsReadBytesJson.Output | ConvertFrom-Json
    $fsReadBytes = @($fsReadBytesReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "fs" -and
        $_.name -eq "fs.read_bytes" -and
        $_.signature -eq "fs.read_bytes(path)"
    } | Select-Object -First 1
    if ($fsReadBytesReport.stdlib_count -lt 1 -or -not $fsReadBytes) {
        Write-Output $fsReadBytesJson.Output
        throw "expected search JSON to include fs.read_bytes metadata"
    }

    $fsSha256Json = Run-Pkg -PkgArgs @("search", "fs.sha256", "--json")
    if ($fsSha256Json.Code -ne 0) {
        Write-Output $fsSha256Json.Output
        throw "expected search fs.sha256 --json to pass"
    }
    $fsSha256Report = $fsSha256Json.Output | ConvertFrom-Json
    $fsSha256 = @($fsSha256Report.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "fs" -and
        $_.name -eq "fs.sha256" -and
        $_.signature -eq "fs.sha256(path)"
    } | Select-Object -First 1
    if ($fsSha256Report.stdlib_count -lt 1 -or -not $fsSha256) {
        Write-Output $fsSha256Json.Output
        throw "expected search JSON to include fs.sha256 metadata"
    }

    $cryptoFileSha256Json = Run-Pkg -PkgArgs @("search", "crypto.file_sha256", "--json")
    if ($cryptoFileSha256Json.Code -ne 0) {
        Write-Output $cryptoFileSha256Json.Output
        throw "expected search crypto.file_sha256 --json to pass"
    }
    $cryptoFileSha256Report = $cryptoFileSha256Json.Output | ConvertFrom-Json
    $cryptoFileSha256 = @($cryptoFileSha256Report.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "crypto" -and
        $_.name -eq "crypto.file_sha256" -and
        $_.signature -eq "crypto.file_sha256(path)"
    } | Select-Object -First 1
    if ($cryptoFileSha256Report.stdlib_count -lt 1 -or -not $cryptoFileSha256) {
        Write-Output $cryptoFileSha256Json.Output
        throw "expected search JSON to include crypto.file_sha256 metadata"
    }

    $cryptoFileHmacSha256Json = Run-Pkg -PkgArgs @("search", "crypto.file_hmac_sha256", "--json")
    if ($cryptoFileHmacSha256Json.Code -ne 0) {
        Write-Output $cryptoFileHmacSha256Json.Output
        throw "expected search crypto.file_hmac_sha256 --json to pass"
    }
    $cryptoFileHmacSha256Report = $cryptoFileHmacSha256Json.Output | ConvertFrom-Json
    $cryptoFileHmacSha256 = @($cryptoFileHmacSha256Report.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "crypto" -and
        $_.name -eq "crypto.file_hmac_sha256" -and
        $_.signature -eq "crypto.file_hmac_sha256(key, path)"
    } | Select-Object -First 1
    if ($cryptoFileHmacSha256Report.stdlib_count -lt 1 -or -not $cryptoFileHmacSha256) {
        Write-Output $cryptoFileHmacSha256Json.Output
        throw "expected search JSON to include crypto.file_hmac_sha256 metadata"
    }

    $cryptoRandomHexJson = Run-Pkg -PkgArgs @("search", "crypto.random_hex", "--json")
    if ($cryptoRandomHexJson.Code -ne 0) {
        Write-Output $cryptoRandomHexJson.Output
        throw "expected search crypto.random_hex --json to pass"
    }
    $cryptoRandomHexReport = $cryptoRandomHexJson.Output | ConvertFrom-Json
    $cryptoRandomHex = @($cryptoRandomHexReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "crypto" -and
        $_.name -eq "crypto.random_hex" -and
        $_.signature -eq "crypto.random_hex(count)"
    } | Select-Object -First 1
    if ($cryptoRandomHexReport.stdlib_count -lt 1 -or -not $cryptoRandomHex) {
        Write-Output $cryptoRandomHexJson.Output
        throw "expected search JSON to include crypto.random_hex metadata"
    }

    $cryptoConstantTimeEqJson = Run-Pkg -PkgArgs @("search", "crypto.constant_time_eq", "--json")
    if ($cryptoConstantTimeEqJson.Code -ne 0) {
        Write-Output $cryptoConstantTimeEqJson.Output
        throw "expected search crypto.constant_time_eq --json to pass"
    }
    $cryptoConstantTimeEqReport = $cryptoConstantTimeEqJson.Output | ConvertFrom-Json
    $cryptoConstantTimeEq = @($cryptoConstantTimeEqReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "crypto" -and
        $_.name -eq "crypto.constant_time_eq" -and
        $_.signature -eq "crypto.constant_time_eq(left, right)"
    } | Select-Object -First 1
    if ($cryptoConstantTimeEqReport.stdlib_count -lt 1 -or -not $cryptoConstantTimeEq) {
        Write-Output $cryptoConstantTimeEqJson.Output
        throw "expected search JSON to include crypto.constant_time_eq metadata"
    }

    $fsGlobJson = Run-Pkg -PkgArgs @("search", "fs.glob", "--json")
    if ($fsGlobJson.Code -ne 0) {
        Write-Output $fsGlobJson.Output
        throw "expected search fs.glob --json to pass"
    }
    $fsGlobReport = $fsGlobJson.Output | ConvertFrom-Json
    $fsGlob = @($fsGlobReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "fs" -and
        $_.name -eq "fs.glob" -and
        $_.signature -eq "fs.glob(pattern)"
    } | Select-Object -First 1
    if ($fsGlobReport.stdlib_count -lt 1 -or -not $fsGlob) {
        Write-Output $fsGlobJson.Output
        throw "expected search JSON to include fs.glob metadata"
    }

    $graphicsProjectJson = Run-Pkg -PkgArgs @("search", "graphics3d.project", "--json")
    if ($graphicsProjectJson.Code -ne 0) {
        Write-Output $graphicsProjectJson.Output
        throw "expected search graphics3d.project --json to pass"
    }
    $graphicsProjectReport = $graphicsProjectJson.Output | ConvertFrom-Json
    $graphicsProject = @($graphicsProjectReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "graphics3d" -and
        $_.name -eq "graphics3d.project" -and
        $_.signature -eq "graphics3d.project(point, camera, [width], [height])"
    } | Select-Object -First 1
    if ($graphicsProjectReport.stdlib_count -lt 1 -or -not $graphicsProject) {
        Write-Output $graphicsProjectJson.Output
        throw "expected search JSON to include graphics3d.project metadata"
    }

    $nnTrainJson = Run-Pkg -PkgArgs @("search", "nn.train", "--json")
    if ($nnTrainJson.Code -ne 0) {
        Write-Output $nnTrainJson.Output
        throw "expected search nn.train --json to pass"
    }
    $nnTrainReport = $nnTrainJson.Output | ConvertFrom-Json
    $nnTrain = @($nnTrainReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "nn" -and
        $_.name -eq "nn.train" -and
        $_.signature -eq "nn.train(model, inputs, targets, [options])"
    } | Select-Object -First 1
    if ($nnTrainReport.stdlib_count -lt 1 -or -not $nnTrain) {
        Write-Output $nnTrainJson.Output
        throw "expected search JSON to include nn.train metadata"
    }

    $autogradBackwardJson = Run-Pkg -PkgArgs @("search", "autograd.backward", "--json")
    if ($autogradBackwardJson.Code -ne 0) {
        Write-Output $autogradBackwardJson.Output
        throw "expected search autograd.backward --json to pass"
    }
    $autogradBackwardReport = $autogradBackwardJson.Output | ConvertFrom-Json
    $autogradBackward = @($autogradBackwardReport.stdlib) | Where-Object {
        $_.type -eq "function" -and
        $_.module -eq "autograd" -and
        $_.name -eq "autograd.backward" -and
        $_.signature -eq "autograd.backward(tensor, [gradient], [retain_graph])"
    } | Select-Object -First 1
    if ($autogradBackwardReport.stdlib_count -lt 1 -or -not $autogradBackward) {
        Write-Output $autogradBackwardJson.Output
        throw "expected search JSON to include autograd.backward metadata"
    }

    "pkg_search_smoke: PASS"
}
finally {
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
