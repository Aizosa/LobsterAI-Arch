// Fix mangled require$$0$2 (a previous string-replace turned $$0 into $0).
// Use function replacement everywhere: function return values are used literally.
const fs = require('fs');
const path = require('path');

const file = path.join(__dirname, '..', 'dist-electron', 'main.js');
let src = fs.readFileSync(file, 'utf8');

const before = (src.match(/require\$0\$2/g) || []).length;
if (before === 0) {
  console.log('NOTHING TO FIX');
  process.exit(0);
}
// Function replacement -> literal, no $ substitution semantics.
src = src.replace(/require\$0\$2/g, () => 'require$$0$2');
fs.writeFileSync(file, src);
console.log('FIXED', before, 'occurrences');
