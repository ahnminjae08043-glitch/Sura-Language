'use strict';

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const extensionRoot = path.resolve(__dirname, '..');
const repoRoot = path.resolve(extensionRoot, '..');
const pkg = JSON.parse(fs.readFileSync(path.join(extensionRoot, 'package.json'), 'utf8'));
const output = path.join(repoRoot, 'dist', `SuraLanguage-VSCode-${pkg.version}.vsix`);
fs.mkdirSync(path.dirname(output), { recursive: true });

const vsceScript = path.join(
  extensionRoot,
  'node_modules',
  '@vscode',
  'vsce',
  'vsce',
);
const result = spawnSync(process.execPath, [vsceScript, 'package', '--out', output], {
  cwd: extensionRoot,
  stdio: 'inherit',
  shell: false,
});
if (result.error) throw result.error;
process.exit(result.status ?? 1);
