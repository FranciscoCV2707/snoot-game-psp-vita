"""Utilidades l?xicas compartidas por el exportador Ren'Py."""

import ast
import re

STRING_RE = re.compile(r""""((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)'""")

def strip_comment(line):
    quote = None
    i = 0
    while i < len(line):
        c = line[i]
        if quote:
            if c == "\\":
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == "#":
            return line[:i].rstrip()
        i += 1
    return line.rstrip()

def bracket_balance(line):
    code = STRING_RE.sub('""', line)
    return sum(code.count(c) for c in "([{") - sum(code.count(c) for c in ")]}")

def extract_literal(text, name):
    m = re.search(rf"define\s+{name}\s*=\s*", text)
    if not m:
        return None
    start = m.end()
    opening = text[start]
    closing = {"(": ")", "{": "}", "[": "]"}[opening]
    depth = 0
    for i in range(start, len(text)):
        if text[i] == opening:
            depth += 1
        elif text[i] == closing:
            depth -= 1
            if depth == 0:
                body = "\n".join(strip_comment(ln) for ln in text[start:i + 1].splitlines())
                return ast.literal_eval(body)
    return None
