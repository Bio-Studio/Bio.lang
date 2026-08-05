/**
 * BioLang VSCode 扩展 — 入口
 * 功能：语法高亮、代码片段、运行命令（bio CLI）、流方法补全（Qual::）、智能引用补全（&）
 */
const vscode = require('vscode');
const { execFile, spawn } = require('child_process');
const path = require('path');

/* 内置子流的方法表 */
const STREAMS = {
  CIO: [
    { name: 'println',   detail: 'CIO::println(...) — 输出参数（空格分隔）并换行',   doc: '示例: `CIO::println("3 + 4 =", r.res);`' },
    { name: 'print',     detail: 'CIO::print(...) — 输出参数，不换行',               doc: '示例: `CIO::print("请稍候...");`' },
    { name: 'write',     detail: 'CIO::write(...) — IO 最基础二进制方法：裸写原始字节', doc: '示例: `IO::write("A");`（不换行不格式化）' },
    { name: 'read',      detail: 'CIO::read(提示?) — 读取一行输入，返回字符串',       doc: '示例: `ALL name = CIO::read("名字: ");`' },
    { name: 'readln',    detail: 'CIO::readln(提示?) — 读取一行（IO 核心方法）',      doc: '示例: `ALL line = IO::readln();`' },
    { name: 'readInt',   detail: 'CIO::readInt(提示?) — 读取整数（失败 → 拒绝）',     doc: '示例: `ALL n = CIO::readInt("年龄: ");`' },
    { name: 'readNumber',detail: 'CIO::readNumber(提示?) — 读取浮点数（失败 → 拒绝）',doc: '示例: `ALL x = CIO::readNumber("小数: ");`' },
    { name: 'error',     detail: 'CIO::error(...) — 输出到 stderr（不换行）',         doc: '示例: `CIO::error("出错了");`' }
  ],
  FIO: [
    { name: 'readFile',  detail: 'FIO::readFile(路径) — 读取整个文件（不存在 → 拒绝）', doc: '示例: `ALL t = FIO::readFile("/tmp/a.txt");`' },
    { name: 'writeFile', detail: 'FIO::writeFile(路径, 内容) — 写入文件（覆盖）',       doc: '示例: `FIO::writeFile("/tmp/a.txt", "hi");`' },
    { name: 'appendFile',detail: 'FIO::appendFile(路径, 内容) — 追加写入',             doc: '示例: `FIO::appendFile("/tmp/a.txt", "more");`' },
    { name: 'exists',    detail: 'FIO::exists(路径) — 文件是否存在（1/0）',             doc: '示例: `ALL ok = FIO::exists("/tmp/a.txt");`' }
  ],
  SIO: [
    /* IO 核心方法（字符串实现：写入/读取内存缓冲区） */
    { name: 'println',  detail: 'SIO::println(...) — 写入缓冲区并换行',  doc: 'IO 核心方法；之后 SIO::readln/content 可读' },
    { name: 'print',    detail: 'SIO::print(...) — 写入缓冲区（不换行）', doc: 'IO 核心方法' },
    { name: 'write',    detail: 'SIO::write(...) — 写入缓冲区（裸写）',   doc: 'IO 核心方法' },
    { name: 'read',     detail: 'SIO::read() — 从缓冲区读一行（消费）',   doc: 'IO 核心方法；空则返回空串' },
    { name: 'readln',   detail: 'SIO::readln() — 从缓冲区读一行（消费）', doc: 'IO 核心方法；空则返回空串' },
    { name: 'content',  detail: 'SIO::content() — 读缓冲区剩余（不消费）', doc: '示例: `SIO::content().res`' },
    { name: 'clear',    detail: 'SIO::clear() — 清空缓冲区',              doc: '示例: `SIO::clear();`' },
    /* 字符串工具 */
    { name: 'format',    detail: 'SIO::format(格式, ...) — 格式化字符串（%d %s %f）',   doc: '示例: `ALL s = SIO::format("%d + %d = %d", 2, 3, 5);`' },
    { name: 'length',    detail: 'SIO::length(字符串) — 长度',                          doc: '示例: `ALL n = SIO::length("abc");`' },
    { name: 'upper',     detail: 'SIO::upper(字符串) — 转大写',                         doc: '示例: `SIO::upper("hello")` → HELLO' },
    { name: 'lower',     detail: 'SIO::lower(字符串) — 转小写',                         doc: '示例: `SIO::lower("ABC")` → abc' },
    { name: 'trim',      detail: 'SIO::trim(字符串) — 去首尾空白',                      doc: '示例: `SIO::trim("  x  ")` → x' },
    { name: 'contains',  detail: 'SIO::contains(字符串, 子串) — 是否包含（1/0）',        doc: '示例: `SIO::contains("hello", "ell")` → 1' },
    { name: 'substring', detail: 'SIO::substring(字符串, 起, 止) — 截取',               doc: '示例: `SIO::substring("hello", 1, 3)` → el' },
    { name: 'replace',   detail: 'SIO::replace(字符串, 旧, 新) — 替换',                 doc: '示例: `SIO::replace("a-b", "-", "+")` → a+b' }
  ],
  Threads: [
    { name: 'spawn',  detail: 'Threads::spawn("方法名", 参数...) — 创建线程，返回 id', doc: '示例: `ALL t = Threads::spawn("factorial", 10).res;`' },
    { name: 'yield',  detail: 'Threads::yield() — 让出 CPU，调度其他线程', doc: '协作式线程切换' },
    { name: 'join',   detail: 'Threads::join(线程id) — 等待线程完成并取回结果', doc: '示例: `ALL r = Threads::join(t);` → r.res' },
    { name: 'active', detail: 'Threads::active() — 存活线程数', doc: '示例: `Threads::active().res`' },
    { name: 'self',   detail: 'Threads::self() — 当前线程 id（主线程 0）', doc: '示例: `Threads::self().res`' }
  ],
  Taskm: [
    { name: 'add',      detail: 'Taskm::add("方法名", 参数...) — 注册任务，返回任务 id', doc: '示例: `ALL t = Taskm::add("jobA", 5).res;`' },
    { name: 'interval', detail: 'Taskm::interval(毫秒) — 设置轮转间隔（默认 0）', doc: '示例: `Taskm::interval(10);`' },
    { name: 'run',      detail: 'Taskm::run() — 自动调度循环：轮流跑所有任务直到完成', doc: '自动切换线程完成线程循环' },
    { name: 'stop',     detail: 'Taskm::stop() — 停止调度循环', doc: '线程内调用后让出即可' },
    { name: 'active',   detail: 'Taskm::active() — 未完成任务数', doc: '示例: `Taskm::active().res`' }
  ],
  Arrays: [
    { name: 'count',   detail: 'Arrays::count() — 注册的 Array/Vector 实例数', doc: '示例: `Arrays::count().res`' },
    { name: 'all',     detail: 'Arrays::all() — 全部实例（数组的数组）', doc: '示例: `ALL xs = Arrays::all().res;`' },
    { name: 'get',     detail: 'Arrays::get(索引) — 第 i 个实例', doc: '示例: `Arrays::get(0).res`' },
    { name: 'add',     detail: 'Arrays::add(数组对象) — 注册实例（Array/Vector __init__ 调用）', doc: 'new 一个 Array 默认在 Arrays 里插入' },
    { name: 'vector',  detail: 'Arrays::vector() — 动态数组（new Vector，可自由 push 扩容）', doc: '示例: `ALL v = Arrays::vector(); v::push(10);`' },
    { name: 'forget',  detail: 'Arrays::forget(数组对象) — 从注册表移除', doc: '示例: `Arrays::forget(v);`' }
  ],
  Solid: [
    { name: 'new',       detail: 'Solid::new() — 新建连续流（连续存储 + 移动头指针）', doc: 'Array/Vector 类的底层存储' },
    { name: 'len',       detail: 'Solid::len(流) — 剩余长度（头指针到末尾）', doc: '示例: `Solid::len(s).res`' },
    { name: 'get',       detail: 'Solid::get(流, 索引) — 相对头指针取元素', doc: '示例: `Solid::get(s, 0).res`' },
    { name: 'set',       detail: 'Solid::set(流, 索引, 值) — 相对头指针设元素', doc: '示例: `Solid::set(s, 0, 42);`' },
    { name: 'push',      detail: 'Solid::push(流, 值) — 末尾追加', doc: '示例: `Solid::push(s, 10);`' },
    { name: 'pop',       detail: 'Solid::pop(流) — 弹出末尾', doc: '示例: `Solid::pop(s).res`' },
    { name: 'read',      detail: 'Solid::read(流) — 读头指针处并前进（移动头指针）', doc: '示例: `Solid::read(s).res`' },
    { name: 'peek',      detail: 'Solid::peek(流) — 读头指针处，不移动', doc: '示例: `Solid::peek(s).res`' },
    { name: 'head',      detail: 'Solid::head(流) — 当前头指针位置', doc: '示例: `Solid::head(s).res`' },
    { name: 'resetHead', detail: 'Solid::resetHead(流) — 头指针归零', doc: '示例: `Solid::resetHead(s);`' },
    { name: 'clear',     detail: 'Solid::clear(流) — 清空', doc: '示例: `Solid::clear(s);`' },
    { name: 'join',      detail: 'Solid::join(流, 分隔符?) — 剩余数据连接为字符串', doc: '示例: `Solid::join(s, "-").res`' }
  ],
  Ref: [
    { name: 'read',   detail: 'Ref::read(引用) — 读引用目标（r/rw/m 权限）', doc: '示例: `ALL v = Ref::read(rr).res;`' },
    { name: 'write',  detail: 'Ref::write(引用, 值) — 写引用目标（w/rw/m 权限）', doc: '示例: `Ref::write(wr, 10);`' },
    { name: 'move',   detail: 'Ref::move(引用) — 取走目标（仅 m 可移动权限）', doc: '示例: `ALL taken = Ref::move(mv);` → 目标被冲走' },
    { name: 'target', detail: 'Ref::target(引用) — 目标真名', doc: '示例: `Ref::target(rr).res` → counter' },
    { name: 'perm',   detail: 'Ref::perm(引用) — 权限（r/w/rw/m）', doc: '示例: `Ref::perm(rr).res`' }
  ],
  Com: [
    { name: 'abs',    detail: 'Com::abs(x) — 绝对值', doc: '示例: `Com::abs(0-5).res` → 5' },
    { name: 'min',    detail: 'Com::min(x, y) — 取较小', doc: '示例: `Com::min(3, 5).res` → 3' },
    { name: 'max',    detail: 'Com::max(x, y) — 取较大', doc: '示例: `Com::max(3, 5).res` → 5' },
    { name: 'pow',    detail: 'Com::pow(x, y) — x 的 y 次方', doc: '示例: `Com::pow(2, 10).res` → 1024' },
    { name: 'sqrt',   detail: 'Com::sqrt(x) — 平方根（负数 → 拒绝）', doc: '示例: `Com::sqrt(9).res` → 3' },
    { name: 'floor',  detail: 'Com::floor(x) — 向下取整', doc: '示例: `Com::floor(2.7).res` → 2' },
    { name: 'ceil',   detail: 'Com::ceil(x) — 向上取整', doc: '示例: `Com::ceil(2.1).res` → 3' },
    { name: 'round',  detail: 'Com::round(x) — 四舍五入', doc: '示例: `Com::round(2.5).res` → 3' },
    { name: 'sign',   detail: 'Com::sign(x) — 符号（-1/0/1）', doc: '示例: `Com::sign(0-3).res` → -1' },
    { name: 'sin',    detail: 'Com::sin(x) — 正弦', doc: '示例: `Com::sin(0).res` → 0' },
    { name: 'cos',    detail: 'Com::cos(x) — 余弦', doc: '示例: `Com::cos(0).res` → 1' },
    { name: 'tan',    detail: 'Com::tan(x) — 正切', doc: '示例: `Com::tan(0).res` → 0' },
    { name: 'log',    detail: 'Com::log(x) — 自然对数（x≤0 → 拒绝）', doc: '示例: `Com::log(1).res` → 0' },
    { name: 'exp',    detail: 'Com::exp(x) — e 的 x 次方', doc: '示例: `Com::exp(1).res` → e' }
  ],
  Time: [
    { name: 'now',      detail: 'Time::now() — 单调时钟秒', doc: '示例: `Time::now().res`' },
    { name: 'sleep',    detail: 'Time::sleep(毫秒) — 睡眠', doc: '示例: `Time::sleep(100);`' },
    { name: 'start',    detail: 'Time::start(?) — 启动计时器（默认第一个归线程所有）', doc: '示例: `Time::start();` → 线程首计时器，不允许归零' },
    { name: 'fork',     detail: 'Time::fork() — 分叉新计时器（允许归零）', doc: '示例: `ALL t = Time::fork(); Time::reset(t.res);`' },
    { name: 'elapsed',  detail: 'Time::elapsed(?) — 计时器已过毫秒数', doc: '示例: `Time::elapsed().res`' },
    { name: 'reset',    detail: 'Time::reset(id?) — 归零（首计时器拒绝；分叉出的允许）', doc: '示例: `Time::reset(t.res);`' }
  ]
};
/* IOStream 聚合 CIO/FIO/SIO（按序分派）；Console 是 CIO 的预置分叉 */
STREAMS.IO = [...STREAMS.CIO, ...STREAMS.FIO, ...STREAMS.SIO];
STREAMS.Console = STREAMS.CIO;
const BUILTIN_STREAMS = ['CIO', 'FIO', 'SIO', 'IO', 'Com', 'Time', 'Solid', 'Arrays', 'Threads', 'Taskm', 'Ref', 'Console'];

/**
 * 找与 start 处 '{' 配对的 '}' 位置（跳过字符串/注释/字符字面量）
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
    if (c === "'") { i++; continue; }                 // 字符字面量粗略跳过
    if (c === '{') depth++;
    else if (c === '}') { depth--; if (depth === 0) return i; }
  }
  return -1;
}

/** 提取块内方法名（行首 void/int/float/double/string/char ... name(） */
function methodsIn(block) {
  const methods = new Set();
  for (const mm of block.matchAll(/\b(?:void|int|float|double|string|char)\s+([A-Za-z_]\w*)\s*\(/g)) methods.add(mm[1]);
  return methods;
}

/** 提取块内字段名（int x, y; / int[] a; / string s; / type T; T n; 泛型风格） */
function fieldsIn(block) {
  const fields = new Set();
  // 类型 名字[, 名字...];（名字后跟逗号或分号才算字段，跟 '(' 是方法）
  for (const mm of block.matchAll(/\b(?:int|float|double|string|char|[A-Z]\w*)\s*(?:\[\])?\s+([A-Za-z_]\w*)\s*(?=(?:,|;))/g))
    fields.add(mm[1]);
  // 逗号分隔的后续字段名：int x, y;
  for (const mm of block.matchAll(/,\s*([A-Za-z_]\w*)\s*(?=(?:,|;))/g))
    fields.add(mm[1]);
  return fields;
}

/**
 * 从文档文本解析声明的流及其成员（方法 + 字段）
 * 返回 { known: Set(签名流/类名), map: Map(实现名 → { methods, fields }) }
 */
function parseStreams(text) {
  const known = new Set();
  for (const m of text.matchAll(/\bStream\s+([A-Za-z_]\w*)/g)) known.add(m[1]);
  for (const m of text.matchAll(/\bClass\s+([A-Za-z_]\w*)/g)) known.add(m[1]);
  const map = new Map();
  const classNames = new Set();

  // 分叉: <签名流> <实现名> { void m() {...} }（用花括号平衡取真实块）
  for (const m of text.matchAll(/([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\{/g)) {
    const sig = m[1], impl = m[2];
    if (!known.has(sig)) continue;
    const braceAt = m.index + m[0].length - 1;
    const end = blockEnd(text, braceAt);
    if (end === -1) continue;
    const inner = text.slice(braceAt + 1, end);
    map.set(impl, { methods: methodsIn(inner), fields: fieldsIn(inner) });
  }
  // 签名流自身的签名方法/字段: Stream X { void hello(); int count; }
  for (const m of text.matchAll(/\bStream\s+([A-Za-z_]\w*)\s*\{/g)) {
    const braceAt = m.index + m[0].length - 1;
    const end = blockEnd(text, braceAt);
    if (end === -1) continue;
    const inner = text.slice(braceAt + 1, end);
    map.set(m[1], { methods: methodsIn(inner), fields: fieldsIn(inner) });
  }
  // 类声明: Class X { int x, y; void m() {...} }
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
  /* ---------- 补全：Qual:: → 方法；Qual: → 流名 ---------- */
  const provider = vscode.languages.registerCompletionItemProvider('biolang', {
    provideCompletionItems(doc, position) {
      const text = doc.getText();
      const before = text.slice(0, doc.offsetAt(position));
      const items = [];

      // 已输入 `流::` 或 `this::` → 提示方法 + 字段
      const m2 = before.match(/([A-Za-z_]\w*)::$/);
      if (m2) {
        const qual = m2[1];
        if (STREAMS[qual]) {
          for (const m of STREAMS[qual]) items.push(methodItem(m.name, m.detail));
          return items;
        }
        const { map } = parseStreams(text);
        if (qual === 'this') {
          // this:: → 聚合所有已声明类的成员（字段 + 方法）
          const agg = { methods: new Set(), fields: new Set() };
          for (const { methods, fields } of map.values()) {
            for (const n of methods) agg.methods.add(n);
            for (const n of fields) agg.fields.add(n);
          }
          for (const name of agg.fields) items.push(fieldItem(name, '类字段（this::属性）'));
          for (const name of agg.methods) items.push(methodItem(name, '类方法'));
          return items;
        }
        const mem = map.get(qual);
        if (mem) {
          for (const name of mem.fields) items.push(fieldItem(name, `流/类 ${qual} 的字段`));
          for (const name of mem.methods) items.push(methodItem(name, `流 ${qual} 的方法`));
          return items;
        }
        return items;
      }

      // 已输入 `流:`（单冒号）→ 提示流名（选中后自动补 ::）
      const m1 = before.match(/([A-Za-z_]\w*):$/);
      if (m1) {
        const { known, map } = parseStreams(text);
        const names = new Set([...BUILTIN_STREAMS, ...known, ...map.keys()]);
        for (const n of names) {
          const item = new vscode.CompletionItem(n, vscode.CompletionItemKind.Class);
          item.insertText = new vscode.SnippetString(`${n}::`);
          item.detail = BUILTIN_STREAMS.includes(n) ? '内置流' : '声明的流';
          items.push(item);
        }
      }

      // 已输入 `对象.` → 提示请求结果属性 + 类字段（v.x / r.res / r.cause）
      const dot = before.match(/([A-Za-z_]\w*)\.$/);
      if (dot) {
        items.push(fieldItem('res', '请求结果：值'));
        items.push(fieldItem('cause', '请求结果：拒绝原因'));
        const { map } = parseStreams(text);
        const seen = new Set();
        for (const { fields } of map.values())
          for (const n of fields) if (!seen.has(n)) { seen.add(n); items.push(fieldItem(n, '类字段（对象属性）')); }
        return items;
      }

      // 已输入 `new ` → 提示类名 + 数组类型（new int[n]）
      if (before.match(/new\s+$/)) {
        const { known } = parseStreams(text);
        const names = new Set([...BUILTIN_STREAMS, ...known]);
        for (const n of names) {
          const item = new vscode.CompletionItem(n, vscode.CompletionItemKind.Class);
          item.insertText = new vscode.SnippetString(`${n}($0)`);
          item.detail = '类实例化';
          items.push(item);
        }
        for (const t of ['int', 'float', 'double', 'string', 'char']) {
          const item = new vscode.CompletionItem(t, vscode.CompletionItemKind.Struct);
          item.insertText = new vscode.SnippetString(t + '[$1:n]');
          item.detail = '数组字面量 new int[n]';
          items.push(item);
        }
        return items;
      }

      // 已输入 `&`（智能引用起点）→ 提示权限 r/w/rw/m
      if (before.match(/&\s*$/)) {
        for (const p of ['r', 'w', 'rw', 'm']) {
          const item = new vscode.CompletionItem(p, vscode.CompletionItemKind.Keyword);
          item.insertText = new vscode.SnippetString(`${p} `);
          item.detail = `智能引用权限 ${p}`;
          item.documentation = '语法: &权限 跟随 真名，如 &r u counter（配 Ref::read/write/move）';
          items.push(item);
        }
        return items;
      }

      // 已输入 `&r `（权限后）→ 提示跟随层 u/f/a
      if (before.match(/&\s*(?:r|w|rw|m)\s+$/)) {
        for (const f of ['u', 'f', 'a']) {
          const item = new vscode.CompletionItem(f, vscode.CompletionItemKind.Keyword);
          item.insertText = new vscode.SnippetString(`${f} `);
          item.detail = `引用跟随层 ${f}`;
          item.documentation = 'u = 程序级(Unistream) · f = 方法级(Functionstream) · a = 作用域级(Areastream)';
          items.push(item);
        }
        return items;
      }
      return items;
    }
  }, ':', '&', '.', ' ');

  /* ---------- 运行命令 ---------- */
  /* Webview 交互式运行面板 HTML：输出区 + 输入框（输入直接进 CIO/stdin） */
  function panelHtml(fileName) {
    return `<!DOCTYPE html>
<html lang="zh">
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
    <input id="in" placeholder="输入 (Enter 发送到 CIO)…" autofocus>
    <button id="send" disabled>发送</button>
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
    if (!doc) { vscode.window.showWarningMessage('没有打开的文件'); return; }
    if (doc.languageId !== 'biolang') { vscode.window.showWarningMessage('当前文件不是 BioLang（.bl/.bio）'); return; }
    await doc.save();
    const fileName = doc.fileName;
    const panel = vscode.window.createWebviewPanel(
      'biolangRun', `BioLang — ${path.basename(fileName)}`, vscode.ViewColumn.One,
      { enableScripts: true, retainContextWhenHidden: true }
    );
    panel.webview.html = panelHtml(fileName);
    const cwd = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0]
      ? vscode.workspace.workspaceFolders[0].uri.fsPath : path.dirname(fileName);
    /* 交互式子进程：stdout/stderr → 面板，面板输入 → stdin（CIO） */
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

  const runDemo = vscode.commands.registerCommand('biolang.runDemo', () => {
    const out = vscode.window.createOutputChannel('BioLang');
    out.show(true);
    out.appendLine('$ bio');
    execFile('bio', [], {}, (err, stdout, stderr) => {
      if (stdout) out.append(stdout);
      if (stderr) out.append(stderr);
      if (err) out.appendLine(`[bio 退出码 ${err.code}]（确认 bio 在 PATH 中：~/.local/bin/bio）`);
    });
  });

  context.subscriptions.push(provider, runFile, runDemo);
}

function deactivate() {}

module.exports = { activate, deactivate };
