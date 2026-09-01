# Sura Language Support for VS Code

Official VS Code support for Sura source files.

![Sura Language official preview](https://suralang.site/store-super-hero.png)

## Features

- Syntax highlighting for `.sura`, including the native `nn`, `ai`, and `autograd` APIs
- Rich completions, hover documentation, signatures, and snippets for native neural networks and automatic differentiation
- Run and Debug CodeLens at the top of Sura files
- Editor title Run menu plus play/debug buttons
- Status bar Run Sura and Debug actions while a `.sura` file is active
- Command palette and editor context menu commands
- Identifier completions from the first typed character, including current-file and workspace symbols
- Engine-backed language server diagnostics, definition/references, rename, formatting, semantic tokens, and code actions
- Focused `console.` completions with browser-style aliases such as `timeLog`, `countReset`, `groupCollapsed`, `profileEnd`, and `readLine`
- Sura-only completion hygiene: inline prose suggestions, Copilot inline completions, Copilot next-edit suggestions, word-based suggestions, and generic text entries are disabled by default
- Debug Adapter Protocol support for breakpoints, stepping, scopes, and watches
- `Sura: Create Starter Project` for a runnable package with tests and VS Code settings
- `Sura: Run Package` and `Sura: Test Package` commands plus a first-use walkthrough

## Create a Starter Project

Run `Sura: Create Starter Project` from the command palette, choose a parent folder, and enter a project name. The extension runs the same starter flow as the command line:

```powershell
surapkg new hello_sura
cd hello_sura
surapkg run
surapkg test
```

The project includes source, a passing test, recommended extension settings, a debug configuration, and Run/Test tasks.

## Native AI and Autograd

The extension understands both direct built-ins such as `autograd_matmul(...)` and module calls such as `autograd.matmul(...)`. The `ai` module is an editor and runtime alias for `nn`.

```sura
use autograd

inputs is autograd.tensor([[0, 0], [0, 1], [1, 0], [1, 1]])
targets is autograd.tensor([[0], [1], [1], [0]])
weights is autograd.parameter([[0.1], [-0.2]])
bias is autograd.parameter([0])
parameters is [weights, bias]

for step in 1 to 200 do
  predictions is autograd.sigmoid(autograd.linear(inputs, weights, bias))
  loss is autograd.bce(predictions, targets)
  autograd.zero_grad(parameters)
  autograd.backward(loss)
  autograd.adam(parameters, 0.03)
end
```

Typing `autograd.` offers every tensor constructor, differentiable operation, loss, backward function, optimizer, and gradient utility. `nn.` and `ai.` offer the complete native MLP training, inference, preprocessing, evaluation, and persistence API.

## Run a File

Install Sura so the `sura` command is on PATH, then open a `.sura` file. Run the active file from:

- `Run Sura File` CodeLens
- Editor title Run menu
- Editor title play button
- Status bar: `Run Sura`
- Command palette: `Sura: Run File`
- Editor context menu: `Sura: Run File`
- Keyboard shortcut: `Ctrl+Shift+R`

Debug the active file from `Debug Sura File` CodeLens, the editor title debug button, the command palette, or a normal VS Code `launch.json` configuration.

`Sura: Run File (JIT)` enables the native JIT. For proven strict counted loops
the 1.11.0 engine can remove repeated VM call and record-materialization
overhead; programs that do not satisfy the runtime proof use the normal JIT
fallback automatically.

## Completion

Completion starts from the first identifier character. Typing `i` offers `if`, `import`, `in`, `is`, `input`, and matching current-file or workspace symbols such as `init`, `index`, or `item`. In assignment context, `a ` or `a i` preselects the `is` snippet.

After `console.`, completion switches to the console API only, including `log`, `warn`, `raw`, `flush`, `json`, `inspect`, `timeLog`, `timeEnd`, `countReset`, `groupCollapsed`, `profileEnd`, `readLine`, `style`, `color`, `stripAnsi`, `setColor`, `resetColor`, `isTTY`, `width`, `height`, and `size`, with hover and signature help.

For `.sura` files, the extension disables inline prose suggestions, Copilot inline completions, Copilot next-edit suggestions, word-based suggestions, and generic text completion entries by default. Sura completions therefore stay focused on keywords, built-ins, modules, APIs, and project symbols.

## Official Editor Experience

Normal users should install Sura Language once so `sura` is on PATH. Source-checkout developers can point `sura.enginePath` at the canonical `SuraLanguage.exe`. The extension starts that executable with `--lsp`; if it is unavailable or cannot start, the extension reports the failure and keeps its basic built-in completion, hover, and signature help. Diagnostics use English by default for searchability; set `sura.language` to `ko` for Korean output. `sura.languageServer.enabled` controls the engine-backed server, `Sura: Restart Language Server` restarts it after an engine/configuration change, and `sura.showRunCodeLens` shows or hides the top-of-file actions.

If VS Code shows the owner as `user` or completions do not appear after typing `i`, remove the old local `user.sura` extension and install the official source copy:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ..\tools\sura_install_vscode_extension.ps1
```

Then run `Developer: Reload Window` in VS Code.

## 한국어 요약

Sura 공식 VS Code 확장은 `.sura` 파일의 문법 강조, 자동완성, 함수 설명, 시그니처 도움말, 실행과 디버깅을 제공합니다.

자동완성은 첫 글자부터 동작합니다. 예를 들어 `i`를 입력하면 `if`, `import`, `in`, `is`, `input`과 현재 파일 또는 워크스페이스의 심볼을 함께 보여줍니다. `a ` 또는 `a i`처럼 변수 할당을 쓰는 문맥에서는 `is`를 가장 먼저 제안합니다.

`autograd.`를 입력하면 네이티브 텐서 생성, 연산, 손실 함수, 역전파, SGD와 Adam, 그래디언트 도구가 모두 표시됩니다. `nn.`과 별칭인 `ai.`에서는 네이티브 신경망의 학습, 예측, 전처리, 평가, 저장과 불러오기 API를 사용할 수 있습니다.

`console.` 뒤에는 콘솔 전용 API만 표시합니다. `.sura` 파일에서는 긴 영어 설명문, Copilot inline completion, Copilot next-edit suggestion, 단어 기반 추천, 일반 텍스트 후보를 기본적으로 꺼서 Sura 키워드, 내장 함수, 모듈, 프로젝트 심볼에 집중합니다.

일반 사용자는 Sura Language 설치 프로그램으로 `sura` 명령을 PATH에 추가하면 됩니다. 확장은 이 실행 파일을 `--lsp`로 시작해 진단, 정의/참조 찾기, 이름 변경, 포맷, 시맨틱 토큰, 코드 액션을 제공합니다. 실행 파일을 찾지 못하거나 서버 시작에 실패하면 오류를 알리고 기본 자동완성·hover·시그니처 도움말로 돌아갑니다. 소스 체크아웃에서 개발할 때만 `sura.enginePath`를 표준 실행 파일인 `SuraLanguage.exe` 경로로 지정하세요. `sura.languageServer.enabled`로 서버 사용 여부를 정하고 엔진 설정을 바꾼 뒤에는 `Sura: Restart Language Server`를 실행할 수 있습니다.

## Source Checkout

For a source checkout without the installer, set:

```json
{
  "sura.enginePath": "C:\\path\\to\\SuraLanguage.exe"
}
```

## Commands

- `Sura: Run File`
- `Sura: Debug File`
- `Sura: Run File (JIT)`
- `Sura: Profile File`
- `Sura: Trace File`
- `Sura: Open REPL`
- `Sura: Restart Language Server`
- `Sura: Create Starter Project`
- `Sura: Run Package`
- `Sura: Test Package`
