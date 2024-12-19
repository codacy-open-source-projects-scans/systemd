#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# pylint: disable=consider-using-with

import ast
import os
import re
import sys

import jinja2

def parse_config_h(filename):
    # Parse config.h file generated by meson.
    ans = {}
    for line in open(filename):
        m = re.match(r'#define\s+(\w+)\s+(.*)', line)
        if not m:
            continue
        a, b = m.groups()
        # The function ast.literal_eval() cannot evaluate octal integers, e.g. 0600.
        # So, it is intentional that the string below does not contain '0'.
        if b and (b[0] in '123456789"' or b == '0'):
            b = ast.literal_eval(b)
        ans[a] = b
    return ans

def render(filename, defines):
    text = open(filename).read()
    template = jinja2.Template(text,
                               trim_blocks=True,
                               lstrip_blocks=True,
                               keep_trailing_newline=True,
                               undefined=jinja2.StrictUndefined)
    return template.render(defines)

def main():
    defines = parse_config_h(sys.argv[1])
    output = render(sys.argv[2], defines)
    with open(sys.argv[3], 'w') as f:
        f.write(output)
    info = os.stat(sys.argv[2])
    os.chmod(sys.argv[3], info.st_mode)

if __name__ == '__main__':
    main()
