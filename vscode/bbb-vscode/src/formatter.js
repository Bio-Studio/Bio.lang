/* formatter.js — BBB document formatter.
 *
 * Formatting is deliberately conservative: it only normalizes indentation and
 * whitespace around operators/commas/keywords. Strings, characters and line
 * comments are passed through untouched, so formatting never changes program
 * semantics (verified against the interpreter's examples).
 *
 * Style (matching examples/):
 *   - 4-space indentation, one level per block
 *   - spaces around binary operators ( + - * / % = == != < > <= >= )
 *   - space after if/while/for and after commas
 *   - no space around :: or inside parens/brackets
 */

'use strict';

/* True while scanning inside a "..." string, '...' char, or // comment. */
function isInString(text, i) {
    let quote = null;            // '"' or "'" while inside a literal
    let lineComment = false;
    for (let j = 0; j < i; j++) {
        const c = text[j];
        if (lineComment) continue;
        if (quote) {
            if (c === '\\') { j++; continue; }
            if (c === quote) quote = null;
            continue;
        }
        if (c === '"' || c === "'") { quote = c; continue; }
        if (c === '/' && text[j + 1] === '/') { lineComment = true; j++; continue; }
    }
    return quote !== null || lineComment;
}

/* Normalize whitespace on a single (already trimmed) line. */
function formatLine(line) {
    let out = '';
    let i = 0;
    const n = line.length;
    let prev = '';               // last emitted non-space char
    while (i < n) {
        const c = line[i];
        const next = line[i + 1];

        if (c === '"' || c === "'") {
            /* Copy string/char literal verbatim. */
            const q = c;
            out += c;
            i++;
            while (i < n && line[i] !== q) {
                if (line[i] === '\\' && i + 1 < n) { out += line[i] + line[i + 1]; i += 2; continue; }
                out += line[i];
                i++;
            }
            if (i < n) { out += line[i]; i++; }
            prev = q;
            continue;
        }
        if (c === '/' && next === '/') {
            /* Line comment: copy the rest verbatim. */
            out += line.slice(i);
            break;
        }

        if (c === ' ' || c === '\t') { i++; continue; }

        const two = c + (next || '');
        const three = c + (next || '') + (line[i + 2] || '');

        if (c === '(' && (prev === 'if' || prev === 'while' || prev === 'for') &&
            !out.endsWith(' ')) {
            out += ' ';
        }

        /* Multi-char operators that never take spaces around them. */
        if (two === '::' || two === '++' || two === '--' || two === '&&' || two === '||') {
            out += two;
            i += 2;
            prev = two;
            continue;
        }
        /* Word operators inside strings were handled above; skip. */

        /* Punctuation: no space before; ';' separates statements (space after),
         * others take no space after. */
        if ('(),;[]'.includes(c)) {
            out += c;
            if (c === ';' && next !== ')') out += ' ';
            if (c === ',') out += ' ';
            i++;
            prev = c;
            continue;
        }
        if (c === '{' || c === '}') {
            /* Space before { (e.g. "Main {") unless after '('; space after
             * both braces for one-line blocks ({ x; }). */
            if (prev && prev !== '(' && prev !== ',' && !out.endsWith(' ') && !out.endsWith('{'))
                out += ' ';
            out += c + ' ';
            i++;
            prev = c;
            continue;
        }

        /* Binary operators: space both sides. */
        if ('=+-*/%<>!'.includes(c)) {
            if (three === '===' || three === '!==') {
                out += (prev && !out.endsWith(' ') ? ' ' : '') + three + ' ';
                i += 3;
                prev = three;
                continue;
            }
            if (two === '==' || two === '!=' || two === '<=' || two === '>=') {
                out += (prev && !out.endsWith(' ') ? ' ' : '') + two + ' ';
                i += 2;
                prev = two;
                continue;
            }
            /* Single-char operator. */
            out += (prev && !out.endsWith(' ') ? ' ' : '') + c + ' ';
            i++;
            prev = c;
            continue;
        }

        /* Keywords: space after if/while/for when followed by '('. */
        if (c === ')' && next === ' ' && line[i + 2] === '{') {
            out += ') ';
            i += 2;
            prev = ')';
            continue;
        }

        /* Identifiers/numbers/keywords. */
        if (/[A-Za-z0-9_]/.test(c)) {
            let j = i;
            while (j < n && /[A-Za-z0-9_]/.test(line[j])) j++;
            const word = line.slice(i, j);
            /* Word separated from a previous word/number (e.g. `int count`). */
            if (prev && /[A-Za-z0-9_]/.test(prev[prev.length - 1]) && !out.endsWith(' '))
                out += ' ';
            out += word;
            i = j;
            prev = word;
            continue;
        }

        if (c === '.') {
            /* Decimal point or property access: keep glued (2.5, h.hp). */
            out += c;
            i++;
            prev = c;
            continue;
        }

        /* Anything else: keep as-is with a space if needed. */
        out += (prev && !out.endsWith(' ') ? ' ' : '') + c;
        i++;
        prev = c;
    }

    /* Trim trailing whitespace only; the scanner never produces double
     * spaces outside string literals, and string content stays untouched. */
    out = out.replace(/ +$/g, '');
    return out;
}

/* Count leading/trailing braces that affect indentation (outside strings). */
function countBraces(text, atStart) {
    let count = 0;
    let quote = null;
    let comment = false;
    /* Trailing-pass: leading '}' already handled the dedent; skip them so a
     * line like `} else {` nets to +1 (open) and `}` alone nets to 0. */
    let i = 0;
    if (!atStart) while (i < text.length && text[i] === '}') i++;
    for (; i < text.length; i++) {
        const c = text[i];
        if (comment) break;
        if (quote) {
            if (c === '\\') { i++; continue; }
            if (c === quote) quote = null;
            continue;
        }
        if (c === '"' || c === "'") { quote = c; continue; }
        if (c === '/' && text[i + 1] === '/') { comment = true; continue; }
        if (c === '{' || c === '}') {
            if (atStart) {
                if (c === '}') count--;
                break;              /* only leading braces matter for dedent */
            } else if (c === '{') {
                count++;
            } else if (c === '}') {
                count--;
            }
        }
    }
    return count;
}

function formatBBB(text) {
    const lines = text.split('\n');
    const out = [];
    let indent = 0;
    for (const raw of lines) {
        const line = raw.trim();
        if (!line) { out.push(''); continue; }

        /* Dedent before emitting a line that starts with }. */
        const leading = countBraces(line, true);
        if (leading < 0) indent = Math.max(0, indent + leading);

        const formatted = formatLine(line);
        out.push('    '.repeat(indent) + formatted);

        /* Indent after a line that opens a block. */
        indent += countBraces(line, false);
    }
    /* Remove multiple trailing blank lines. */
    while (out.length > 1 && out[out.length - 1] === '') out.pop();
    return out.join('\n') + '\n';
}

module.exports = { formatBBB };
