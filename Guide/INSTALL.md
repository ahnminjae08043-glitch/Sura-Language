# Installing Sura Language

Sura should feel like Python for normal users: install once, open a new terminal, run `sura app.sura`.

## Windows User Install

Build or download the Sura Language Windows single-file installer, then run:

```text
SuraLanguageSetup-1.11.1.exe
```

`SuraLanguageSetup-1.11.1.exe` is the only installer file normal users need to download or click. It extracts the internal setup kit to a temporary folder, opens the Sura Language setup window, and cleans up when setup exits. The setup window shows the Sura logo from `assets\sura-logo.png` packaged as a 1:1 square image and displayed larger in the right header area, plus the required disk space, installation actions, install path, and user PATH option.

Before upgrading, close running Sura REPLs, Sura terminals, and VS Code windows using the Sura language server. If an installed executable is locked, the installer leaves it unchanged and reports the process when it can identify it. Detailed logs are written under `%LOCALAPPDATA%\Sura\Logs`.

The folder installer at `dist\SuraLanguage-windows-x64\SuraSetup.exe` is for release verification and developer packaging. The `payload` and `support` folders are installer internals.

For command-line install or uninstall, use the support script directly:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\support\SuraSetup.ps1
```

The installer copies the runtime to:

```text
%LOCALAPPDATA%\Programs\Sura\bin
```

It creates these commands and moves that `bin` directory to the front of the user PATH:

```text
sura
surapkg
```

After installing, open a new terminal:

```powershell
sura --help
sura app.sura
sura --repl
surapkg new my_app
cd my_app
surapkg run
surapkg test
```

If `sura app.sura` prints another tool's help text, another `sura` launcher is earlier in PATH. Run `surapkg doctor --json doctor.json` to see the first resolved `sura` command and any installed Sura command found later. Re-run the installer or move `%LOCALAPPDATA%\Programs\Sura\bin` earlier in the user PATH.

## Diagnostic Language

Sura defaults diagnostics and help to English so errors are easy to search globally. Korean is available when you want it:

```powershell
sura --lang ko app.sura
$env:SURA_LANG="ko"; sura app.sura
```

## VS Code

The Sura VS Code extension defaults `sura.enginePath` to the installed `sura` command. Open a `.sura` file and press the play button in the editor title, use the editor title Run menu, or run `Sura: Run File` from the command palette. Completion starts from the first identifier character, so typing `i` offers Sura symbols such as `if`, `import`, `in`, `is`, and `input`, plus matching current-file and workspace symbols such as `init`, `index`, and `item`. Sura files disable inline prose suggestions, Copilot inline completions, Copilot next-edit suggestions, word-based suggestions, and raw word entries by default so long English paragraph suggestions do not appear in normal Sura completion. Set `sura.language` to `ko` for Korean diagnostics, `en` for English, or `auto` to use the engine default.

For a source checkout, run `tools\sura_install_vscode_extension.ps1` to install the official `sura-team.sura-language` extension into VS Code. The script moves old local `user.sura` extensions to a backup folder because those legacy copies can own `.sura` files without providing completions.

## What Users Need

For normal `.sura` scripts, users only need Sura:

- `sura` to run scripts and the REPL.
- `surapkg` only when they want packages.

They do not need Python, Node.js, CMake, or a C++ compiler unless they use optional developer features.

## Optional Developer Tools

These are optional:

- Python: `use python`, Python bridge smoke tests, Python comparison benchmarks.
- Node.js: JavaScript target, VS Code/debug-adapter smoke tests.
- C++ compiler or CMake: native embedding, native plugins, bindgen, local source builds.
- WASM tools: external WebAssembly validation or runtime experiments.
- FFmpeg: optional local video decoding for `use media` and `media.ascii_frames`; set `SURA_FFMPEG` when it is not on PATH.

## Build The Installer Kit

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_make_installer.ps1
```

This writes:

```text
dist\SuraLanguageSetup-1.11.1.exe
dist\SuraLanguage-windows-x64\SuraSetup.exe
dist\SuraLanguage-windows-x64\support\SuraSetup.ps1
dist\SuraLanguage-windows-x64\support\SuraLogo.png
dist\SuraLanguage-windows-x64\payload\surapkg.exe
dist\SuraLanguage-1.11.1-windows-x64.zip
```

## Uninstall

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\support\SuraSetup.ps1 -Uninstall
```
