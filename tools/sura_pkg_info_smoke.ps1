param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_info_" + [System.Guid]::NewGuid().ToString("N"))

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

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $text = Run-Pkg -PkgArgs @("info", "cli")
    if ($text.Code -ne 0 -or
        $text.Output -notmatch "cli \(stdlib\)" -or
        $text.Output -notmatch "parse\s+\(builtin:cli\)") {
        Write-Output $text.Output
        throw "expected builtin cli info text output"
    }

    $json = Run-Pkg -PkgArgs @("info", "cli", "--json")
    if ($json.Code -ne 0) {
        Write-Output $json.Output
        throw "expected info cli --json to pass"
    }
    $info = $json.Output | ConvertFrom-Json
    $symbols = @($info.symbols)
    $parse = $symbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "parse" -and
        $_.signature -eq "cli.parse(text, [value_flags])" -and
        $_.source -eq "builtin:cli"
    } | Select-Object -First 1
    if ($info.schema -ne "sura.package.info.v1" -or
        -not $info.passed -or
        $info.source -ne "stdlib" -or
        $info.package -ne "cli" -or
        $info.path -ne "builtin:cli" -or
        [int]$info.symbol_count -lt 4 -or
        -not $parse) {
        Write-Output $json.Output
        throw "expected builtin cli info JSON metadata"
    }

    $aliasJson = Run-Pkg -PkgArgs @("info", "logging", "--json")
    if ($aliasJson.Code -ne 0) {
        Write-Output $aliasJson.Output
        throw "expected info logging --json alias to pass"
    }
    $alias = $aliasJson.Output | ConvertFrom-Json
    $aliasSymbols = @($alias.symbols)
    $warn = $aliasSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "warn" -and
        $_.signature -eq "log.warn(message)" -and
        $_.source -eq "builtin:log"
    } | Select-Object -First 1
    if ($alias.schema -ne "sura.package.info.v1" -or
        -not $alias.passed -or
        $alias.query -ne "logging" -or
        $alias.source -ne "stdlib" -or
        $alias.package -ne "log" -or
        $alias.path -ne "builtin:log" -or
        -not $warn) {
        Write-Output $aliasJson.Output
        throw "expected logging alias to resolve to builtin log metadata"
    }

    $nnJson = Run-Pkg -PkgArgs @("info", "ai", "--json")
    if ($nnJson.Code -ne 0) {
        Write-Output $nnJson.Output
        throw "expected info ai --json alias to pass"
    }
    $nn = $nnJson.Output | ConvertFrom-Json
    $nnTrain = @($nn.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "train" -and
        $_.signature -eq "nn.train(model, inputs, targets, [options])" -and
        $_.source -eq "builtin:nn"
    } | Select-Object -First 1
    if ($nn.schema -ne "sura.package.info.v1" -or
        -not $nn.passed -or
        $nn.query -ne "ai" -or
        $nn.package -ne "nn" -or
        $nn.path -ne "builtin:nn" -or
        -not $nnTrain) {
        Write-Output $nnJson.Output
        throw "expected ai alias to expose native nn metadata"
    }

    $autogradJson = Run-Pkg -PkgArgs @("info", "autograd", "--json")
    if ($autogradJson.Code -ne 0) {
        Write-Output $autogradJson.Output
        throw "expected info autograd --json to pass"
    }
    $autograd = $autogradJson.Output | ConvertFrom-Json
    $autogradBackward = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "backward" -and
        $_.signature -eq "autograd.backward(tensor, [gradient], [retain_graph])" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradAdam = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "adam" -and
        $_.signature -eq "autograd.adam(parameters, learning_rate, [options])" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradRequiresGrad = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "requires_grad" -and
        $_.signature -eq "autograd.requires_grad(tensor)" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradLimits = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "limits" -and
        $_.signature -eq "autograd.limits()" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradAttention = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "causal_attention" -and
        $_.signature -eq "autograd.causal_attention(query, key, value, [options])" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradSaveCheckpoint = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "save_checkpoint" -and
        $_.signature -eq "autograd.save_checkpoint(state_dict, path, [options])" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradLoadCheckpoint = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "load_checkpoint" -and
        $_.signature -eq "autograd.load_checkpoint(path, [options])" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradDtype = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "dtype" -and
        $_.signature -eq "autograd.dtype(tensor)" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradDevice = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "device" -and
        $_.signature -eq "autograd.device(tensor)" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradTo = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "to" -and
        $_.signature -eq "autograd.to(tensor, device)" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradCudaInfo = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "cuda_info" -and
        $_.signature -eq "autograd.cuda_info()" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradCudaStats = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "cuda_stats" -and
        $_.signature -eq "autograd.cuda_stats()" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradCudaResetStats = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "cuda_reset_stats" -and
        $_.signature -eq "autograd.cuda_reset_stats()" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradSaveSafetensors = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "save_safetensors" -and
        $_.signature -eq "autograd.save_safetensors(state_dict, path)" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradLoadOnnx = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "load_onnx_weights" -and
        $_.signature -eq "autograd.load_onnx_weights(path, [options])" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradRunOnnx = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "run_onnx" -and
        $_.signature -eq "autograd.run_onnx(path, inputs, [options])" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    $autogradAllReduce = @($autograd.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "all_reduce_gradients" -and
        $_.signature -eq "autograd.all_reduce_gradients(parameters, options)" -and
        $_.source -eq "builtin:autograd"
    } | Select-Object -First 1
    if ($autograd.schema -ne "sura.package.info.v1" -or
        -not $autograd.passed -or
        $autograd.package -ne "autograd" -or
        $autograd.path -ne "builtin:autograd" -or
        [int]$autograd.symbol_count -ne 63 -or
        -not $autogradBackward -or
        -not $autogradAdam -or
        -not $autogradRequiresGrad -or
        -not $autogradLimits -or
        -not $autogradAttention -or
        -not $autogradSaveCheckpoint -or
        -not $autogradLoadCheckpoint -or
        -not $autogradDtype -or
        -not $autogradDevice -or
        -not $autogradTo -or
        -not $autogradCudaInfo -or
        -not $autogradCudaStats -or
        -not $autogradCudaResetStats -or
        -not $autogradSaveSafetensors -or
        -not $autogradLoadOnnx -or
        -not $autogradRunOnnx -or
        -not $autogradAllReduce) {
        Write-Output $autogradJson.Output
        throw "expected autograd metadata to expose all 63 tensor, device, interop, CUDA, and distributed APIs"
    }

    $tokenizerJson = Run-Pkg -PkgArgs @("info", "tokenizer", "--json")
    if ($tokenizerJson.Code -ne 0) {
        Write-Output $tokenizerJson.Output
        throw "expected info tokenizer --json to pass"
    }
    $tokenizer = $tokenizerJson.Output | ConvertFrom-Json
    $tokenizerByte = @($tokenizer.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "byte" -and
        $_.signature -eq "tokenizer.byte([options])" -and
        $_.source -eq "builtin:tokenizer"
    } | Select-Object -First 1
    $tokenizerTrainBpe = @($tokenizer.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "train_bpe" -and
        $_.signature -eq "tokenizer.train_bpe(corpus, [options])" -and
        $_.source -eq "builtin:tokenizer"
    } | Select-Object -First 1
    $tokenizerEncode = @($tokenizer.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "encode" -and
        $_.signature -eq "tokenizer.encode(tokenizer, text, [options])" -and
        $_.source -eq "builtin:tokenizer"
    } | Select-Object -First 1
    $tokenizerLoad = @($tokenizer.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "load" -and
        $_.signature -eq "tokenizer.load(path)" -and
        $_.source -eq "builtin:tokenizer"
    } | Select-Object -First 1
    if ($tokenizer.schema -ne "sura.package.info.v1" -or
        -not $tokenizer.passed -or
        $tokenizer.package -ne "tokenizer" -or
        $tokenizer.path -ne "builtin:tokenizer" -or
        [int]$tokenizer.symbol_count -ne 7 -or
        -not $tokenizerByte -or
        -not $tokenizerTrainBpe -or
        -not $tokenizerEncode -or
        -not $tokenizerLoad) {
        Write-Output $tokenizerJson.Output
        throw "expected tokenizer metadata to expose exactly seven native APIs"
    }

    $datasetJson = Run-Pkg -PkgArgs @("info", "dataset", "--json")
    if ($datasetJson.Code -ne 0) {
        Write-Output $datasetJson.Output
        throw "expected info dataset --json to pass"
    }
    $dataset = $datasetJson.Output | ConvertFrom-Json
    $datasetPack = @($dataset.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "pack_text" -and
        $_.signature -eq "dataset.pack_text(source, tokenizer, path, [options])" -and
        $_.source -eq "builtin:dataset"
    } | Select-Object -First 1
    $datasetOpen = @($dataset.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "open" -and
        $_.signature -eq "dataset.open(path, [options])" -and
        $_.source -eq "builtin:dataset"
    } | Select-Object -First 1
    $datasetNext = @($dataset.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "next" -and
        $_.signature -eq "dataset.next(loader)" -and
        $_.source -eq "builtin:dataset"
    } | Select-Object -First 1
    if ($dataset.schema -ne "sura.package.info.v1" -or
        -not $dataset.passed -or
        $dataset.package -ne "dataset" -or
        $dataset.path -ne "builtin:dataset" -or
        [int]$dataset.symbol_count -ne 6 -or
        -not $datasetPack -or
        -not $datasetOpen -or
        -not $datasetNext) {
        Write-Output $datasetJson.Output
        throw "expected dataset metadata to expose exactly six streaming APIs"
    }

    $llmJson = Run-Pkg -PkgArgs @("info", "llm", "--json")
    if ($llmJson.Code -ne 0) {
        Write-Output $llmJson.Output
        throw "expected info llm --json to pass"
    }
    $llm = $llmJson.Output | ConvertFrom-Json
    $llmSymbols = @($llm.symbols)
    $requestJson = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "request_json" -and
        $_.signature -eq "llm.request_json(model, messages, [temperature])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $requestSchemaJson = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "request_schema_json" -and
        $_.signature -eq "llm.request_schema_json(model, messages, schema, [temperature], [name], [strict])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $toolsSymbol = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "tools" -and
        $_.signature -eq "llm.tools([names])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $requestToolsSchemaJson = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "request_tools_schema_json" -and
        $_.signature -eq "llm.request_tools_schema_json(model, messages, tool_names, schema, [temperature], [name], [strict])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $chatRequest = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "chat_request" -and
        $_.signature -eq "llm.chat_request(endpoint, api_key, request)" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $extractJson = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "extract_json" -and
        $_.signature -eq "llm.extract_json(response, [schema])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $toolCalls = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "tool_calls" -and
        $_.signature -eq "llm.tool_calls(response)" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $toolResult = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "tool_result" -and
        $_.signature -eq "llm.tool_result(call_or_id, result)" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $runTools = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "run_tools" -and
        $_.signature -eq "llm.run_tools(response, policy)" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $nextMessages = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "next_messages" -and
        $_.signature -eq "llm.next_messages(messages, response, policy)" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $nextRequest = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "next_request" -and
        $_.signature -eq "llm.next_request(model, messages, response, policy, tool_names, [temperature])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $nextRequestJson = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "next_request_json" -and
        $_.signature -eq "llm.next_request_json(model, messages, response, policy, tool_names, [temperature])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $nextSchemaRequest = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "next_schema_request" -and
        $_.signature -eq "llm.next_schema_request(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    $nextSchemaRequestJson = $llmSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "next_schema_request_json" -and
        $_.signature -eq "llm.next_schema_request_json(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])" -and
        $_.source -eq "builtin:llm"
    } | Select-Object -First 1
    if ($llm.schema -ne "sura.package.info.v1" -or
        -not $llm.passed -or
        $llm.source -ne "stdlib" -or
        $llm.package -ne "llm" -or
        $llm.path -ne "builtin:llm" -or
        [int]$llm.symbol_count -lt 29 -or
        -not $requestJson -or
        -not $requestSchemaJson -or
        -not $toolsSymbol -or
        -not $requestToolsSchemaJson -or
        -not $chatRequest -or
        -not $extractJson -or
        -not $toolCalls -or
        -not $toolResult -or
        -not $runTools -or
        -not $nextMessages -or
        -not $nextRequest -or
        -not $nextRequestJson -or
        -not $nextSchemaRequest -or
        -not $nextSchemaRequestJson) {
        Write-Output $llmJson.Output
        throw "expected builtin llm info JSON metadata"
    }

    $jsonInfo = Run-Pkg -PkgArgs @("info", "json", "--json")
    if ($jsonInfo.Code -ne 0) {
        Write-Output $jsonInfo.Output
        throw "expected info json --json to pass"
    }
    $jsonModule = $jsonInfo.Output | ConvertFrom-Json
    $jsonSymbols = @($jsonModule.symbols)
    $jsonTemplate = $jsonSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "template_render" -and
        $_.signature -eq "json.template_render(text, data, [missing])" -and
        $_.source -eq "builtin:json"
    } | Select-Object -First 1
    $jsonCountBy = $jsonSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "count_by" -and
        $_.signature -eq "json.count_by(rows, path)" -and
        $_.source -eq "builtin:json"
    } | Select-Object -First 1
    $jsonSseData = $jsonSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "sse_data" -and
        $_.signature -eq "json.sse_data(text, [parse_json])" -and
        $_.source -eq "builtin:json"
    } | Select-Object -First 1
    $jsonIniParse = $jsonSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "ini_parse" -and
        $_.signature -eq "json.ini_parse(text)" -and
        $_.source -eq "builtin:json"
    } | Select-Object -First 1
    $jsonPretty = $jsonSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "pretty" -and
        $_.signature -eq "json.pretty(value, [indent])" -and
        $_.source -eq "builtin:json"
    } | Select-Object -First 1
    if ($jsonModule.schema -ne "sura.package.info.v1" -or
        -not $jsonModule.passed -or
        $jsonModule.source -ne "stdlib" -or
        $jsonModule.package -ne "json" -or
        $jsonModule.path -ne "builtin:json" -or
        [int]$jsonModule.symbol_count -lt 19 -or
        -not $jsonTemplate -or
        -not $jsonCountBy -or
        -not $jsonSseData -or
        -not $jsonIniParse -or
        -not $jsonPretty) {
        Write-Output $jsonInfo.Output
        throw "expected builtin json info JSON metadata"
    }

    $fsJson = Run-Pkg -PkgArgs @("info", "fs", "--json")
    if ($fsJson.Code -ne 0) {
        Write-Output $fsJson.Output
        throw "expected info fs --json to pass"
    }
    $fsModule = $fsJson.Output | ConvertFrom-Json
    $fsSymbols = @($fsModule.symbols)
    $fsReadJson = $fsSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "read_json" -and
        $_.signature -eq "fs.read_json(path)" -and
        $_.source -eq "builtin:fs"
    } | Select-Object -First 1
    $fsWriteJson = $fsSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "write_json" -and
        $_.signature -eq "fs.write_json(path, value)" -and
        $_.source -eq "builtin:fs"
    } | Select-Object -First 1
    $fsGlob = $fsSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "glob" -and
        $_.signature -eq "fs.glob(pattern)" -and
        $_.source -eq "builtin:fs"
    } | Select-Object -First 1
    $fsReadBytes = $fsSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "read_bytes" -and
        $_.signature -eq "fs.read_bytes(path)" -and
        $_.source -eq "builtin:fs"
    } | Select-Object -First 1
    $fsWriteBytes = $fsSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "write_bytes" -and
        $_.signature -eq "fs.write_bytes(path, bytes)" -and
        $_.source -eq "builtin:fs"
    } | Select-Object -First 1
    $fsSha256 = $fsSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "sha256" -and
        $_.signature -eq "fs.sha256(path)" -and
        $_.source -eq "builtin:fs"
    } | Select-Object -First 1
    if ($fsModule.schema -ne "sura.package.info.v1" -or
        -not $fsModule.passed -or
        $fsModule.source -ne "stdlib" -or
        $fsModule.package -ne "fs" -or
        $fsModule.path -ne "builtin:fs" -or
        -not $fsReadJson -or
        -not $fsWriteJson -or
        -not $fsGlob -or
        -not $fsReadBytes -or
        -not $fsWriteBytes -or
        -not $fsSha256) {
        Write-Output $fsJson.Output
        throw "expected builtin fs info JSON metadata"
    }

    $cryptoJson = Run-Pkg -PkgArgs @("info", "crypto", "--json")
    if ($cryptoJson.Code -ne 0) {
        Write-Output $cryptoJson.Output
        throw "expected info crypto --json to pass"
    }
    $cryptoModule = $cryptoJson.Output | ConvertFrom-Json
    $cryptoSymbols = @($cryptoModule.symbols)
    $cryptoSha256 = $cryptoSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "sha256" -and
        $_.signature -eq "crypto.sha256(text)" -and
        $_.source -eq "builtin:crypto"
    } | Select-Object -First 1
    $cryptoFileSha256 = $cryptoSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "file_sha256" -and
        $_.signature -eq "crypto.file_sha256(path)" -and
        $_.source -eq "builtin:crypto"
    } | Select-Object -First 1
    $cryptoFileHmacSha256 = $cryptoSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "file_hmac_sha256" -and
        $_.signature -eq "crypto.file_hmac_sha256(key, path)" -and
        $_.source -eq "builtin:crypto"
    } | Select-Object -First 1
    $cryptoRandomBytes = $cryptoSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "random_bytes" -and
        $_.signature -eq "crypto.random_bytes(count)" -and
        $_.source -eq "builtin:crypto"
    } | Select-Object -First 1
    $cryptoRandomHex = $cryptoSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "random_hex" -and
        $_.signature -eq "crypto.random_hex(count)" -and
        $_.source -eq "builtin:crypto"
    } | Select-Object -First 1
    $cryptoConstantTimeEq = $cryptoSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "constant_time_eq" -and
        $_.signature -eq "crypto.constant_time_eq(left, right)" -and
        $_.source -eq "builtin:crypto"
    } | Select-Object -First 1
    if ($cryptoModule.schema -ne "sura.package.info.v1" -or
        -not $cryptoModule.passed -or
        $cryptoModule.source -ne "stdlib" -or
        $cryptoModule.package -ne "crypto" -or
        $cryptoModule.path -ne "builtin:crypto" -or
        [int]$cryptoModule.symbol_count -lt 16 -or
        -not $cryptoSha256 -or
        -not $cryptoFileSha256 -or
        -not $cryptoFileHmacSha256 -or
        -not $cryptoRandomBytes -or
        -not $cryptoRandomHex -or
        -not $cryptoConstantTimeEq) {
        Write-Output $cryptoJson.Output
        throw "expected builtin crypto info JSON metadata"
    }

    $asyncJson = Run-Pkg -PkgArgs @("info", "async", "--json")
    if ($asyncJson.Code -ne 0) {
        Write-Output $asyncJson.Output
        throw "expected info async --json to pass"
    }
    $async = $asyncJson.Output | ConvertFrom-Json
    $asyncSymbols = @($async.symbols)
    $asyncSleep = $asyncSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "sleep" -and
        $_.signature -eq "async.sleep(milliseconds, [scope_id])" -and
        $_.source -eq "builtin:async"
    } | Select-Object -First 1
    $asyncAwaitTimeout = $asyncSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "await_timeout" -and
        $_.signature -eq "async.await_timeout(task_id, milliseconds, [default])" -and
        $_.source -eq "builtin:async"
    } | Select-Object -First 1
    $asyncCancel = $asyncSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "cancel" -and
        $_.signature -eq "async.cancel(task_id)" -and
        $_.source -eq "builtin:async"
    } | Select-Object -First 1
    $asyncScopeClose = $asyncSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "scope_close" -and
        $_.signature -eq "async.scope_close(scope_id, [milliseconds])" -and
        $_.source -eq "builtin:async"
    } | Select-Object -First 1
    if ($async.schema -ne "sura.package.info.v1" -or
        -not $async.passed -or
        $async.source -ne "stdlib" -or
        $async.package -ne "async" -or
        $async.path -ne "builtin:async" -or
        [int]$async.symbol_count -lt 25 -or
        -not $asyncSleep -or
        -not $asyncAwaitTimeout -or
        -not $asyncCancel -or
        -not $asyncScopeClose) {
        Write-Output $asyncJson.Output
        throw "expected builtin async info JSON metadata"
    }

    $testJson = Run-Pkg -PkgArgs @("info", "test", "--json")
    if ($testJson.Code -ne 0) {
        Write-Output $testJson.Output
        throw "expected info test --json to pass"
    }
    $test = $testJson.Output | ConvertFrom-Json
    $testSymbols = @($test.symbols)
    $testApprox = $testSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "approx" -and
        $_.signature -eq "test.approx(actual, expected, [epsilon], [message])" -and
        $_.source -eq "builtin:test"
    } | Select-Object -First 1
    $testNotContains = $testSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "not_contains" -and
        $_.signature -eq "test.not_contains(container, value, [message])" -and
        $_.source -eq "builtin:test"
    } | Select-Object -First 1
    $testCheckMatch = $testSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "check_match" -and
        $_.signature -eq "test.check_match(name, text, pattern, [message])" -and
        $_.source -eq "builtin:test"
    } | Select-Object -First 1
    if ($test.schema -ne "sura.package.info.v1" -or
        -not $test.passed -or
        $test.source -ne "stdlib" -or
        $test.package -ne "test" -or
        $test.path -ne "builtin:test" -or
        [int]$test.symbol_count -lt 16 -or
        -not $testApprox -or
        -not $testNotContains -or
        -not $testCheckMatch) {
        Write-Output $testJson.Output
        throw "expected builtin test info JSON metadata"
    }

    $httpJson = Run-Pkg -PkgArgs @("info", "http", "--json")
    if ($httpJson.Code -ne 0) {
        Write-Output $httpJson.Output
        throw "expected info http --json to pass"
    }
    $http = $httpJson.Output | ConvertFrom-Json
    $httpSymbols = @($http.symbols)
    $httpCheckedJson = $httpSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "request_json_checked" -and
        $_.signature -eq "http.request_json_checked(spec)" -and
        $_.source -eq "builtin:http"
    } | Select-Object -First 1
    $httpRetryCheckedJson = $httpSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "request_retry_json_checked" -and
        $_.signature -eq "http.request_retry_json_checked(spec, [attempts], [delay_ms])" -and
        $_.source -eq "builtin:http"
    } | Select-Object -First 1
    $httpAuthBearer = $httpSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "auth_bearer" -and
        $_.signature -eq "http.auth_bearer(token)" -and
        $_.source -eq "builtin:http"
    } | Select-Object -First 1
    $httpHeadersMerge = $httpSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "headers_merge" -and
        $_.signature -eq "http.headers_merge(headers, ...)" -and
        $_.source -eq "builtin:http"
    } | Select-Object -First 1
    $httpQueryParse = $httpSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "query_parse" -and
        $_.signature -eq "http.query_parse(query)" -and
        $_.source -eq "builtin:http"
    } | Select-Object -First 1
    $httpServerUrl = $httpSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "server_url" -and
        $_.signature -eq "http.server_url(server)" -and
        $_.source -eq "builtin:http"
    } | Select-Object -First 1
    if ($http.schema -ne "sura.package.info.v1" -or
        -not $http.passed -or
        $http.source -ne "stdlib" -or
        $http.package -ne "http" -or
        $http.path -ne "builtin:http" -or
        [int]$http.symbol_count -lt 19 -or
        -not $httpCheckedJson -or
        -not $httpRetryCheckedJson -or
        -not $httpAuthBearer -or
        -not $httpHeadersMerge -or
        -not $httpQueryParse -or
        -not $httpServerUrl) {
        Write-Output $httpJson.Output
        throw "expected builtin http info JSON metadata"
    }

    $randomJson = Run-Pkg -PkgArgs @("info", "random", "--json")
    if ($randomJson.Code -ne 0) {
        Write-Output $randomJson.Output
        throw "expected info random --json to pass"
    }
    $random = $randomJson.Output | ConvertFrom-Json
    $randomSymbols = @($random.symbols)
    $randomSeed = $randomSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "seed" -and
        $_.signature -eq "random.seed(seed)" -and
        $_.source -eq "builtin:random"
    } | Select-Object -First 1
    $randomChoice = $randomSymbols | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "choice" -and
        $_.signature -eq "random.choice(array)" -and
        $_.source -eq "builtin:random"
    } | Select-Object -First 1
    if ($random.schema -ne "sura.package.info.v1" -or
        -not $random.passed -or
        $random.source -ne "stdlib" -or
        $random.package -ne "random" -or
        $random.path -ne "builtin:random" -or
        [int]$random.symbol_count -lt 8 -or
        -not $randomSeed -or
        -not $randomChoice) {
        Write-Output $randomJson.Output
        throw "expected builtin random info JSON metadata"
    }

    $mathJson = Run-Pkg -PkgArgs @("info", "math", "--json")
    if ($mathJson.Code -ne 0) {
        Write-Output $mathJson.Output
        throw "expected info math --json to pass"
    }
    $math = $mathJson.Output | ConvertFrom-Json
    $mathPow = @($math.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "pow" -and
        $_.signature -eq "math.pow(base, exponent)" -and
        $_.source -eq "builtin:math"
    } | Select-Object -First 1
    if ($math.schema -ne "sura.package.info.v1" -or
        -not $math.passed -or
        $math.source -ne "stdlib" -or
        $math.package -ne "math" -or
        $math.path -ne "builtin:math" -or
        [int]$math.symbol_count -lt 12 -or
        -not $mathPow) {
        Write-Output $mathJson.Output
        throw "expected builtin math info JSON metadata"
    }

    $stringJson = Run-Pkg -PkgArgs @("info", "string", "--json")
    if ($stringJson.Code -ne 0) {
        Write-Output $stringJson.Output
        throw "expected info string --json to pass"
    }
    $string = $stringJson.Output | ConvertFrom-Json
    $stringChunks = @($string.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "chunks" -and
        $_.signature -eq "string.chunks(text, [max_chars], [overlap])" -and
        $_.source -eq "builtin:string"
    } | Select-Object -First 1
    if ($string.schema -ne "sura.package.info.v1" -or
        -not $string.passed -or
        $string.source -ne "stdlib" -or
        $string.package -ne "string" -or
        $string.path -ne "builtin:string" -or
        [int]$string.symbol_count -lt 12 -or
        -not $stringChunks) {
        Write-Output $stringJson.Output
        throw "expected builtin string info JSON metadata"
    }

    $osJson = Run-Pkg -PkgArgs @("info", "os", "--json")
    if ($osJson.Code -ne 0) {
        Write-Output $osJson.Output
        throw "expected info os --json to pass"
    }
    $os = $osJson.Output | ConvertFrom-Json
    $osEnvLoad = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "env_load" -and
        $_.signature -eq "os.env_load(path, [override])" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osWait = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "wait" -and
        $_.signature -eq "os.wait(milliseconds)" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osTempDir = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "temp_dir" -and
        $_.signature -eq "os.temp_dir()" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osName = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "name" -and
        $_.signature -eq "os.name()" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osWhich = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "which" -and
        $_.signature -eq "os.which(command)" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osCmdExists = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "cmd_exists" -and
        $_.signature -eq "os.cmd_exists(command)" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osCmdQuote = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "cmd_quote" -and
        $_.signature -eq "os.cmd_quote(text)" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osCmdJoin = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "cmd_join" -and
        $_.signature -eq "os.cmd_join(args)" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osRun = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "run" -and
        $_.signature -eq "os.run(command)" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    $osRunChecked = @($os.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "run_checked" -and
        $_.signature -eq "os.run_checked(command)" -and
        $_.source -eq "builtin:os"
    } | Select-Object -First 1
    if ($os.schema -ne "sura.package.info.v1" -or
        -not $os.passed -or
        $os.source -ne "stdlib" -or
        $os.package -ne "os" -or
        $os.path -ne "builtin:os" -or
        [int]$os.symbol_count -lt 20 -or
        -not $osEnvLoad -or
        -not $osWait -or
        -not $osTempDir -or
        -not $osName -or
        -not $osWhich -or
        -not $osCmdExists -or
        -not $osCmdQuote -or
        -not $osCmdJoin -or
        -not $osRun -or
        -not $osRunChecked) {
        Write-Output $osJson.Output
        throw "expected builtin os info JSON metadata"
    }

    $arrayJson = Run-Pkg -PkgArgs @("info", "array", "--json")
    if ($arrayJson.Code -ne 0) {
        Write-Output $arrayJson.Output
        throw "expected info array --json to pass"
    }
    $array = $arrayJson.Output | ConvertFrom-Json
    $arraySlice = @($array.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "slice" -and
        $_.signature -eq "array.slice(array, start, [end])" -and
        $_.source -eq "builtin:array"
    } | Select-Object -First 1
    if ($array.schema -ne "sura.package.info.v1" -or
        -not $array.passed -or
        $array.source -ne "stdlib" -or
        $array.package -ne "array" -or
        $array.path -ne "builtin:array" -or
        [int]$array.symbol_count -lt 10 -or
        -not $arraySlice) {
        Write-Output $arrayJson.Output
        throw "expected builtin array info JSON metadata"
    }

    $pathJson = Run-Pkg -PkgArgs @("info", "path", "--json")
    if ($pathJson.Code -ne 0) {
        Write-Output $pathJson.Output
        throw "expected info path --json to pass"
    }
    $path = $pathJson.Output | ConvertFrom-Json
    $pathJoin = @($path.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "join" -and
        $_.signature -eq "path.join(part, ...)" -and
        $_.source -eq "builtin:path"
    } | Select-Object -First 1
    if ($path.schema -ne "sura.package.info.v1" -or
        -not $path.passed -or
        $path.source -ne "stdlib" -or
        $path.package -ne "path" -or
        $path.path -ne "builtin:path" -or
        [int]$path.symbol_count -lt 8 -or
        -not $pathJoin) {
        Write-Output $pathJson.Output
        throw "expected builtin path info JSON metadata"
    }

    $pythonJson = Run-Pkg -PkgArgs @("info", "python", "--json")
    if ($pythonJson.Code -ne 0) {
        Write-Output $pythonJson.Output
        throw "expected info python --json to pass"
    }
    $python = $pythonJson.Output | ConvertFrom-Json
    $pythonCallJson = @($python.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "call_json" -and
        $_.signature -eq "python.call_json(module, function, [args], [kwargs])" -and
        $_.source -eq "builtin:python"
    } | Select-Object -First 1
    if ($python.schema -ne "sura.package.info.v1" -or
        -not $python.passed -or
        $python.source -ne "stdlib" -or
        $python.package -ne "python" -or
        $python.path -ne "builtin:python" -or
        [int]$python.symbol_count -lt 5 -or
        -not $pythonCallJson) {
        Write-Output $pythonJson.Output
        throw "expected builtin python info JSON metadata"
    }

    $ffiJson = Run-Pkg -PkgArgs @("info", "ffi", "--json")
    if ($ffiJson.Code -ne 0) {
        Write-Output $ffiJson.Output
        throw "expected info ffi --json to pass"
    }
    $ffi = $ffiJson.Output | ConvertFrom-Json
    $ffiCall = @($ffi.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "call" -and
        $_.signature -eq "ffi.call(lib, symbol, signature, ...args)" -and
        $_.source -eq "builtin:ffi"
    } | Select-Object -First 1
    if ($ffi.schema -ne "sura.package.info.v1" -or
        -not $ffi.passed -or
        $ffi.source -ne "stdlib" -or
        $ffi.package -ne "ffi" -or
        $ffi.path -ne "builtin:ffi" -or
        [int]$ffi.symbol_count -lt 2 -or
        -not $ffiCall) {
        Write-Output $ffiJson.Output
        throw "expected builtin ffi info JSON metadata"
    }

    $pluginJson = Run-Pkg -PkgArgs @("info", "plugin", "--json")
    if ($pluginJson.Code -ne 0) {
        Write-Output $pluginJson.Output
        throw "expected info plugin --json to pass"
    }
    $plugin = $pluginJson.Output | ConvertFrom-Json
    $pluginCall = @($plugin.symbols) | Where-Object {
        $_.kind -eq "function" -and
        $_.name -eq "call" -and
        $_.signature -eq "plugin.call(plugin, export, ...args)" -and
        $_.source -eq "builtin:plugin"
    } | Select-Object -First 1
    if ($plugin.schema -ne "sura.package.info.v1" -or
        -not $plugin.passed -or
        $plugin.source -ne "stdlib" -or
        $plugin.package -ne "plugin" -or
        $plugin.path -ne "builtin:plugin" -or
        [int]$plugin.symbol_count -lt 5 -or
        -not $pluginCall) {
        Write-Output $pluginJson.Output
        throw "expected builtin plugin info JSON metadata"
    }

    "pkg_info_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
