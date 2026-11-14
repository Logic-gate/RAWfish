#!/usr/bin/env python3
"""scripts/qmlcheck.py -- run by check-host.sh on the app's QML files.

Sailfish OS runs Qt 5.6, whose JavaScript is ES5: a QML file using later
syntax loads on a PC (Qt 5.15's qmllint accepts it) and fails on the phone.
This looks, in the code only (comments and strings skipped), for the ES6
forms most likely to slip in:
  - let / const declarations,
  - arrow functions (=>),
  - template strings (`...`).
Prints one line per finding (file:line: what) and exits 1 if any.

    qmlcheck.py FILE.qml...          check the files
    qmlcheck.py --self-test          check a bad sample (must find 3)
"""
import re
import sys

DECL = re.compile(r'(?<![A-Za-z0-9_$.])(let|const)\s+[A-Za-z_$\[{]')


def code_only(text):
    """The text with comments and string contents blanked (newlines kept, so
    line numbers stay). Returns (code, list of line numbers of template
    strings)."""
    out, backticks = [], []
    i, n, line = 0, len(text), 1
    state = None                         # None, '//', '/*', '"', "'"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if c == '\n':
            line += 1
            if state == '//':
                state = None
            out.append('\n')
            i += 1
            continue
        if state is None:
            if c == '/' and nxt == '/':
                state = '//'; out.append('  '); i += 2; continue
            if c == '/' and nxt == '*':
                state = '/*'; out.append('  '); i += 2; continue
            if c in '"\'':
                state = c; out.append(c); i += 1; continue
            if c == '`':
                backticks.append(line)
            out.append(c); i += 1; continue
        if state == '/*':
            if c == '*' and nxt == '/':
                state = None; out.append('  '); i += 2; continue
            out.append(' '); i += 1; continue
        if state == '//':
            out.append(' '); i += 1; continue
        # inside a string
        if c == '\\' and i + 1 < n:
            out.append('  ' if text[i + 1] != '\n' else ' \n')
            if text[i + 1] == '\n':
                line += 1
            i += 2
            continue
        if c == state:
            state = None
        out.append(c if state is None else ' ')
        i += 1
    return ''.join(out), backticks


def check(name, text):
    code, backticks = code_only(text)
    found = []
    for no, l in enumerate(code.split('\n'), 1):
        m = DECL.search(l)
        if m:
            found.append('%s:%d: "%s" is ES6 (Qt 5.6 on the phone: use var)' % (name, no, m.group(1)))
        if '=>' in l:
            found.append('%s:%d: an arrow function (=>) is ES6 (Qt 5.6 on the phone: use function)' % (name, no))
    for no in sorted(set(backticks)):
        found.append('%s:%d: a template string (`) is ES6 (Qt 5.6 on the phone: use + and quotes)' % (name, no))
    return found


SAMPLE = '''import QtQuick 2.0
// let x = 1, a comment: fine
/* const y = 2; in a block comment: fine */
Item {
    property string s: "let me => see `this`"
    function f() {
        let a = 1
        var g = (b) => b + 1
        return `x${a}`
    }
}
'''


def main(argv):
    if argv[1:] == ['--self-test']:
        n = len(check('sample.qml', SAMPLE))
        if n != 3:
            print('qmlcheck.py: its self-test found %d of 3 faults' % n)
            return 1
        return 0
    bad = 0
    for f in argv[1:]:
        with open(f, encoding='utf-8') as h:
            for line in check(f, h.read()):
                print(line)
                bad = 1
    return bad


if __name__ == '__main__':
    sys.exit(main(sys.argv))
