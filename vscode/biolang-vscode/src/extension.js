/**
 * BioLang VSCode extension — entry
 * Features: syntax highlighting, code snippets, run commands (bio CLI),
 * stream method completion (Qual::), smart-reference completion (&)
 */
const vscode = require('vscode');
const { execFile, spawn } = require('child_process');
const path = require('path');

/* Method tables for the builtin substreams */
const STREAMS = {
  CIO: [
    /* Text streams: println/print write text, get/getln read text */
    { name: 'println',   detail: 'CIO::println(...) — outputs args (space-separated) and a newline',   doc: 'text stream; e.g. `CIO::println("3 + 4 =", r.res);`' },
    { name: 'print',     detail: 'CIO::print(...) — outputs args without a newline',               doc: 'text stream; e.g. `CIO::print("please wait...");`' },
    { name: 'get',       detail: 'CIO::get() — read one character (text stream)',                doc: 'text stream; empty string on EOF' },
    { name: 'getln',     detail: 'CIO::getln(prompt?) — read one line (text stream)',          doc: 'text stream; e.g. `ALL line = CIO::getln("name: ");`' },
    /* Byte streams: write/read raw bytes */
    { name: 'write',     detail: 'CIO::write(...) — write raw bytes (byte stream)',          doc: 'byte stream; no newline, no formatting' },
    { name: 'read',      detail: 'CIO::read() — read one raw byte 0-255 (byte stream)',       doc: 'byte stream; -1 on EOF' },
    /* Numeric / errors */
    { name: 'readInt',   detail: 'CIO::readInt(prompt?) — read an integer (refused on failure)',     doc: 'e.g. `ALL n = CIO::readInt("age: ");`' },
    { name: 'readNumber',detail: 'CIO::readNumber(prompt?) — read a float (refused on failure)',doc: 'e.g. `ALL x = CIO::readNumber("number: ");`' },
    { name: 'error',     detail: 'CIO::error(...) — output to stderr (no newline)',         doc: 'e.g. `CIO::error("an error occurred");`' }
  ],
  FIO: [
    /* IO core methods (file implementation): open the current file stream first */
    { name: 'open',     detail: 'FIO::open(path, mode?) — open the current file stream',  doc: 'mode: "r" (default)/"w"/"a"; write/read/println/getln operate on it afterwards' },
    { name: 'close',    detail: 'FIO::close() — close the current file stream',            doc: 'e.g. `FIO::close();`' },
    { name: 'println',  detail: 'FIO::println(...) — write a text line to the current file',    doc: 'text stream; requires open(path,"w") first' },
    { name: 'print',    detail: 'FIO::print(...) — write text to the current file (no newline)', doc: 'text stream; requires open(path,"w") first' },
    { name: 'write',    detail: 'FIO::write(...) — write raw bytes to the current file',     doc: 'byte stream; requires open(path,"w") first' },
    { name: 'getln',    detail: 'FIO::getln() — read one line from the current file (text stream)',  doc: 'text stream; requires open(path) first' },
    { name: 'get',      detail: 'FIO::get() — read one character from the current file (text stream)', doc: 'text stream; empty string on EOF' },
    { name: 'read',     detail: 'FIO::read() — read one raw byte from the current file',     doc: 'byte stream; -1 on EOF' },
    /* Convenience file operations */
    { name: 'readFile',  detail: 'FIO::readFile(path) — read an entire file (refused if missing)', doc: 'e.g. `ALL t = FIO::readFile("/tmp/a.txt");`' },
    { name: 'writeFile', detail: 'FIO::writeFile(path, content) — write a file (overwrite)',       doc: 'e.g. `FIO::writeFile("/tmp/a.txt", "hi");`' },
    { name: 'appendFile',detail: 'FIO::appendFile(path, content) — append to a file',             doc: 'e.g. `FIO::appendFile("/tmp/a.txt", "more");`' },
    { name: 'exists',    detail: 'FIO::exists(path) — whether a file exists (1/0)',             doc: 'e.g. `ALL ok = FIO::exists("/tmp/a.txt");`' }
  ],
  SIO: [
    /* Text streams (string implementation): print/println write text, get/getln read text */
    { name: 'println',  detail: 'SIO::println(...) — write text with a newline',  doc: 'text stream; readable afterwards via getln/get/content' },
    { name: 'print',    detail: 'SIO::print(...) — write text (no newline)', doc: 'text stream' },
    { name: 'get',      detail: 'SIO::get() — read one character (text stream)',  doc: 'text stream; empty string when empty' },
    { name: 'getln',    detail: 'SIO::getln() — read one line (text stream)', doc: 'text stream; empty string when empty' },
    /* Byte streams: write/read raw bytes */
    { name: 'write',    detail: 'SIO::write(...) — write raw bytes to the buffer', doc: 'byte stream' },
    { name: 'read',     detail: 'SIO::read() — read one raw byte 0-255',  doc: 'byte stream; -1 when empty' },
    /* Buffer utilities */
    { name: 'content',  detail: 'SIO::content() — read remaining buffer (non-consuming)', doc: 'e.g. `SIO::content().res`' },
    { name: 'clear',    detail: 'SIO::clear() — clear the buffer',              doc: 'e.g. `SIO::clear();`' },
    /* String utilities */
    { name: 'format',    detail: 'SIO::format(fmt, ...) — format a string (%d %s %f)',   doc: 'e.g. `ALL s = SIO::format("%d + %d = %d", 2, 3, 5);`' },
    { name: 'length',    detail: 'SIO::length(str) — length',                          doc: 'e.g. `ALL n = SIO::length("abc");`' },
    { name: 'upper',     detail: 'SIO::upper(str) — uppercase',                         doc: '`SIO::upper("hello")` → HELLO' },
    { name: 'lower',     detail: 'SIO::lower(str) — lowercase',                         doc: '`SIO::lower("ABC")` → abc' },
    { name: 'trim',      detail: 'SIO::trim(str) — strip surrounding whitespace',                      doc: '`SIO::trim("  x  ")` → x' },
    { name: 'contains',  detail: 'SIO::contains(str, sub) — whether it contains (1/0)',        doc: '`SIO::contains("hello", "ell")` → 1' },
    { name: 'substring', detail: 'SIO::substring(str, start, end) — slice',               doc: '`SIO::substring("hello", 1, 3)` → el' },
    { name: 'replace',   detail: 'SIO::replace(str, old, new) — replace',                 doc: '`SIO::replace("a-b", "-", "+")` → a+b' }
  ],
  Threads: [
    { name: 'spawn',  detail: 'Threads::spawn("method", args...) — create a thread, returns its id', doc: 'e.g. `ALL t = Threads::spawn("factorial", 10).res;`' },
    { name: 'yield',  detail: 'Threads::yield() — yield the CPU, schedule other threads', doc: 'cooperative thread switch' },
    { name: 'join',   detail: 'Threads::join(threadId) — wait for a thread and take its result', doc: 'e.g. `ALL r = Threads::join(t);` → r.res' },
    { name: 'active', detail: 'Threads::active() — number of live threads', doc: 'e.g. `Threads::active().res`' },
    { name: 'self',   detail: 'Threads::self() — current thread id (main = 0)', doc: 'e.g. `Threads::self().res`' }
  ],
  Taskm: [
    { name: 'add',      detail: 'Taskm::add("method", args...) — register a task, returns its id', doc: 'e.g. `ALL t = Taskm::add("jobA", 5).res;`' },
    { name: 'interval', detail: 'Taskm::interval(ms) — set the round-robin interval (default 0)', doc: 'e.g. `Taskm::interval(10);`' },
    { name: 'run',      detail: 'Taskm::run() — scheduling loop: runs all tasks round-robin until done', doc: 'automatically switches threads through the loop' },
    { name: 'stop',     detail: 'Taskm::stop() — stop the scheduling loop', doc: 'call from inside a thread, then yield' },
    { name: 'active',   detail: 'Taskm::active() — number of unfinished tasks', doc: 'e.g. `Taskm::active().res`' }
  ],
  Arrays: [
    { name: 'count',   detail: 'Arrays::count() — number of registered Array/Vector instances', doc: 'e.g. `Arrays::count().res`' },
    { name: 'all',     detail: 'Arrays::all() — all instances (an array of arrays)', doc: 'e.g. `ALL xs = Arrays::all().res;`' },
    { name: 'get',     detail: 'Arrays::get(index) — the i-th instance', doc: 'e.g. `Arrays::get(0).res`' },
    { name: 'add',     detail: 'Arrays::add(arrayObject) — register an instance (called by Array/Vector __init__)', doc: 'new Array is inserted into Arrays by default' },
    { name: 'vector',  detail: 'Arrays::vector() — dynamic array (new Vector, auto-growing push)', doc: 'e.g. `ALL v = Arrays::vector(); v::push(10);`' },
    { name: 'forget',  detail: 'Arrays::forget(arrayObject) — remove from the registry', doc: 'e.g. `Arrays::forget(v);`' }
  ],
  Solid: [
    { name: 'new',       detail: 'Solid::new() — create a contiguous stream (contiguous storage + moving head pointer)', doc: 'the underlying storage of the Array/Vector classes' },
    { name: 'len',       detail: 'Solid::len(stream) — remaining length (head pointer to end)', doc: 'e.g. `Solid::len(s).res`' },
    { name: 'get',       detail: 'Solid::get(stream, index) — get an element relative to the head pointer', doc: 'e.g. `Solid::get(s, 0).res`' },
    { name: 'set',       detail: 'Solid::set(stream, index, value) — set an element relative to the head pointer', doc: 'e.g. `Solid::set(s, 0, 42);`' },
    { name: 'push',      detail: 'Solid::push(stream, value) — append at the end', doc: 'e.g. `Solid::push(s, 10);`' },
    { name: 'pop',       detail: 'Solid::pop(stream) — pop from the end', doc: 'e.g. `Solid::pop(s).res`' },
    { name: 'read',      detail: 'Solid::read(stream) — read at the head pointer and advance it', doc: 'e.g. `Solid::read(s).res`' },
    { name: 'peek',      detail: 'Solid::peek(stream) — read at the head pointer without moving', doc: 'e.g. `Solid::peek(s).res`' },
    { name: 'head',      detail: 'Solid::head(stream) — current head pointer position', doc: 'e.g. `Solid::head(s).res`' },
    { name: 'resetHead', detail: 'Solid::resetHead(stream) — reset the head pointer to zero', doc: 'e.g. `Solid::resetHead(s);`' },
    { name: 'clear',     detail: 'Solid::clear(stream) — clear', doc: 'e.g. `Solid::clear(s);`' },
    { name: 'join',      detail: 'Solid::join(stream, sep?) — join remaining data into a string', doc: 'e.g. `Solid::join(s, "-").res`' }
  ],
  Ref: [
    { name: 'read',   detail: 'Ref::read(ref) — read the reference target (r/rw/m permission)', doc: 'e.g. `ALL v = Ref::read(rr).res;`' },
    { name: 'write',  detail: 'Ref::write(ref, value) — write the reference target (w/rw/m permission)', doc: 'e.g. `Ref::write(wr, 10);`' },
    { name: 'move',   detail: 'Ref::move(ref) — take the target (movable m permission only)', doc: 'e.g. `ALL taken = Ref::move(mv);` → target washed away' },
    { name: 'target', detail: 'Ref::target(ref) — target name', doc: 'e.g. `Ref::target(rr).res` → counter' },
    { name: 'perm',   detail: 'Ref::perm(ref) — permission (r/w/rw/m)', doc: 'e.g. `Ref::perm(rr).res`' }
  ],
  Com: [
    { name: 'abs',    detail: 'Com::abs(x) — absolute value', doc: 'e.g. `Com::abs(0-5).res` → 5' },
    { name: 'min',    detail: 'Com::min(x, y) — smaller of the two', doc: 'e.g. `Com::min(3, 5).res` → 3' },
    { name: 'max',    detail: 'Com::max(x, y) — larger of the two', doc: 'e.g. `Com::max(3, 5).res` → 5' },
    { name: 'pow',    detail: 'Com::pow(x, y) — x raised to the power y', doc: 'e.g. `Com::pow(2, 10).res` → 1024' },
    { name: 'sqrt',   detail: 'Com::sqrt(x) — square root (refused for negatives)', doc: 'e.g. `Com::sqrt(9).res` → 3' },
    { name: 'floor',  detail: 'Com::floor(x) — floor', doc: 'e.g. `Com::floor(2.7).res` → 2' },
    { name: 'ceil',   detail: 'Com::ceil(x) — ceiling', doc: 'e.g. `Com::ceil(2.1).res` → 3' },
    { name: 'round',  detail: 'Com::round(x) — round to nearest', doc: 'e.g. `Com::round(2.5).res` → 3' },
    { name: 'sign',   detail: 'Com::sign(x) — sign (-1/0/1)', doc: 'e.g. `Com::sign(0-3).res` → -1' },
    { name: 'sin',    detail: 'Com::sin(x) — sine', doc: 'e.g. `Com::sin(0).res` → 0' },
    { name: 'cos',    detail: 'Com::cos(x) — cosine', doc: 'e.g. `Com::cos(0).res` → 1' },
    { name: 'tan',    detail: 'Com::tan(x) — tangent', doc: 'e.g. `Com::tan(0).res` → 0' },
    { name: 'log',    detail: 'Com::log(x) — natural logarithm (refused for x≤0)', doc: 'e.g. `Com::log(1).res` → 0' },
    { name: 'exp',    detail: 'Com::exp(x) — e raised to the power x', doc: 'e.g. `Com::exp(1).res` → e' }
  ],
  Time: [
    { name: 'now',      detail: 'Time::now() — monotonic clock seconds', doc: 'e.g. `Time::now().res`' },
    { name: 'sleep',    detail: 'Time::sleep(ms) — sleep', doc: 'e.g. `Time::sleep(100);`' },
    { name: 'start',    detail: 'Time::start(?) — start a timer (default first timer owned by the thread)', doc: 'e.g. `Time::start();` → thread\'s first timer, cannot be reset' },
    { name: 'fork',     detail: 'Time::fork() — fork a new timer (resettable)', doc: 'e.g. `ALL t = Time::fork(); Time::reset(t.res);`' },
    { name: 'elapsed',  detail: 'Time::elapsed(?) — elapsed milliseconds of a timer', doc: 'e.g. `Time::elapsed().res`' },
    { name: 'reset',    detail: 'Time::reset(id?) — reset (first timer refuses; forked timers allowed)', doc: 'e.g. `Time::reset(t.res);`' }
  ]
};
/* IOStream aggregates CIO/FIO/SIO (dispatched in order); Console is CIO's pre-forked implementation */
STREAMS.IO = [...STREAMS.CIO, ...STREAMS.FIO, ...STREAMS.SIO];
STREAMS.Console = STREAMS.CIO;
const BUILTIN_STREAMS = ['CIO', 'FIO', 'SIO', 'IO', 'Com', 'Time', 'Solid', 'Arrays', 'Threads', 'Taskm', 'Ref', 'Console'];

/**
 * Find the position of the "}" matching the "{" at start (skipping strings/comments/char literals)
 */
function blockEnd(text, start) {
  let depth = 0, inStr = false, inLine = false, inBlock = false;
  for (let i = start; i < text.length; i++) {
    const c = text[i], n = text[i + 1];
    if (inLine) { if (c === '\n') inLine = false; continue; }
    if (inBlock) { if (c === '*' && n === '/') { inBlock = false; i++; } continue; }
    if (inStr) { if (c === '\\') { i++; continue; } if (c === '"') inStr = false; continue; }
    if (c === '/' && n === '/') { inLine = true; i++; continue; }
    if (c === '/' && n === '*') { inBlock = true; i++; continue; }
    if (c === '"') { inStr = true; continue; }
    if (c === "'") { i++; continue; }                 // roughly skip char literals
    if (c === '{') depth++;
    else if (c === '}') { depth--; if (depth === 0) return i; }
  }
  return -1;
}

/** Extract method names inside a block (line-start void/int/float/double/string/char ... name() */
function methodsIn(block) {
  const methods = new Set();
  for (const mm of block.matchAll(/\b(?:void|int|float|double|string|char)\s+([A-Za-z_]\w*)\s*\(/g)) methods.add(mm[1]);
  return methods;
}

/** Extract field names inside a block (int x, y; / int[] a; / string s; / type T; T n; generic style) */
function fieldsIn(block) {
  const fields = new Set();
  // type name[, name...]; — a name followed by a comma or semicolon is a field; a '(' means a method
  for (const mm of block.matchAll(/\b(?:int|float|double|string|char|[A-Z]\w*)\s*(?:\[\])?\s+([A-Za-z_]\w*)\s*(?=(?:,|;))/g))
    fields.add(mm[1]);
  // subsequent comma-separated field names: int x, y;
  for (const mm of block.matchAll(/,\s*([A-Za-z_]\w*)\s*(?=(?:,|;))/g))
    fields.add(mm[1]);
  return fields;
}

/**
 * Parse declared streams and their members (methods + fields) from the document text.
 * Returns { known: Set(signature stream/class names), map: Map(impl name → { methods, fields }) }
 */
function parseStreams(text) {
  const known = new Set();
  for (const m of text.matchAll(/\bStream\s+([A-Za-z_]\w*)/g)) known.add(m[1]);
  for (const m of text.matchAll(/\bClass\s+([A-Za-z_]\w*)/g)) known.add(m[1]);
  const map = new Map();
  const classNames = new Set();

  // fork: <signature> <impl> { void m() {...} } (use brace balancing for the real block)
  for (const m of text.matchAll(/([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\{/g)) {
    const sig = m[1], impl = m[2];
    if (!known.has(sig)) continue;
    const braceAt = m.index + m[0].length - 1;
    const end = blockEnd(text, braceAt);
    if (end === -1) continue;
    const inner = text.slice(braceAt + 1, end);
    map.set(impl, { methods: methodsIn(inner), fields: fieldsIn(inner) });
  }
  // a signature stream's own signature methods/fields: Stream X { void hello(); int count; }
  for (const m of text.matchAll(/\bStream\s+([A-Za-z_]\w*)\s*\{/g)) {
    const braceAt = m.index + m[0].length - 1;
    const end = blockEnd(text, braceAt);
    if (end === -1) continue;
    const inner = text.slice(braceAt + 1, end);
    map.set(m[1], { methods: methodsIn(inner), fields: fieldsIn(inner) });
  }
  // class declaration: Class X { int x, y; void m() {...} }
  for (const m of text.matchAll(/\bClass\s+([A-Za-z_]\w*)\s*\{/g)) {
    const braceAt = m.index + m[0].length - 1;
    const end = blockEnd(text, braceAt);
    if (end === -1) continue;
    const inner = text.slice(braceAt + 1, end);
    map.set(m[1], { methods: methodsIn(inner), fields: fieldsIn(inner) });
    classNames.add(m[1]);
  }
  return { known, map, classNames };
}

function methodItem(name, detail) {
  const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Method);
  item.insertText = new vscode.SnippetString(`${name}($0)`);
  item.detail = detail;
  return item;
}

function fieldItem(name, detail) {
  const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Property);
  item.insertText = new vscode.SnippetString(`${name}`);
  item.detail = detail;
  return item;
}

function activate(context) {
  /* ---------- Completion: Qual:: → methods; Qual: → stream names ---------- */
  const provider = vscode.languages.registerCompletionItemProvider('biolang', {
    provideCompletionItems(doc, position) {
      const text = doc.getText();
      const before = text.slice(0, doc.offsetAt(position));
      const items = [];

      // already typed `Stream::` or `this::` → suggest methods + fields
      const m2 = before.match(/([A-Za-z_]\w*)::$/);
      if (m2) {
        const qual = m2[1];
        if (STREAMS[qual]) {
          for (const m of STREAMS[qual]) items.push(methodItem(m.name, m.detail));
          return items;
        }
        const { map } = parseStreams(text);
        if (qual === 'this') {
          // this:: → aggregate all declared class members (fields + methods)
          const agg = { methods: new Set(), fields: new Set() };
          for (const { methods, fields } of map.values()) {
            for (const n of methods) agg.methods.add(n);
            for (const n of fields) agg.fields.add(n);
          }
          for (const name of agg.fields) items.push(fieldItem(name, 'class field (this:: attribute)'));
          for (const name of agg.methods) items.push(methodItem(name, 'class method'));
          return items;
        }
        const mem = map.get(qual);
        if (mem) {
          for (const name of mem.fields) items.push(fieldItem(name, `field of stream/class ${qual}`));
          for (const name of mem.methods) items.push(methodItem(name, `method of stream ${qual}`));
          return items;
        }
        return items;
      }

      // already typed `Stream:` (single colon) → suggest stream names (auto-appends ::)
      const m1 = before.match(/([A-Za-z_]\w*):$/);
      if (m1) {
        const { known, map } = parseStreams(text);
        const names = new Set([...BUILTIN_STREAMS, ...known, ...map.keys()]);
        for (const n of names) {
          const item = new vscode.CompletionItem(n, vscode.CompletionItemKind.Class);
          item.insertText = new vscode.SnippetString(`${n}::`);
          item.detail = BUILTIN_STREAMS.includes(n) ? 'builtin stream' : 'declared stream';
          items.push(item);
        }
      }

      // already typed `object.` → suggest request-result props + class fields (v.x / r.res / r.cause)
      const dot = before.match(/([A-Za-z_]\w*)\.$/);
      if (dot) {
        items.push(fieldItem('res', 'request result: value'));
        items.push(fieldItem('cause', 'request result: refusal reason'));
        const { map } = parseStreams(text);
        const seen = new Set();
        for (const { fields } of map.values())
          for (const n of fields) if (!seen.has(n)) { seen.add(n); items.push(fieldItem(n, 'class field (object attribute)')); }
        return items;
      }

      // already typed `new ` → suggest class names + array types (new int[n])
      if (before.match(/new\s+$/)) {
        const { known } = parseStreams(text);
        const names = new Set([...BUILTIN_STREAMS, ...known]);
        for (const n of names) {
          const item = new vscode.CompletionItem(n, vscode.CompletionItemKind.Class);
          item.insertText = new vscode.SnippetString(`${n}($0)`);
          item.detail = 'class instantiation';
          items.push(item);
        }
        for (const t of ['int', 'float', 'double', 'string', 'char']) {
          const item = new vscode.CompletionItem(t, vscode.CompletionItemKind.Struct);
          item.insertText = new vscode.SnippetString(t + '[$1:n]');
          item.detail = 'array literal new int[n]';
          items.push(item);
        }
        return items;
      }

      // already typed `&` (smart-ref start) → suggest permissions r/w/rw/m
      if (before.match(/&\s*$/)) {
        for (const p of ['r', 'w', 'rw', 'm']) {
          const item = new vscode.CompletionItem(p, vscode.CompletionItemKind.Keyword);
          item.insertText = new vscode.SnippetString(`${p} `);
          item.detail = `smart-ref permission ${p}`;
          item.documentation = 'syntax: &perm follow name, e.g. &r u counter (with Ref::read/write/move)';
          items.push(item);
        }
        return items;
      }

      // already typed `&r ` (after a permission) → suggest follow layers u/f/a
      if (before.match(/&\s*(?:r|w|rw|m)\s+$/)) {
        for (const f of ['u', 'f', 'a']) {
          const item = new vscode.CompletionItem(f, vscode.CompletionItemKind.Keyword);
          item.insertText = new vscode.SnippetString(`${f} `);
          item.detail = `reference follow layer ${f}`;
          item.documentation = 'u = program level (Unistream) · f = method level (Functionstream) · a = scope level (Areastream)';
          items.push(item);
        }
        return items;
      }
      return items;
    }
  }, ':', '&', '.', ' ');

  /* ---------- Run commands ---------- */
  /* Webview interactive run panel HTML: output area + input box (input feeds CIO/stdin directly) */
  function panelHtml(fileName) {
    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<style>
  body { margin:0; font-family: var(--vscode-editor-font-family, monospace); font-size: 13px; display:flex; flex-direction:column; height:100vh; }
  header { padding:8px 12px; background: var(--vscode-titleBar-activeBackground); border-bottom:1px solid var(--vscode-panel-border); display:flex; align-items:center; gap:10px; }
  header .dot { width:9px; height:9px; border-radius:50%; background:#888; }
  header.running .dot { background:#4ecb71; animation: pulse 1.2s infinite; }
  @keyframes pulse { 50% { opacity: .35; } }
  header .name { font-weight:600; }
  header .status { opacity:.75; font-size:12px; }
  #out { flex:1; overflow:auto; padding:10px 12px; white-space:pre-wrap; word-break:break-all; }
  #out .err { color: var(--vscode-errorForeground, #f14c4c); }
  #out .done { opacity:.6; margin-top:8px; }
  .inputrow { display:flex; border-top:1px solid var(--vscode-panel-border); }
  .inputrow input { flex:1; border:none; background:transparent; color:var(--vscode-input-foreground); padding:9px 12px; outline:none; font-family:inherit; }
  .inputrow button { border:none; border-left:1px solid var(--vscode-panel-border); background:var(--vscode-button-background); color:var(--vscode-button-foreground); padding:0 16px; cursor:pointer; }
  .inputrow button:disabled { opacity:.4; cursor:default; }
</style>
</head>
<body>
  <header id="hd"><span class="dot"></span><span class="name">${escapeHtml(fileName)}</span><span class="status" id="st">running…</span></header>
  <div id="out"></div>
  <div class="inputrow">
    <input id="in" placeholder="Input (Enter sends to CIO)…" autofocus>
    <button id="send" disabled>Send</button>
  </div>
<script>
  const vscode = acquireVsCodeApi();
  const out = document.getElementById('out');
  const input = document.getElementById('in');
  const send = document.getElementById('send');
  const st = document.getElementById('st');
  const hd = document.getElementById('hd');
  function append(text, cls) {
    const el = document.createElement('span');
    if (cls) el.className = cls;
    el.textContent = text;
    out.appendChild(el);
    out.scrollTop = out.scrollHeight;
  }
  function done(code) {
    hd.classList.remove('running');
    st.textContent = code === 0 ? 'done' : 'exited (' + code + ')';
    input.disabled = send.disabled = true;
    append(code === 0 ? '[exit 0]' : '[exit ' + code + ']', 'done');
  }
  function sendInput() {
    const v = input.value;
    if (v === '') return;
    vscode.postMessage({ type: 'input', text: v });
    append(v + '\\n');
    input.value = '';
    input.focus();
  }
  input.addEventListener('keydown', e => { if (e.key === 'Enter') sendInput(); });
  send.addEventListener('click', sendInput);
  window.addEventListener('message', ev => {
    const m = ev.data;
    if (m.type === 'out') append(m.text, '');
    else if (m.type === 'err') append(m.text, 'err');
    else if (m.type === 'ready') { send.disabled = false; input.focus(); }
    else if (m.type === 'done') done(m.code);
  });
</script>
</body>
</html>`;
  }
  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c]));
  }

  const runFile = vscode.commands.registerCommand('biolang.runFile', async () => {
    const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
    if (!doc) { vscode.window.showWarningMessage('No file is open'); return; }
    if (doc.languageId !== 'biolang') { vscode.window.showWarningMessage('The active file is not BioLang (.bl/.bio)'); return; }
    await doc.save();
    const fileName = doc.fileName;
    const panel = vscode.window.createWebviewPanel(
      'biolangRun', `BioLang — ${path.basename(fileName)}`, vscode.ViewColumn.One,
      { enableScripts: true, retainContextWhenHidden: true }
    );
    panel.webview.html = panelHtml(fileName);
    const cwd = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0]
      ? vscode.workspace.workspaceFolders[0].uri.fsPath : path.dirname(fileName);
    /* interactive child process: stdout/stderr → panel, panel input → stdin (CIO) */
    const child = spawn('bio', [fileName], { cwd });
    let killed = false;
    child.stdout.on('data', d => panel.webview.postMessage({ type: 'out', text: d.toString() }));
    child.stderr.on('data', d => panel.webview.postMessage({ type: 'err', text: d.toString() }));
    child.on('error', e => panel.webview.postMessage({ type: 'err', text: '[bio] ' + e.message + '\n' }));
    child.on('close', code => {
      if (!killed) panel.webview.postMessage({ type: 'done', code });
    });
    panel.webview.onDidReceiveMessage(msg => {
      if (msg.type === 'input') {
        if (child.stdin.writable) child.stdin.write(msg.text + '\n');
      }
    });
    panel.onDidDispose(() => { killed = true; child.kill(); });
    panel.webview.postMessage({ type: 'ready' });
  });

  /* Compile current file: bio -b file → self-contained executable (output into the file's directory) */
  const compileFile = vscode.commands.registerCommand('biolang.compileFile', async () => {
    const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
    if (!doc) { vscode.window.showWarningMessage('No file is open'); return; }
    if (doc.languageId !== 'biolang') { vscode.window.showWarningMessage('The active file is not BioLang (.bl/.bio)'); return; }
    await doc.save();
    const out = vscode.window.createOutputChannel('BioLang Compile');
    out.show(true);
    const base = doc.fileName.replace(/\.[^.]+$/, '');
    const outBin = base;   /* strip the extension, e.g. example.bio → example */
    const cwd = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0]
      ? vscode.workspace.workspaceFolders[0].uri.fsPath : path.dirname(doc.fileName);
    out.appendLine(`$ bio -b ${doc.fileName} -o ${outBin}`);
    execFile('bio', ['-b', doc.fileName, '-o', outBin], { cwd }, (err, stdout, stderr) => {
      if (stdout) out.append(stdout);
      if (stderr) out.append(stderr);
      if (err) {
        out.appendLine(`[bio compile failed, exit code ${err.code}] (make sure bio is on PATH: ~/.local/bin/bio)`);
      } else {
        out.appendLine(`✔ compiled: ${outBin}`);
      }
    });
  });

  /* Project root: current workspace (with package.toml) or upward from the active file's directory */
  function projectRoot() {
    if (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0]) {
      const root = vscode.workspace.workspaceFolders[0].uri.fsPath;
      if (require('fs').existsSync(require('path').join(root, 'package.toml'))) return root;
    }
    const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
    if (doc) {
      let dir = path.dirname(doc.fileName);
      for (let i = 0; i < 6; i++) {
        if (require('fs').existsSync(path.join(dir, 'package.toml'))) return dir;
        const up = path.dirname(dir);
        if (up === dir) break;
        dir = up;
      }
    }
    return null;
  }

  function projCmd(cmd, args, successMsg, isRun) {
    const dir = projectRoot();
    if (!dir) { vscode.window.showWarningMessage('No project found (package.toml)'); return; }
    const out = vscode.window.createOutputChannel('BioLang Project');
    out.show(true);
    out.appendLine(`$ bio ${cmd} ${dir}${args ? ' ' + args : ''}`);
    if (isRun) {
      /* project run: Webview interactive (stdin input) */
      const panel = vscode.window.createWebviewPanel('biolangProjRun', `BioLang Project Run`, vscode.ViewColumn.One, { enableScripts: true, retainContextWhenHidden: true });
      panel.webview.html = panelHtml('Project run (bio run)');
      const child = spawn('bio', ['run', dir], { cwd: dir });
      let killed = false;
      child.stdout.on('data', d => panel.webview.postMessage({ type: 'out', text: d.toString() }));
      child.stderr.on('data', d => panel.webview.postMessage({ type: 'err', text: d.toString() }));
      child.on('error', e => panel.webview.postMessage({ type: 'err', text: '[bio] ' + e.message + '\n' }));
      child.on('close', code => { if (!killed) panel.webview.postMessage({ type: 'done', code }); });
      panel.webview.onDidReceiveMessage(msg => { if (msg.type === 'input' && child.stdin.writable) child.stdin.write(msg.text + '\n'); });
      panel.onDidDispose(() => { killed = true; child.kill(); });
      panel.webview.postMessage({ type: 'ready' });
      return;
    }
    execFile('bio', [cmd, dir].concat(args ? args.split(' ') : []), { cwd: dir }, (err, stdout, stderr) => {
      if (stdout) out.append(stdout);
      if (stderr) out.append(stderr);
      if (err) out.appendLine(`[bio ${cmd} failed, exit code ${err.code}]`);
      else if (successMsg) out.appendLine(successMsg);
    });
  }

  const projectInit = vscode.commands.registerCommand('biolang.projectInit', async () => {
    const name = await vscode.window.showInputBox({ prompt: 'Project name', value: 'myapp' });
    if (!name) return;
    const out = vscode.window.createOutputChannel('BioLang Project');
    out.show(true);
    const ws = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0]
      ? vscode.workspace.workspaceFolders[0].uri.fsPath : '';
    execFile('bio', ['init', ws ? path.join(ws, name) : name], {}, (err, stdout, stderr) => {
      if (stdout) out.append(stdout);
      if (stderr) out.append(stderr);
      if (err) out.appendLine(`[bio init failed, exit code ${err.code}]`);
    });
  });

  const projectBuild = vscode.commands.registerCommand('biolang.projectBuild', () => {
    const dir = projectRoot();
    if (!dir) { vscode.window.showWarningMessage('No project found (package.toml)'); return; }
    const out = vscode.window.createOutputChannel('BioLang Project');
    out.show(true);
    out.appendLine(`$ bio build ${dir}`);
    execFile('bio', ['build', dir, '-o', path.join(dir, 'app')], { cwd: dir }, (err, stdout, stderr) => {
      if (stdout) out.append(stdout);
      if (stderr) out.append(stderr);
      if (err) out.appendLine(`[bio build failed, exit code ${err.code}]`);
      else out.appendLine('✔ project compiled');
    });
  });

  const projectRun = vscode.commands.registerCommand('biolang.projectRun', () => projCmd('run', null, null, true));

  const projectInstall = vscode.commands.registerCommand('biolang.projectInstall', () => projCmd('install', null, '✔ dependencies installed'));

  const projectDestroy = vscode.commands.registerCommand('biolang.projectDestroy', async () => {
    const dir = projectRoot();
    if (!dir) { vscode.window.showWarningMessage('No project found (package.toml)'); return; }
    const yes = await vscode.window.showWarningMessage(`Destroy the build artifacts of ${path.basename(dir)}?`, { modal: true }, 'Destroy');
    if (!yes) return;
    const out = vscode.window.createOutputChannel('BioLang Project');
    out.show(true);
    execFile('bio', ['destroy', dir], { cwd: dir }, (err, stdout, stderr) => {
      if (stdout) out.append(stdout);
      if (stderr) out.append(stderr);
      if (err) out.appendLine(`[bio destroy failed, exit code ${err.code}]`);
    });
  });

  const runDemo = vscode.commands.registerCommand('biolang.runDemo', () => {
    const out = vscode.window.createOutputChannel('BioLang');
    out.show(true);
    out.appendLine('$ bio');
    execFile('bio', [], {}, (err, stdout, stderr) => {
      if (stdout) out.append(stdout);
      if (stderr) out.append(stderr);
      if (err) out.appendLine(`[bio exit code ${err.code}] (make sure bio is on PATH: ~/.local/bin/bio)`);
    });
  });

  context.subscriptions.push(provider, runFile, compileFile, projectInit, projectBuild, projectRun, projectInstall, projectDestroy, runDemo);
}

function deactivate() {}

module.exports = { activate, deactivate };
