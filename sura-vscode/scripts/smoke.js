'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const repo = path.resolve(root, '..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');
const requireText = (text, needle, label) => {
  if (!text.includes(needle)) throw new Error(`${label} is missing ${needle}`);
};

const pkg = JSON.parse(read('package.json'));
const releaseVersion = JSON.parse(fs.readFileSync(path.join(repo, 'version.json'), 'utf8')).version;
if (pkg.version !== releaseVersion) throw new Error(`expected extension version ${releaseVersion}, got ${pkg.version}`);
if (pkg.publisher !== 'sura-team' || pkg.name !== 'sura-language') {
  throw new Error('official extension identity changed unexpectedly');
}
if (pkg.license !== 'UNLICENSED') throw new Error('package license metadata must reflect the current source status');

const source = read('extension.ts');
const output = read(path.join('out', 'extension.js'));
const debugSource = read('debugAdapter.ts');
const debugOutput = read(path.join('out', 'debugAdapter.js'));
const grammar = read(path.join('syntaxes', 'sura.tmLanguage.json'));
JSON.parse(grammar);

for (const command of ['sura.createProject', 'sura.runPackage', 'sura.testPackage']) {
  if (!pkg.contributes.commands.some((item) => item.command === command)) {
    throw new Error(`missing starter command contribution: ${command}`);
  }
  requireText(source, `registerCommand('${command}'`, 'extension starter commands');
}
if (!pkg.contributes.walkthroughs?.some((item) => item.id === 'sura.getStarted')) {
  throw new Error('missing Sura getting-started walkthrough');
}
requireText(source, "packageManagerPath", 'starter package manager resolution');
requireText(source, "'new', projectName.trim()", 'starter project creation');

const extensionBuiltinNames = (prefix) => [...new Set(
  [...source.matchAll(new RegExp(`^\\s*(${prefix}[a-z_]+): \\{`, 'gm'))].map((match) => match[1])
)].sort();
const requireExactBuiltins = (label, expected) => {
  const actual = extensionBuiltinNames(`${label}_`);
  const missing = expected.filter((name) => !actual.includes(name));
  const extra = actual.filter((name) => !expected.includes(name));
  if (missing.length || extra.length) {
    throw new Error(`${label} extension built-ins differ from C++ (missing: ${missing.join(', ') || 'none'}; extra: ${extra.join(', ') || 'none'})`);
  }
};

const autogradHeader = [
  fs.readFileSync(path.join(repo, 'autograd.hpp'), 'utf8'),
  fs.readFileSync(path.join(repo, 'checkpoint.hpp'), 'utf8'),
  fs.readFileSync(path.join(repo, 'safetensors.hpp'), 'utf8'),
  fs.readFileSync(path.join(repo, 'onnx_weights.hpp'), 'utf8'),
  fs.readFileSync(path.join(repo, 'distributed.hpp'), 'utf8')
].join('\n');
const autogradNames = [...new Set(
  [...autogradHeader.matchAll(/inline Value b_(autograd_[a-z_]+)\s*\(/g)].map((match) => match[1])
)].sort();
if (autogradNames.length !== 67) {
  throw new Error(`expected 67 public autograd functions, found ${autogradNames.length}`);
}
requireExactBuiltins('autograd', autogradNames);

for (const name of autogradNames) {
  requireText(source, `${name}: {`, 'extension built-ins');
  requireText(output, name, 'compiled extension');
  requireText(grammar, name, 'syntax grammar');
}

const tokenizerHeader = fs.readFileSync(path.join(repo, 'tokenizer.hpp'), 'utf8');
const tokenizerNames = [...new Set(
  [...tokenizerHeader.matchAll(/inline Value b_(tokenizer_[a-z_]+)\s*\(/g)].map((match) => match[1])
)].sort();
if (tokenizerNames.length !== 7) {
  throw new Error(`expected 7 public tokenizer functions, found ${tokenizerNames.length}`);
}
requireExactBuiltins('tokenizer', tokenizerNames);
for (const name of tokenizerNames) {
  requireText(source, `${name}: {`, 'extension built-ins');
  requireText(output, name, 'compiled extension');
  requireText(grammar, name, 'syntax grammar');
}

const datasetHeader = fs.readFileSync(path.join(repo, 'dataset.hpp'), 'utf8');
const datasetNames = [...new Set(
  [...datasetHeader.matchAll(/inline Value b_(dataset_[a-z_]+)\s*\(/g)].map((match) => match[1])
)].sort();
if (datasetNames.length !== 6) {
  throw new Error(`expected 6 public dataset functions, found ${datasetNames.length}`);
}
requireExactBuiltins('dataset', datasetNames);
for (const name of datasetNames) {
  requireText(source, `${name}: {`, 'extension built-ins');
  requireText(output, name, 'compiled extension');
  requireText(grammar, name, 'syntax grammar');
}

const nnNames = [
  'nn_mlp', 'nn_forward', 'nn_predict', 'nn_train', 'nn_classify', 'nn_evaluate',
  'nn_summary', 'nn_one_hot', 'nn_fit_standardizer', 'nn_standardize', 'nn_split',
  'nn_save', 'nn_load'
];
for (const name of nnNames) {
  requireText(source, `${name}: {`, 'extension built-ins');
  requireText(grammar, name, 'syntax grammar');
}

for (const declaration of [
  "prefixedModuleMembers('nn', 'nn_')",
  "prefixedModuleMembers('ai', 'nn_')",
  "prefixedModuleMembers('autograd', 'autograd_')",
  "prefixedModuleMembers('tokenizer', 'tokenizer_')",
  "prefixedModuleMembers('dataset', 'dataset_')"
]) {
  requireText(source, declaration, 'module IntelliSense');
}
requireText(source, 'callableContextAt', 'nested signature help');
requireText(source, "from 'vscode-languageclient/node'", 'engine-backed language client');
requireText(source, "args: [...configuredLanguageArgs(), '--lsp']", 'language server launch');
requireText(source, 'registerFallbackLanguageProviders', 'language server fallback');
requireText(source, "registerCommand('sura.restartLanguageServer'", 'language server restart command');
requireText(output, 'suraLanguageServer', 'compiled language client');
if (pkg.dependencies?.['vscode-languageclient'] !== '8.1.0') {
  throw new Error('vscode-languageclient runtime dependency is not pinned to 8.1.0');
}
if (pkg.contributes.configuration.properties['sura.languageServer.enabled']?.default !== true) {
  throw new Error('engine-backed language server must be enabled by default');
}
requireText(debugSource, "kind: 'tensor'", 'debug Tensor parser');
requireText(debugSource, "startsWith('<Tensor '", 'debug Tensor type detection');
requireText(debugOutput, '<Tensor ', 'compiled debug Tensor handling');

const readme = read('README.md');
for (const needle of ['Official VS Code support', 'autograd.', 'nn.', 'ai.', '자동완성']) {
  requireText(readme, needle, 'README');
}
if (readme.includes('\uFFFD')) throw new Error('README contains a Unicode replacement character');

const ignore = read('.vscodeignore');
for (const needle of ['node_modules/**', 'scripts/**', 'package-lock.json', 'out/**/*.map']) {
  requireText(ignore, needle, '.vscodeignore');
}

console.log(`sura-vscode smoke: PASS (${autogradNames.length} autograd APIs, ${tokenizerNames.length} tokenizer APIs, ${datasetNames.length} dataset APIs, ${nnNames.length} nn APIs)`);
