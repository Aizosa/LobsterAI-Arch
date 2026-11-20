// Patch dist-electron/main.js: replace blanket --disable-gpu with SwiftShader path.
const fs = require('fs');
const path = require('path');

const file = path.join(__dirname, '..', 'dist-electron', 'main.js');
let src = fs.readFileSync(file, 'utf8');

const D = String.fromCharCode(36); // '$' — avoid shell escaping pitfalls
const oldBlock = [
  '  const forceHardwareGpu = process.env.LOBSTERAI_ENABLE_GPU === "1" || process.env.LOBSTERAI_ENABLE_GPU === "true";',
  '  if (!forceHardwareGpu && !disableGpu) {',
  `    require$$0${D}2.app.commandLine.appendSwitch("disable-gpu");`,
  '  }',
].join('\n');

const newBlock = [
  '  const forceHardwareGpu = process.env.LOBSTERAI_ENABLE_GPU === "1" || process.env.LOBSTERAI_ENABLE_GPU === "true";',
  '  if (forceHardwareGpu) {',
  '    require$$0' + D + '2.app.commandLine.appendSwitch("disable-gpu-sandbox");',
  '  } else if (!disableGpu) {',
  '    require$$0' + D + '2.app.commandLine.appendSwitch("use-gl", "angle");',
  '    require$$0' + D + '2.app.commandLine.appendSwitch("use-angle", "swiftshader");',
  '    require$$0' + D + '2.app.commandLine.appendSwitch("enable-unsafe-swiftshader");',
  '  }',
].join('\n');

if (!src.includes(oldBlock)) {
  console.error('PATCH FAILED: old block not found');
  process.exit(1);
}
src = src.replace(oldBlock, newBlock);
fs.writeFileSync(file, src);
console.log('PATCH OK');
