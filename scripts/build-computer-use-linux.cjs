#!/usr/bin/env node
/**
 * Builds and packages the Linux Computer Use helper.
 *
 * Why a script instead of a Makefile target invoked from electron-builder:
 * the archive has to be produced before packaging, and it has to sit in
 * resources/computer-use/ under the exact name computerUseRuntime.ts expects
 * (lobsterai-computer-use-runtime-linux-x64-<version>.tar.gz). Keeping that
 * name in one place is the whole point.
 *
 * Steps:
 *   1. Check the build dependencies with pkg-config and explain what to install
 *      if any are missing. Node's child_process ENOENT is not a useful message
 *      for a missing atspi header.
 *   2. make clean && make in resources/computer-use-linux.
 *   3. Stage a clean tree (binary, client module, runtime.json, licenses) and
 *      tar it with mode bits preserved, since the helper is spawned directly.
 *
 * Env:
 *   CU_SKIP_BUILD=1   package the existing binary instead of rebuilding
 *   CU_OUT_DIR=...    where to write the archive (default resources/computer-use)
 */
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');

const ROOT = path.resolve(__dirname, '..');
const HELPER_DIR = path.join(ROOT, 'resources', 'computer-use-linux');
const BINARY = 'lobsterai-computer-use-linux';
const OUT_DIR = process.env.CU_OUT_DIR
  ? path.resolve(process.env.CU_OUT_DIR)
  : path.join(ROOT, 'resources', 'computer-use');

const REQUIRED_PKGS = [
  'atspi-2',
  'json-glib-1.0',
  'cairo',
  'glib-2.0',
  'gobject-2.0',
  'gio-2.0',
];
const OPTIONAL_PKGS = ['x11', 'xtst', 'libei-1.0', 'wayland-client'];

function fail(message) {
  console.error(`\n[cu-linux] ${message}\n`);
  process.exit(1);
}

function hasPkg(pkg) {
  try {
    execFileSync('pkg-config', ['--exists', pkg], { stdio: 'ignore' });
    return true;
  } catch {
    return false;
  }
}

function checkDependencies() {
  try {
    execFileSync('pkg-config', ['--version'], { stdio: 'ignore' });
  } catch {
    fail(
      'pkg-config not found. Install the build toolchain first:\n'
      + '  Arch:   sudo pacman -S --needed base-devel pkgconf\n'
      + '  Debian: sudo apt install build-essential pkg-config\n'
      + '  Fedora: sudo dnf install gcc make pkgconf-pkg-config',
    );
  }

  const missing = REQUIRED_PKGS.filter(pkg => !hasPkg(pkg));
  if (missing.length > 0) {
    fail(
      `missing build dependencies: ${missing.join(', ')}\n`
      + 'Install them with one of:\n'
      + '  Arch:   sudo pacman -S --needed at-spi2-core json-glib cairo glib2 libx11 libxtst\n'
      + '  Debian: sudo apt install libatspi2.0-dev libjson-glib-dev libcairo2-dev libglib2.0-dev libx11-dev libxtst-dev\n'
      + '  Fedora: sudo dnf install at-spi2-core-devel json-glib-devel cairo-devel glib2-devel libX11-devel libXtst-devel',
    );
  }

  const missingOptional = OPTIONAL_PKGS.filter(pkg => !hasPkg(pkg));
  if (missingOptional.length > 0) {
    console.warn(
      `[cu-linux] WARNING: ${missingOptional.join(', ')} not found. `
      + 'Install libx11/libxtst for X11/XWayland support, libei for the '
      + 'Wayland EIS virtual-seat input route, and wayland-client for the KDE '
      + 'plasma-window-management window enumeration fallback; without them the '
      + 'corresponding feature degrades or is unavailable.',
    );
  }
}

function readRuntimeVersion() {
  const manifestPath = path.join(HELPER_DIR, 'runtime.json');
  if (!fs.existsSync(manifestPath)) {
    fail(`missing ${path.relative(ROOT, manifestPath)}`);
  }
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  if (!manifest.version) {
    fail('runtime.json has no version');
  }
  return manifest.version;
}

function build() {
  if (process.env.CU_SKIP_BUILD === '1') {
    console.log('[cu-linux] CU_SKIP_BUILD=1, reusing the existing binary');
    return;
  }
  console.log('[cu-linux] building helper');
  execFileSync('make', ['clean'], { cwd: HELPER_DIR, stdio: 'inherit' });
  execFileSync('make', ['-j', String(Math.max(1, os.cpus().length))], {
    cwd: HELPER_DIR,
    stdio: 'inherit',
  });
}

/*
 * Copies a file without fs.copyFileSync.
 *
 * copyFileSync goes through a brokered FS shim in some sandboxed build
 * environments and fails with EPERM even when the destination is writable — read
 * into a buffer and write it out instead, which uses the plain path. The
 * binaries here are small (a few hundred KB), so buffering costs nothing.
 */
function copyFile(from, to) {
  fs.writeFileSync(to, fs.readFileSync(from));
}

function stageAndArchive(version) {
  const binaryPath = path.join(HELPER_DIR, BINARY);
  if (!fs.existsSync(binaryPath)) {
    fail(`helper binary missing after build: ${path.relative(ROOT, binaryPath)}`);
  }

  const archiveName = `lobsterai-computer-use-runtime-linux-x64-${version}.tar.gz`;
  /*
   * Stage inside the repo rather than os.tmpdir(). On Arch /tmp is a small
   * tmpfs (10MB by default) and in some sandboxed build environments it is not
   * writable across processes, which turns a copy into EPERM halfway through the
   * archive. .tools/tmp is already excluded from packaging and from git.
   */
  const stageParent = path.join(ROOT, '.tools', 'tmp');
  fs.mkdirSync(stageParent, { recursive: true });
  const stageRoot = fs.mkdtempSync(path.join(stageParent, 'cu-linux-stage-'));
  try {
    // runtime.json declares runtimePackageRoot "." and helper at the top level,
    // so the archive's root *is* the runtime root.
    copyFile(path.join(HELPER_DIR, 'runtime.json'), path.join(stageRoot, 'runtime.json'));
    copyFile(binaryPath, path.join(stageRoot, BINARY));
    fs.chmodSync(path.join(stageRoot, BINARY), 0o755);

    const clientDir = path.join(stageRoot, 'client');
    fs.mkdirSync(clientDir, { recursive: true });
    copyFile(
      path.join(HELPER_DIR, 'client', 'index.mjs'),
      path.join(clientDir, 'index.mjs'),
    );

    // Ship the source alongside the binary: MIT/Apache-style attribution needs it,
    // and a user on a different distro may want to rebuild against their glib.
    const srcDir = path.join(stageRoot, 'src');
    fs.mkdirSync(srcDir, { recursive: true });
    for (const entry of fs.readdirSync(path.join(HELPER_DIR, 'src'))) {
      if (entry.endsWith('.c') || entry.endsWith('.h')) {
        copyFile(path.join(HELPER_DIR, 'src', entry), path.join(srcDir, entry));
      }
    }
    for (const entry of ['Makefile', 'README.md']) {
      const from = path.join(HELPER_DIR, entry);
      if (fs.existsSync(from)) {
        copyFile(from, path.join(stageRoot, entry));
      }
    }

    fs.mkdirSync(OUT_DIR, { recursive: true });
    const archivePath = path.join(OUT_DIR, archiveName);
    fs.rmSync(archivePath, { force: true });
    // -h keeps symlinks; --owner/--group 0 make extraction host-independent.
    execFileSync('tar', [
      '-czf', archivePath,
      '-C', stageRoot,
      '--owner=0', '--group=0',
      '.',
    ], { stdio: 'inherit' });

    const { size } = fs.statSync(archivePath);
    console.log(
      `[cu-linux] wrote ${path.relative(ROOT, archivePath)} (${(size / 1024).toFixed(1)} KiB)`,
    );
    return archivePath;
  } finally {
    fs.rmSync(stageRoot, { recursive: true, force: true });
  }
}

function main() {
  if (process.platform !== 'linux') {
    console.log('[cu-linux] not building on Linux; skipping');
    return;
  }
  checkDependencies();
  const version = readRuntimeVersion();
  build();
  stageAndArchive(version);
}

main();
