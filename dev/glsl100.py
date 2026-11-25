#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys

DEFAULT = ["ground.fs", "wall.vs", "wall.fs", "sprite.fs"]
HERE = os.path.dirname(os.path.abspath(__file__))
SHADERS = os.path.join(HERE, "..", "packages", "engine", "shaders")

PRECISION = """#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
"""


def convert(src, vertex):
    lines = src.split("\n")
    assert lines[0].strip() == "#version 330", "expected '#version 330' on the first line"
    uses_derivatives = re.search(r"\b(dFdx|dFdy|fwidth)\b", src) is not None
    head = ["#version 100"]
    if uses_derivatives:
        head += ["#ifdef GL_OES_standard_derivatives", "#extension GL_OES_standard_derivatives : enable", "#endif"]
    if not vertex:
        head.append(PRECISION.rstrip("\n"))
    out = []
    for line in lines[1:]:
        if vertex:
            line = re.sub(r"^in\s", "attribute ", line)
            line = re.sub(r"^out\s", "varying ", line)
        else:
            if re.match(r"^out\s+vec4\s+finalColor\s*;", line):
                continue
            line = re.sub(r"^in\s", "varying ", line)
            line = line.replace("finalColor", "gl_FragColor")
        line = re.sub(r"\btexture\(", "texture2D(", line)
        line = re.sub(r"\bivec2 (\w+) = ivec2\((.*)\);", r"vec2 \1 = \2;", line)
        line = re.sub(r"\btexelFetch\((\w+),\s*(\w+),\s*0\)", r"texture2D(\1, (\2 + 0.5) / mapSize)", line)
        m = re.match(r"^(\s*)float (\w+) = (.*\b(dFdx|dFdy|fwidth)\b.*);(.*)$", line)
        if m:
            indent, name = m.group(1), m.group(2)
            out += ["#ifdef GL_OES_standard_derivatives", line, "#else",
                    f"{indent}float {name} = 1.0;", "#endif"]
            continue
        out.append(line)
    body = "\n".join(out)
    assert "texelFetch" not in body and "finalColor" not in body, "left something 3.30-only"
    return "\n".join(head) + "\n// Generated from glsl330 by dev/glsl100.py: edit that file and re-run.\n" + body


def main(paths):
    if not paths:
        paths = [os.path.join(SHADERS, "glsl330", n) for n in DEFAULT]
    validator = shutil.which("glslangValidator")
    failed = 0
    for p in paths:
        p = os.path.abspath(p)
        dst_dir = os.path.join(os.path.dirname(os.path.dirname(p)), "glsl100")
        dst = os.path.join(dst_dir, os.path.basename(p))
        vertex = p.endswith(".vs")
        with open(p) as f:
            out = convert(f.read(), vertex)
        os.makedirs(dst_dir, exist_ok=True)
        with open(dst, "w") as f:
            f.write(out)
        status = "written"
        if validator:
            r = subprocess.run([validator, "-S", "vert" if vertex else "frag", dst], capture_output=True, text=True)
            status = "ok" if r.returncode == 0 else "INVALID\n" + r.stdout
            failed += r.returncode != 0
        print(f"{os.path.relpath(dst)}: {status}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
