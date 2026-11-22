"""TOML, as much of it as this repository's own files use, for a Python older than 3.11.

  import minitoml
  data = minitoml.loads(text)

Python has a TOML reader of its own since 3.11 (tomllib) and loads() uses it where it is there. macOS
still ships 3.9, and nothing here may ask for a package to be installed, so this file reads the same
files there: tables ([a], [a.b]), arrays of tables ([[a]]), keys with strings ("..." with the usual
escapes, '...'), integers (decimal, 0x, 0o, 0b, with _), true and false, and arrays of those, which may
run over several lines. Dates, floats, inline tables, multi-line strings and dotted keys are not read:
a file that uses one is refused with its line, never half understood. workbench/tools_test.py holds the
two readers against each other on every TOML file of the repository.
"""
import re

try:
    import tomllib as _tomllib
except ImportError:             # Python < 3.11
    _tomllib = None


class TOMLError(ValueError):
    pass


_KEY = re.compile(r'[A-Za-z0-9_-]+')
_INT = re.compile(r'[+-]?(0x[0-9A-Fa-f_]+|0o[0-7_]+|0b[01_]+|[0-9][0-9_]*)$')
_WORD = re.compile(r'[^\s,\]#]+')
_ESCAPES = {'b': '\b', 't': '\t', 'n': '\n', 'f': '\f', 'r': '\r', '"': '"', '\\': '\\'}


class _Reader:
    def __init__(self, text):
        self.text = text
        self.at = 0
        self.line = 1

    def fail(self, why):
        raise TOMLError(f'line {self.line}: {why}')

    def peek(self):
        return self.text[self.at] if self.at < len(self.text) else ''

    def blanks(self, lines=False):
        """Skip spaces and comments; with lines, the ends of lines as well."""
        while self.at < len(self.text):
            c = self.text[self.at]
            if c in ' \t' or (lines and c in '\r\n'):
                if c == '\n':
                    self.line += 1
                self.at += 1
            elif c == '#':
                while self.at < len(self.text) and self.text[self.at] != '\n':
                    self.at += 1
            else:
                break

    def end_of_line(self):
        self.blanks()
        if self.at >= len(self.text):
            return
        if self.text[self.at] == '\r':
            self.at += 1
        if self.peek() != '\n':
            self.fail(f'unexpected {self.text[self.at:self.at + 20]!r}')
        self.at += 1
        self.line += 1

    def key(self):
        if self.peek() in ('"', "'"):
            return self.string()
        m = _KEY.match(self.text, self.at)
        if not m:
            self.fail('a key was expected')
        self.at = m.end()
        return m.group(0)

    def string(self):
        quote = self.text[self.at]
        if self.text.startswith(quote * 3, self.at):
            self.fail('multi-line strings are not read here')
        self.at += 1
        out = []
        while True:
            c = self.peek()
            if c in ('', '\n'):
                self.fail('a string without its end')
            self.at += 1
            if c == quote:
                return ''.join(out)
            if c == '\\' and quote == '"':
                e = self.peek()
                self.at += 1
                if e in _ESCAPES:
                    out.append(_ESCAPES[e])
                elif e in ('u', 'U'):
                    n = 4 if e == 'u' else 8
                    digits = self.text[self.at:self.at + n]
                    if len(digits) != n or not re.fullmatch('[0-9A-Fa-f]+', digits):
                        self.fail('a bad \\u escape')
                    out.append(chr(int(digits, 16)))
                    self.at += n
                else:
                    self.fail(f'an unknown escape \\{e}')
            else:
                out.append(c)

    def value(self):
        c = self.peek()
        if c in ('"', "'"):
            return self.string()
        if c == '[':
            self.at += 1
            items = []
            while True:
                self.blanks(lines=True)
                if self.peek() == ']':
                    self.at += 1
                    return items
                if self.peek() == '':
                    self.fail('an array without its end')
                items.append(self.value())
                self.blanks(lines=True)
                if self.peek() == ',':
                    self.at += 1
                elif self.peek() != ']':
                    self.fail('a , or a ] was expected in an array')
        if c == '{':
            self.fail('inline tables are not read here')
        m = _WORD.match(self.text, self.at)
        if not m:
            self.fail('a value was expected')
        word = m.group(0)
        self.at = m.end()
        if word == 'true':
            return True
        if word == 'false':
            return False
        if _INT.match(word):
            return int(word.replace('_', ''), 0)
        self.fail(f'{word!r} is not a value this reader knows (a string, an integer, true, false, an array)')


def _loads(text):
    r = _Reader(text)
    root = {}
    table = root
    while True:
        r.blanks(lines=True)
        if r.at >= len(r.text):
            return root
        if r.peek() == '[':
            array = r.text.startswith('[[', r.at)
            r.at += 2 if array else 1
            names = []
            while True:
                r.blanks()
                names.append(r.key())
                r.blanks()
                if r.peek() == '.':
                    r.at += 1
                else:
                    break
            if not r.text.startswith(']]' if array else ']', r.at):
                r.fail('a table header without its end')
            r.at += 2 if array else 1
            table = root
            for name in names[:-1]:
                table = table.setdefault(name, {})
                if isinstance(table, list):
                    table = table[-1]
                if not isinstance(table, dict):
                    r.fail(f'{name} is not a table')
            last = names[-1]
            if array:
                rows = table.setdefault(last, [])
                if not isinstance(rows, list):
                    r.fail(f'{last} is not an array of tables')
                rows.append({})
                table = rows[-1]
            else:
                if last in table and not isinstance(table[last], dict):
                    r.fail(f'{last} is defined twice')
                table = table.setdefault(last, {})
            r.end_of_line()
            continue
        name = r.key()
        r.blanks()
        if r.peek() == '.':
            r.fail('dotted keys are not read here')
        if r.peek() != '=':
            r.fail(f'= was expected after {name}')
        r.at += 1
        r.blanks()
        if name in table:
            r.fail(f'{name} is defined twice')
        table[name] = r.value()
        r.end_of_line()


def loads(text):
    if _tomllib is not None:
        return _tomllib.loads(text)
    return _loads(text)


def loads_own(text):
    """This file's own reader, whatever the Python: for the test that holds it against tomllib."""
    return _loads(text)
