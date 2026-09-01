param(
    [string]$RepoRoot = "."
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$packagePath = Join-Path $root "sura-vscode/package.json"
if (-not (Test-Path -LiteralPath $packagePath)) {
    throw "Sura VS Code package.json not found: $packagePath"
}

$raw = [System.IO.File]::ReadAllText($packagePath, [System.Text.Encoding]::UTF8)
$pkg = $raw | ConvertFrom-Json

if ($pkg.name -ne "sura-language" -or $pkg.publisher -ne "sura-team" -or $pkg.publisher -eq "user") {
    $pkg | ConvertTo-Json -Depth 5
    throw "VS Code extension should use the official sura-team.sura-language identity, not the legacy user.sura owner"
}

$iconPath = Join-Path $root ([string]("sura-vscode/" + $pkg.icon))
if ($pkg.icon -ne "assets/icon.png" -or -not (Test-Path -LiteralPath $iconPath)) {
    $pkg | ConvertTo-Json -Depth 5
    throw "VS Code extension should use the Sura language logo as its package icon"
}
Add-Type -AssemblyName System.Drawing
$iconImage = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $iconPath).Path)
try {
    if ($iconImage.Width -ne 128 -or $iconImage.Height -ne 128) {
        throw "VS Code extension icon should be a 128x128 PNG built from the Sura language logo"
    }
} finally {
    $iconImage.Dispose()
}

$commands = @($pkg.contributes.commands)
$runCommand = $commands | Where-Object { $_.command -eq "sura.runFile" } | Select-Object -First 1
if ($null -eq $runCommand -or
    $runCommand.title -ne "Sura: Run File" -or
    $runCommand.icon -ne '$(play)') {
    $commands | ConvertTo-Json -Depth 5
    throw "VS Code extension should expose Sura: Run File with a play icon"
}
$debugCommand = $commands | Where-Object { $_.command -eq "sura.debugFile" } | Select-Object -First 1
if ($null -eq $debugCommand -or
    $debugCommand.title -ne "Sura: Debug File" -or
    $debugCommand.icon -ne '$(debug-alt)') {
    $commands | ConvertTo-Json -Depth 5
    throw "VS Code extension should expose Sura: Debug File with a debug icon"
}

$editorTitle = @($pkg.contributes.menus.'editor/title')
if (-not ($editorTitle | Where-Object {
    $_.command -eq "sura.runFile" -and
    $_.when -eq "resourceLangId == sura" -and
    $_.group -match "^navigation"
}) -or
    -not ($editorTitle | Where-Object {
    $_.command -eq "sura.debugFile" -and
    $_.when -eq "resourceLangId == sura" -and
    $_.group -match "^navigation"
})) {
    $editorTitle | ConvertTo-Json -Depth 5
    throw "VS Code extension should show Run and Debug Sura File in the editor title for .sura files"
}

$editorTitleRun = @($pkg.contributes.menus.'editor/title/run')
if (-not ($editorTitleRun | Where-Object {
    $_.command -eq "sura.runFile" -and
    $_.when -eq "resourceLangId == sura"
}) -or
    -not ($editorTitleRun | Where-Object {
    $_.command -eq "sura.debugFile" -and
    $_.when -eq "resourceLangId == sura"
})) {
    $editorTitleRun | ConvertTo-Json -Depth 5
    throw "VS Code extension should also expose Run and Debug Sura File in the editor title Run menu"
}

$editorContext = @($pkg.contributes.menus.'editor/context')
if (-not ($editorContext | Where-Object { $_.command -eq "sura.runFile" -and $_.when -eq "resourceLangId == sura" }) -or
    -not ($editorContext | Where-Object { $_.command -eq "sura.runJIT" -and $_.when -eq "resourceLangId == sura" }) -or
    -not ($editorContext | Where-Object { $_.command -eq "sura.debugFile" -and $_.when -eq "resourceLangId == sura" })) {
    $editorContext | ConvertTo-Json -Depth 5
    throw "VS Code extension should expose run, JIT, and debug commands in the Sura editor context menu"
}

$engineSetting = $pkg.contributes.configuration.properties.'sura.enginePath'
if ($engineSetting.default -ne "sura" -or
    $engineSetting.description -notmatch "Windows installer" -or
    $engineSetting.description -notmatch "SuraLanguage\.exe") {
    $engineSetting | ConvertTo-Json -Depth 5
    throw "VS Code extension should default to the installed sura command while documenting source checkout override"
}

$languageSetting = $pkg.contributes.configuration.properties.'sura.language'
if ($null -eq $languageSetting -or
    $languageSetting.default -ne "auto" -or
    @($languageSetting.enum) -notcontains "en" -or
    @($languageSetting.enum) -notcontains "ko" -or
    $languageSetting.description -notmatch "--lang") {
    $languageSetting | ConvertTo-Json -Depth 5
    throw "VS Code extension should expose a language setting that passes --lang en|ko to run commands"
}

$suraDefaults = $pkg.contributes.configurationDefaults."[sura]"
if ($null -eq $suraDefaults -or
    $suraDefaults."editor.quickSuggestions".other -ne $true -or
    $suraDefaults."editor.quickSuggestions".comments -ne $false -or
    $suraDefaults."editor.quickSuggestions".strings -ne $false -or
    $suraDefaults."editor.suggestOnTriggerCharacters" -ne $true -or
    $suraDefaults."editor.inlineSuggest.enabled" -ne $false -or
    [string]$suraDefaults."editor.wordBasedSuggestions" -ne "off" -or
    $suraDefaults."editor.suggest.showWords" -ne $false -or
    $suraDefaults."editor.suggest.showText" -ne $false -or
    $suraDefaults."github.copilot.completions.enabled" -ne $false -or
    $suraDefaults."github.copilot.nextEditSuggestions.enabled" -ne $false -or
    $pkg.contributes.configurationDefaults."github.copilot.enable".sura -ne $false) {
    $pkg.contributes.configurationDefaults | ConvertTo-Json -Depth 5
    throw "VS Code extension should disable prose, text, Copilot inline, Copilot next-edit, and word-based suggestions for .sura files by default"
}

$workspaceSettingsPath = Join-Path $root ".vscode/settings.json"
if (-not (Test-Path $workspaceSettingsPath)) {
    throw "Repository should include VS Code workspace settings for local .sura completion hygiene"
}
$workspaceSettings = Get-Content $workspaceSettingsPath -Raw | ConvertFrom-Json
$workspaceSuraDefaults = $workspaceSettings."[sura]"
$workspaceAssociations = $workspaceSettings."files.associations"
if ($null -eq $workspaceSuraDefaults -or
    $workspaceAssociations."*.sura" -ne "sura" -or
    $workspaceSuraDefaults."editor.quickSuggestions".other -ne $true -or
    $workspaceSuraDefaults."editor.quickSuggestions".comments -ne $false -or
    $workspaceSuraDefaults."editor.quickSuggestions".strings -ne $false -or
    $workspaceSuraDefaults."editor.suggestOnTriggerCharacters" -ne $true -or
    $workspaceSuraDefaults."editor.inlineSuggest.enabled" -ne $false -or
    [string]$workspaceSuraDefaults."editor.wordBasedSuggestions" -ne "off" -or
    $workspaceSuraDefaults."editor.suggest.showWords" -ne $false -or
    $workspaceSuraDefaults."editor.suggest.showText" -ne $false -or
    $workspaceSuraDefaults."github.copilot.completions.enabled" -ne $false -or
    $workspaceSuraDefaults."github.copilot.nextEditSuggestions.enabled" -ne $false -or
    $workspaceSettings."github.copilot.enable".sura -ne $false) {
    $workspaceSettings | ConvertTo-Json -Depth 5
    throw "Workspace settings should keep long prose suggestions out of local .sura editing"
}

$workspacePath = Join-Path $root "Sura.code-workspace"
if (-not (Test-Path $workspacePath)) {
    throw "Repository should include Sura.code-workspace"
}
$workspace = Get-Content $workspacePath -Raw | ConvertFrom-Json
$workspaceFolders = @($workspace.folders | ForEach-Object { [string]$_.path })
if ($workspaceFolders -notcontains "." -or
    $workspaceFolders -notcontains "sura-vscode" -or
    ($workspaceFolders | Where-Object { $_ -match "\.vscode[\\/]+extensions" })) {
    $workspace | ConvertTo-Json -Depth 5
    throw "Sura.code-workspace should open the editable sura-vscode source folder, not an installed VS Code extension copy"
}

$vscodeIgnorePath = Join-Path $root "sura-vscode/.vscodeignore"
$vscodeIgnore = [System.IO.File]::ReadAllText($vscodeIgnorePath, [System.Text.Encoding]::UTF8)
if ($vscodeIgnore -match "(?m)^\*\*$" -or
    $vscodeIgnore -match "(?m)^!node_modules/" -or
    $vscodeIgnore -notmatch "node_modules/\*\*" -or
    $vscodeIgnore -notmatch "\.vscode/\*\*" -or
    $vscodeIgnore -notmatch "\.git/\*\*") {
    throw "VS Code extension packaging should not ignore every source file or re-include node_modules"
}

$installScriptPath = Join-Path $root "tools/sura_install_vscode_extension.ps1"
if (-not (Test-Path -LiteralPath $installScriptPath)) {
    throw "Repository should include a source-checkout VS Code extension installer"
}
$installScript = [System.IO.File]::ReadAllText($installScriptPath, [System.Text.Encoding]::UTF8)
if ($installScript -notmatch "sura-team" -or
    $installScript -notmatch "sura-language" -or
    $installScript -notmatch "\`$officialId-\*" -or
    $installScript -notmatch "user\.sura-\*" -or
    $installScript -notmatch "Move-Item" -or
    $installScript -notmatch "Reload VS Code" -or
    $installScript -notmatch "Refusing to install") {
    throw "VS Code installer script should install sura-team.sura-language and move legacy user.sura extensions out of VS Code"
}

$codeLensSetting = $pkg.contributes.configuration.properties.'sura.showRunCodeLens'
if ($null -eq $codeLensSetting -or
    $codeLensSetting.default -ne $true -or
    $codeLensSetting.description -notmatch "CodeLens") {
    $codeLensSetting | ConvertTo-Json -Depth 5
    throw "VS Code extension should expose a CodeLens setting for Python-like run/debug actions"
}

$extensionPath = Join-Path $root "sura-vscode/extension.ts"
$extensionOutPath = Join-Path $root "sura-vscode/out/extension.js"
$extensionSource = [System.IO.File]::ReadAllText($extensionPath, [System.Text.Encoding]::UTF8)
$extensionOut = [System.IO.File]::ReadAllText($extensionOutPath, [System.Text.Encoding]::UTF8)
if ($extensionSource -notmatch "configuredLanguageArgs" -or
    $extensionSource -notmatch "'--lang'" -or
    $extensionOut -notmatch "configuredLanguageArgs" -or
    $extensionOut -notmatch "--lang") {
    throw "VS Code run and REPL commands should pass configured --lang arguments"
}
if ($extensionSource -notmatch "installedWindowsEnginePath" -or
    $extensionSource -notmatch "LOCALAPPDATA" -or
    $extensionSource -notmatch "Programs', 'Sura', 'bin" -or
    $extensionSource -notmatch "localEnginePath\(file\) \|\| installedWindowsEnginePath\(\) \|\| configured" -or
    $extensionOut -notmatch "installedWindowsEnginePath" -or
    $extensionOut -notmatch "LOCALAPPDATA" -or
    $extensionOut -notmatch "Programs[`"'], [`"']Sura[`"'], [`"']bin" -or
    $extensionOut -notmatch "localEnginePath\(file\) \|\| installedWindowsEnginePath\(\) \|\| configured") {
    throw "VS Code run and debug commands should prefer the installed Sura engine before falling back to a PATH command"
}
if ($extensionSource -notmatch "terminalCommand" -or
    $extensionSource -notmatch "quotePowerShellArg" -or
    $extensionSource -notmatch "terminalShellKind" -or
    $extensionSource -notmatch 'return `& ' -or
    $extensionSource -notmatch "terminal\.integrated" -or
    $extensionOut -notmatch "terminalCommand" -or
    $extensionOut -notmatch "quotePowerShellArg" -or
    $extensionOut -notmatch "terminalShellKind" -or
    $extensionOut -notmatch 'return `& ' -or
    $extensionOut -notmatch "terminal\.integrated") {
    throw "VS Code run and REPL commands should quote terminal commands correctly, including PowerShell call-operator execution for quoted Sura paths"
}
if ($extensionSource -notmatch "SuraRunCodeLensProvider" -or
    $extensionSource -notmatch "registerCodeLensProvider" -or
    $extensionSource -notmatch "sura.debugFile" -or
    $extensionSource -notmatch "Debug Sura File" -or
    $extensionOut -notmatch "SuraRunCodeLensProvider" -or
    $extensionOut -notmatch "registerCodeLensProvider" -or
    $extensionOut -notmatch "sura.debugFile" -or
    $extensionOut -notmatch "Debug Sura File") {
    throw "VS Code extension should provide Python-like Run/Debug CodeLens actions for Sura files"
}
if ($extensionSource -notmatch "COMPLETION_TRIGGER_CHARS" -or
    $extensionSource -notmatch "currentWordPrefix" -or
    $extensionSource -notmatch "assignmentKeywordCompletion" -or
    $extensionSource -notmatch "currentModuleMemberContext" -or
    $extensionSource -notmatch "CONSOLE_MEMBERS" -or
    $extensionSource -notmatch "readLine" -or
    $extensionSource -notmatch "collectProjectSymbols" -or
    $extensionSource -notmatch "extractSuraSymbols" -or
    $extensionSource -notmatch "matchesPrefix" -or
    $extensionOut -notmatch "COMPLETION_TRIGGER_CHARS" -or
    $extensionOut -notmatch "currentWordPrefix" -or
    $extensionOut -notmatch "assignmentKeywordCompletion" -or
    $extensionOut -notmatch "currentModuleMemberContext" -or
    $extensionOut -notmatch "CONSOLE_MEMBERS" -or
    $extensionOut -notmatch "readLine" -or
    $extensionOut -notmatch "collectProjectSymbols" -or
    $extensionOut -notmatch "extractSuraSymbols" -or
    $extensionOut -notmatch "matchesPrefix") {
    throw "VS Code extension should trigger and filter completions from the first typed identifier character, including assignment-keyword snippets, project symbols, and console member completions"
}
if ($extensionSource -notmatch "nn_train" -or
    $extensionSource -notmatch "'nn'" -or
    $extensionSource -notmatch "'ai'" -or
    $extensionOut -notmatch "nn_train" -or
    $extensionOut -notmatch '"nn"' -or
    $extensionOut -notmatch '"ai"') {
    throw "VS Code extension should expose native nn/ai modules and neural-network builtin completions"
}

$readmePath = Join-Path $root "sura-vscode/README.md"
$ecosystemPath = Join-Path $root "Guide/ECOSYSTEM.md"
$referencePath = Join-Path $root "reference.html"
$readme = [System.IO.File]::ReadAllText($readmePath, [System.Text.Encoding]::UTF8)
$ecosystem = [System.IO.File]::ReadAllText($ecosystemPath, [System.Text.Encoding]::UTF8)
$reference = [System.IO.File]::ReadAllText($referencePath, [System.Text.Encoding]::UTF8)
$koreanCompletion = ([string][char]0xC790) + ([string][char]0xB3D9) + ([string][char]0xC644) + ([string][char]0xC131)
if ($readme -notmatch "Official VS Code support" -or
    $readme -notmatch "Editor title Run menu" -or
    $readme -notmatch 'Typing `i` offers' -or
    $readme -notmatch "assignment context" -or
    $readme -notmatch "console\." -or
    $readme -notmatch "timeLog" -or
    $readme -notmatch "readLine" -or
    $readme -notmatch "workspace symbols" -or
    $readme -notmatch "word-based suggestions" -or
    $readme -notmatch "inline prose suggestions" -or
    $readme -notmatch "Copilot inline completions" -or
    $readme -notmatch "Copilot next-edit suggestions" -or
    $readme -notmatch "sura\.showRunCodeLens" -or
    $readme -notmatch "user\.sura" -or
    -not $readme.Contains($koreanCompletion)) {
    throw "VS Code README should document the official run/debug/completion workflow in English and Korean"
}
$badCodepoints = @(0xFFFD, 0xF9E3, 0x5A9B, 0xB348, 0xAFB9, 0x745C, 0x6FE1)
foreach ($codepoint in $badCodepoints) {
    if ($readme.Contains([string][char]$codepoint)) {
        throw "VS Code README contains mojibake or replacement characters"
    }
    if ($reference.Contains([string][char]$codepoint)) {
        throw "Sura reference contains mojibake or replacement characters"
    }
}
if ($ecosystem -notmatch "Editor title Run menu" -or
    $ecosystem -notmatch "first typed character" -or
    $ecosystem -notmatch "console\." -or
    $ecosystem -notmatch "timeLog" -or
    $ecosystem -notmatch "workspace symbols" -or
    $ecosystem -notmatch "Copilot inline completions" -or
    $ecosystem -notmatch "Copilot next-edit suggestions" -or
    $ecosystem -notmatch 'installed `sura` command' -or
    $ecosystem -notmatch "sura\.showRunCodeLens") {
    throw "Guide/ECOSYSTEM.md should document current VS Code run/debug/completion behavior"
}
# The release version lives in version.json; hardcoding it here meant the
# check silently drifted to an old release and then failed on every run.
$referenceVersion = ([System.IO.File]::ReadAllText((Join-Path $root "version.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json).version
if ($reference -notmatch ("Sura Language " + [regex]::Escape($referenceVersion)) -or
    $reference -notmatch "VS Code extension" -or
    $reference -notmatch "--lsp" -or
    $reference -notmatch "semantic tokens" -or
    $reference -notmatch "console\." -or
    $reference -notmatch "readLine" -or
    $reference -notmatch "reference-contract" -or
    $reference -notmatch "<code>to_str</code>" -or
    $reference -notmatch "gc-stats" -or
    $reference -notmatch "status-editor") {
    throw "reference.html should present the official VS Code workflow and project-symbol completion status"
}

"sura_vscode_run_button_smoke: PASS"
