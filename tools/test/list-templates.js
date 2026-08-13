// Parse a game Templates folder and emit the random-scenario template list as JSON:
//   [ { "index": 0, "file": "Ascension.lua", "name": "Ascension 1.5.2" }, ... ]
// The displayed name is template.name from each .lua, resolved with a tiny AST evaluator
// (string literals, top-level locals, `..` concatenation, and simple if/elseif/return
// functions like getName/smm/gmm). No Lua runtime, no game, no compilation.
//
// Usage:  npm i luaparse  &&  node list-templates.js <TemplatesDir>
//   stdout: the JSON array (consumed by the generation-matrix workflow)
//   stderr: a readable index | name table
//
// index is the case-sensitive filename order used by the generator's std::set<fs::path>.
const fs = require('fs');
const path = require('path');
const luaparse = require('luaparse');

function strRaw(node) {
  if (!node || node.type !== 'StringLiteral') return null;
  if (typeof node.value === 'string') return node.value;
  if (typeof node.raw === 'string') return node.raw.replace(/^['"]|['"]$/g, '');
  return null;
}
function collect(ast) {
  const locals = {}, funcs = {};
  for (const st of ast.body) {
    if (st.type === 'LocalStatement') st.variables.forEach((v, i) => {
      const init = st.init && st.init[i];
      const s = strRaw(init);
      if (s !== null) locals[v.name] = s;
      else if (init && init.type === 'NumericLiteral') locals[v.name] = init.value;
    });
    if (st.type === 'FunctionDeclaration' && st.identifier && st.identifier.type === 'Identifier') funcs[st.identifier.name] = st;
  }
  return { locals, funcs };
}
class Unresolved extends Error {}
function evalExpr(node, scope, funcs) {
  if (!node) throw new Unresolved('nil');
  switch (node.type) {
  case 'StringLiteral': return strRaw(node);
  case 'NumericLiteral': return node.value;
  case 'BooleanLiteral': return node.value;
  case 'Identifier':
    if (node.name in scope) return scope[node.name];
    throw new Unresolved('id ' + node.name);
  case 'BinaryExpression': {
    const l = evalExpr(node.left, scope, funcs), r = evalExpr(node.right, scope, funcs);
    switch (node.operator) {
    case '..': return String(l) + String(r);
    case '==': return l === r;
    case '~=': return l !== r;
    case '+': return l + r; case '-': return l - r; case '*': return l * r;
    default: throw new Unresolved('op ' + node.operator);
    }
  }
  case 'CallExpression': {
    const fn = node.base && node.base.type === 'Identifier' ? funcs[node.base.name] : null;
    if (!fn) throw new Unresolved('call ' + (node.base && node.base.name));
    const child = Object.assign({}, scope);
    fn.parameters.forEach((p, i) => { child[p.name] = node.arguments[i] ? evalExpr(node.arguments[i], scope, funcs) : undefined; });
    return evalBlock(fn.body, child, funcs);
  }
  default: throw new Unresolved('expr ' + node.type);
  }
}
function evalBlock(body, scope, funcs) {
  for (const st of body) {
    if (st.type === 'ReturnStatement') return st.arguments[0] ? evalExpr(st.arguments[0], scope, funcs) : undefined;
    if (st.type === 'IfStatement') for (const cl of st.clauses)
      if (cl.type === 'ElseClause' || evalExpr(cl.condition, scope, funcs)) return evalBlock(cl.body, scope, funcs);
  }
  throw new Unresolved('no return');
}
function templateTable(ast) {
  let result = null;
  for (const st of ast.body) {
    if (st.type === 'AssignmentStatement') for (let i = 0; i < st.variables.length; i++)
      if (st.variables[i].type === 'Identifier' && st.variables[i].name === 'template')
        result = st.init[i] && st.init[i].type === 'TableConstructorExpression' ? st.init[i] : null;
  }
  return result;
}
function tableField(tbl, key) {
  for (const f of tbl.fields) {
    if (f.type === 'TableKeyString' && f.key.name === key) return f.value;
    if (f.type === 'TableKey' && strRaw(f.key) === key) return f.value;
  }
  return null;
}
function readTemplate(code) {
  const ast = luaparse.parse(code, { comments: false, luaVersion: '5.3' });
  const { locals, funcs } = collect(ast);
  const tbl = templateTable(ast);
  if (!tbl) return null;
  // readTemplateSettings requires template.getContents to exist and be a function. The static
  // parser cannot execute arbitrary Lua, but requiring the field excludes ordinary helper files
  // while preserving the same index for real templates whose display name is computed dynamically.
  const getContents = tableField(tbl, 'getContents');
  const hasGetContents = getContents && (getContents.type === 'FunctionDeclaration' ||
    (getContents.type === 'Identifier' && Object.prototype.hasOwnProperty.call(funcs, getContents.name)));
  if (!hasGetContents) return null;
  const v = tableField(tbl, 'name');
  if (!v) return { name: 'default name', resolved: true };
  const s = strRaw(v);
  if (s !== null) return { name: s, resolved: true };
  try {
    return { name: String(evalExpr(v, locals, funcs)), resolved: true };
  } catch (e) {
    if (!(e instanceof Unresolved)) throw e;
    return { name: null, resolved: false };
  }
}

const dir = process.argv[2];
if (!dir) { console.error('usage: node list-templates.js <TemplatesDir>'); process.exit(2); }
// Match the generator's order: scenariotemplates.cpp inserts full paths into
// std::set<std::filesystem::path>, whose path::compare ordering is case-sensitive.
const files = fs.readdirSync(dir).filter(f => f.endsWith('.lua'))
  .sort((a, b) => a < b ? -1 : a > b ? 1 : 0);
const list = [];
for (const file of files) {
  try {
    const parsed = readTemplate(fs.readFileSync(path.join(dir, file), 'latin1'));
    if (!parsed) continue;
    // Name is optional in D2RSG, and valid templates may compute it with Lua beyond this tiny
    // evaluator. Keep their numeric index and use the filename only as a diagnostic label.
    const name = parsed.resolved ? parsed.name : `${path.parse(file).name} (dynamic name)`;
    list.push({ index: list.length, file, name });
  } catch (e) { /* mirror the game's skip-on-template-read-error behavior */ }
}
for (const t of list) console.error(String(t.index).padStart(2) + ' | ' + t.name);
process.stdout.write(JSON.stringify(list));
