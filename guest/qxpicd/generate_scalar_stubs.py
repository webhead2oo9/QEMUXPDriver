#!/usr/bin/env python3
"""Generate ABI-correct pointer-free OpenGL 1.1 FIFO stubs."""

import os
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
GLFUNCS = HERE / "third_party/reactos/glfuncs.h"
OUTPUT = HERE / "generated_gl_scalar.c"

WORD_TYPES = {
    "GLenum", "GLboolean", "GLbitfield", "GLbyte", "GLshort", "GLint",
    "GLsizei", "GLubyte", "GLushort", "GLuint",
}
FLOAT_TYPES = {"GLfloat", "GLclampf"}
DOUBLE_TYPES = {"GLdouble", "GLclampd"}
RETURN_TYPES = {"GLboolean", "GLenum", "GLint", "GLuint"}


def balanced_calls(text: str, prefix: str):
    start = 0
    needle = prefix + "("
    while True:
        pos = text.find(needle, start)
        if pos < 0:
            return
        depth = 0
        for end in range(pos + len(prefix), len(text)):
            char = text[end]
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    yield text[pos + len(prefix) + 1:end]
                    start = end + 1
                    break
        else:
            raise ValueError(f"unterminated {prefix} at byte {pos}")


def split_top(value: str):
    result, depth, begin = [], 0, 0
    for index, char in enumerate(value):
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(value[begin:index].strip())
            begin = index + 1
    result.append(value[begin:].strip())
    return result


def parse_parameters(prototype: str):
    if not prototype.startswith("(") or not prototype.endswith(")"):
        return None
    body = prototype[1:-1].strip()
    if body == "void":
        return []
    parameters = []
    for raw in split_top(body):
        if "*" in raw or "[" in raw:
            return None
        match = re.fullmatch(r"(.+?)\s+([A-Za-z_]\w*)", raw)
        if not match:
            return None
        type_name, name = match.groups()
        if type_name not in WORD_TYPES | FLOAT_TYPES | DOUBLE_TYPES:
            return None
        parameters.append((type_name, name))
    return parameters


def parse_raw_parameters(prototype: str):
    if not prototype.startswith("(") or not prototype.endswith(")"):
        return None
    body = prototype[1:-1].strip()
    if body == "void":
        return []
    parameters = []
    for raw in split_top(body):
        pointer = re.fullmatch(
            r"(?:const\s+)?([A-Za-z_]\w*)\s*\*\s*([A-Za-z_]\w*)", raw)
        if pointer:
            parameters.append((pointer.group(1), pointer.group(2), True))
            continue
        scalar = re.fullmatch(r"([A-Za-z_]\w*)\s+([A-Za-z_]\w*)", raw)
        if not scalar:
            return None
        parameters.append((scalar.group(1), scalar.group(2), False))
    return parameters


def fixed_vector_components(name: str):
    match = re.fullmatch(
        r"(?:Color|Vertex|TexCoord|RasterPos)([1234])(?:ub|ui|us|[bdfis])v",
        name)
    if match:
        return int(match.group(1))
    match = re.fullmatch(r"Normal3[bdfis]v", name)
    if match:
        return 3
    if re.fullmatch(r"Index(?:ub|[dfis])v", name):
        return 1
    match = re.fullmatch(r"EvalCoord([12])[df]v", name)
    if match:
        return int(match.group(1))
    if name == "EdgeFlagv":
        return 1
    if re.fullmatch(r"(?:Load|Mult)Matrix[df]", name):
        return 16
    if name == "ClipPlane":
        return 4
    if name == "PolygonStipple":
        return 128
    return None


def main():
    # The paired core's Mesa pass-through table; the Makefile passes
    # QEMU_DIR (guest/paired-core.mk).
    if "QEMU_DIR" not in os.environ:
        raise SystemExit("generate_scalar_stubs.py: set QEMU_DIR to the "
                         "paired qemu-libretro core (make passes it)")
    gl_text = GLFUNCS.read_text(encoding="utf-8")
    enum_text = (Path(os.environ["QEMU_DIR"]) / "hw/mesa/mglfunci.h").read_text(
        encoding="utf-8")
    entries = []
    for call in balanced_calls(gl_text, "USE_GL_FUNC"):
        fields = split_top(call)
        if len(fields) != 5:
            continue
        name, prototype, _arguments, offset, _stack = fields
        parameters = parse_parameters(prototype)
        if parameters is None or f"FEnum_gl{name}" not in enum_text:
            continue
        entries.append((name, prototype, int(offset), parameters))

    return_entries = []
    for call in balanced_calls(gl_text, "USE_GL_FUNC_RET"):
        fields = split_top(call)
        if len(fields) != 6:
            continue
        name, return_type, prototype, _arguments, offset, _stack = fields
        parameters = parse_parameters(prototype)
        if (parameters is None or return_type not in RETURN_TYPES or
                f"FEnum_gl{name}" not in enum_text):
            continue
        return_entries.append(
            (name, return_type, prototype, int(offset), parameters))

    vector_entries = []
    for call in balanced_calls(gl_text, "USE_GL_FUNC"):
        fields = split_top(call)
        if len(fields) != 5:
            continue
        name, prototype, _arguments, offset, _stack = fields
        components = fixed_vector_components(name)
        parameters = parse_raw_parameters(prototype)
        if (components is None or parameters is None or
                sum(1 for _type, _name, pointer in parameters if pointer) != 1 or
                f"FEnum_gl{name}" not in enum_text):
            continue
        vector_entries.append(
            (name, prototype, int(offset), parameters, components))

    lines = [
        "/* Generated by generate_scalar_stubs.py. Do not edit. */",
        "#include <windows.h>",
        "#include <GL/gl.h>",
        "#include <stdint.h>",
        "#include <string.h>",
        "#include \"transport.h\"",
        "#include \"mglfunci.h\"",
        "",
        "static uint32_t scalar_float_word(GLfloat value)",
        "{",
        "    uint32_t word;",
        "    memcpy(&word, &value, sizeof(word));",
        "    return word;",
        "}",
        "",
        "static void scalar_double_words(uint32_t *words, GLdouble value)",
        "{",
        "    memcpy(words, &value, sizeof(value));",
        "}",
        "",
    ]
    for name, prototype, _offset, parameters in entries:
        words = sum(2 if type_name in DOUBLE_TYPES else 1
                    for type_name, _ in parameters)
        lines.append(f"static void APIENTRY Scalar{name}{prototype}")
        lines.append("{")
        if words:
            lines.append(f"    uint32_t arguments[{words}];")
            cursor = 0
            for type_name, parameter in parameters:
                if type_name in DOUBLE_TYPES:
                    lines.append(
                        f"    scalar_double_words(&arguments[{cursor}], {parameter});")
                    cursor += 2
                elif type_name in FLOAT_TYPES:
                    lines.append(
                        f"    arguments[{cursor}] = scalar_float_word({parameter});")
                    cursor += 1
                else:
                    lines.append(f"    arguments[{cursor}] = (uint32_t){parameter};")
                    cursor += 1
            lines.append(
                f"    qxp_transport_submit(FEnum_gl{name}, arguments, {words});")
        else:
            lines.append(f"    qxp_transport_submit(FEnum_gl{name}, NULL, 0);")
        lines.append("}")
        lines.append("")

    for name, return_type, prototype, _offset, parameters in return_entries:
        words = sum(2 if type_name in DOUBLE_TYPES else 1
                    for type_name, _ in parameters)
        lines.append(f"static {return_type} APIENTRY Scalar{name}{prototype}")
        lines.append("{")
        if words:
            lines.append(f"    uint32_t arguments[{words}];")
            cursor = 0
            for type_name, parameter in parameters:
                if type_name in DOUBLE_TYPES:
                    lines.append(
                        f"    scalar_double_words(&arguments[{cursor}], {parameter});")
                    cursor += 2
                elif type_name in FLOAT_TYPES:
                    lines.append(
                        f"    arguments[{cursor}] = scalar_float_word({parameter});")
                    cursor += 1
                else:
                    lines.append(f"    arguments[{cursor}] = (uint32_t){parameter};")
                    cursor += 1
            argument_expr = "arguments"
        else:
            argument_expr = "NULL"
        lines.append(
            f"    return ({return_type})qxp_transport_call_return("
            f"FEnum_gl{name}, {argument_expr}, {words});")
        lines.append("}")
        lines.append("")

    for name, prototype, _offset, parameters, components in vector_entries:
        pointer_name = next(parameter_name for _type, parameter_name, pointer
                            in parameters if pointer)
        lines.append(f"static void APIENTRY Vector{name}{prototype}")
        lines.append("{")
        lines.append(f"    uint32_t arguments[{len(parameters)}];")
        for index, (_type, parameter_name, _pointer) in enumerate(parameters):
            lines.append(
                f"    arguments[{index}] = (uint32_t)(uintptr_t){parameter_name};")
        lines.append(f"    if ({pointer_name}) qxp_transport_submit_input(")
        lines.append(
            f"        FEnum_gl{name}, arguments, {len(parameters)}, {pointer_name},")
        if name == "PolygonStipple":
            lines.append(f"        {components});")
        else:
            lines.append(
                f"        {components}U * sizeof(*{pointer_name}));")
        lines.append("}")
        lines.append("")

    lines += [
        "void qxp_install_scalar_stubs(PROC *entries)",
        "{",
    ]
    for name, _prototype, offset, _parameters in entries:
        lines.append(f"    entries[{offset}] = (PROC)Scalar{name};")
    for name, _return_type, _prototype, offset, _parameters in return_entries:
        lines.append(f"    entries[{offset}] = (PROC)Scalar{name};")
    for name, _prototype, offset, _parameters, _components in vector_entries:
        lines.append(f"    entries[{offset}] = (PROC)Vector{name};")
    lines += ["}", ""]
    OUTPUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"generated {len(entries)} void scalar, {len(return_entries)} return "
          f"scalar, and {len(vector_entries)} fixed-vector OpenGL 1.1 stubs")


if __name__ == "__main__":
    main()
