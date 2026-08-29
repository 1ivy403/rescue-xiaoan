import assert from 'node:assert/strict';
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

const REQUIRED_FILES = [
  'README.md',
  '.gitignore',
  'CONTRIBUTING.md',
  'technical/README.md',
  'demo/README.md',
  'docs/project-overview.md',
  'docs/architecture.md',
  'docs/hardware.md',
  'docs/technical-details.md',
  'docs/limitations.md',
  'showcase/README.md',
  'submission/project-description.md',
  'submission/pitch-script.md',
  'submission/credits.md',
  'showcase/videos/rescue-xiaoan-demo.mp4',
  'showcase/videos/index.html',
  'showcase/poster/rescue-xiaoan-poster.png',
];

const DEMO_FILES = [
  'demo/rescue-command-center/index.html',
  'demo/rescue-command-center/assets/rescue/live-rescue.mp4',
  'demo/rescue-command-center/assets/rescue/p01.png',
  'demo/rescue-command-center/assets/rescue/p02.png',
  'demo/rescue-command-center/assets/rescue/p03.png',
  'demo/rescue-command-center/README.md',
];

const FORBIDDEN_NAMES = [
  '.env',
  '.DS_Store',
  '__pycache__',
];

const FORBIDDEN_EXTENSIONS = new Set(['.onnx', '.pem', '.key', '.p12']);
const GITHUB_FILE_LIMIT_BYTES = 100 * 1024 * 1024;

function walk(directory) {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    if (entry.name === '.git') return [];
    const absolutePath = path.join(directory, entry.name);
    return entry.isDirectory() ? walk(absolutePath) : [absolutePath];
  });
}

test('judge-facing repository entry points exist', () => {
  for (const relativePath of REQUIRED_FILES) {
    assert.equal(existsSync(path.join(ROOT, relativePath)), true, `missing ${relativePath}`);
  }
});

test('root README identifies Rescue Xiaoan and starts with the outcome', () => {
  const readme = readFileSync(path.join(ROOT, 'README.md'), 'utf8');
  assert.match(readme, /^# Rescue Xiaoan/m);
  assert.match(readme, /360/);
  assert.match(readme, /机器狗/);
  assert.match(readme, /人到不了的地方，让它先找到生命/);
  assert.match(readme, /showcase\/poster\/rescue-xiaoan-poster\.png/);
  assert.match(readme, /showcase\/videos\/rescue-xiaoan-demo\.mp4/);
  assert.match(
    readme,
    /https:\/\/1ivy403\.github\.io\/rescue-xiaoan\/demo\/rescue-command-center\//,
  );
  assert.doesNotMatch(readme.slice(0, 800), /安装|Installation/);
});

test('rescue command-center demo is self-contained inside its directory', () => {
  for (const relativePath of DEMO_FILES) {
    assert.equal(existsSync(path.join(ROOT, relativePath)), true, `missing ${relativePath}`);
  }

  const html = readFileSync(path.join(ROOT, 'demo/rescue-command-center/index.html'), 'utf8');
  assert.doesNotMatch(html, /file:\/\//);
  assert.doesNotMatch(html, /(?:127\.0\.0\.1|localhost)/);
  assert.match(html, /assets\/rescue\/live-rescue\.mp4/);
  for (const target of ['p01', 'p02', 'p03']) {
    assert.match(html, new RegExp(`assets/rescue/${target}\\.png`));
  }
});

test('documentation states the manual robot-control boundary honestly', () => {
  const rootReadme = readFileSync(path.join(ROOT, 'README.md'), 'utf8');
  const hardware = readFileSync(path.join(ROOT, 'docs/hardware.md'), 'utf8');
  const robotControl = readFileSync(path.join(ROOT, 'technical/robot-control/README.md'), 'utf8');
  const combined = `${rootReadme}\n${hardware}\n${robotControl}`;
  const architecture = rootReadme.match(/## 系统架构([\s\S]*?)## 当前阶段实现边界/)?.[1] ?? '';

  assert.match(combined, /手动遥控/);
  assert.match(combined, /未开放 SDK/);
  assert.match(combined, /机械臂.*硬件故障/);
  assert.match(rootReadme, /## 当前阶段实现边界[\s\S]*手动遥控/);
  assert.doesNotMatch(architecture, /现场操作员|手动遥控/);
  assert.doesNotMatch(rootReadme, /机器狗自主/);
});

test('public video page embeds the project demo with native controls', () => {
  const page = readFileSync(path.join(ROOT, 'showcase/videos/index.html'), 'utf8');
  assert.match(page, /<video[^>]*\bcontrols\b/);
  assert.match(page, /src="\.\/rescue-xiaoan-demo\.mp4"/);
  assert.match(page, /playsinline/);
  assert.doesNotMatch(page, /autoplay/);
  assert.doesNotMatch(page, /https?:\/\/(?:cdn|unpkg|jsdelivr)/);
});

test('repository excludes secrets, caches, model weights, and oversized files', () => {
  for (const absolutePath of walk(ROOT)) {
    const relativePath = path.relative(ROOT, absolutePath);
    const pathSegments = relativePath.split(path.sep);
    assert.equal(
      pathSegments.some((segment) => FORBIDDEN_NAMES.includes(segment)),
      false,
      `forbidden repository file: ${relativePath}`,
    );
    assert.equal(
      FORBIDDEN_EXTENSIONS.has(path.extname(absolutePath).toLowerCase()),
      false,
      `forbidden repository extension: ${relativePath}`,
    );
    assert.ok(statSync(absolutePath).size < GITHUB_FILE_LIMIT_BYTES, `file exceeds GitHub limit: ${relativePath}`);
  }
});
