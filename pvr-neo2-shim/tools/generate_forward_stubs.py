#!/usr/bin/env python3
import argparse
import json
import os


def generate(functions, hooks, stubs_path, vars_path):
    hooks_set = set(hooks)
    funcs = [f for f in functions if f not in hooks_set]

    with open(stubs_path, "w") as f:
        f.write("/* Auto-generated forward stubs. Do not edit by hand. */\n")
        f.write("    .text\n")
        for name in funcs:
            f.write(f"    .global {name}\n")
            f.write(f"    .type {name}, %function\n")
            f.write(f"{name}:\n")
            f.write(f"    adrp x16, {name}_orig\n")
            f.write(f"    ldr x16, [x16, :lo12:{name}_orig]\n")
            f.write(f"    br x16\n\n")

    with open(vars_path, "w") as f:
        f.write("/* Auto-generated forward symbol resolver. Do not edit by hand. */\n")
        f.write('#include <cstddef>\n')
        f.write('#include "loader.h"\n')
        f.write('#include "log.h"\n\n')
        f.write('extern "C" {\n')
        for name in funcs:
            f.write(f"    void* {name}_orig = nullptr;\n")
        f.write("}\n\n")
        f.write("namespace pvr_shim {\n\n")
        f.write("struct Symbol { const char* name; void** ptr; };\n")
        f.write("static const Symbol kSymbols[] = {\n")
        for name in funcs:
            f.write(f'    {{"{name}", &{name}_orig}},\n')
        f.write("};\n\n")
        f.write("void pvr_shim_resolve_forward_symbols() {\n")
        f.write("    for (const auto& s : kSymbols) {\n")
        f.write("        *s.ptr = resolve_original(s.name);\n")
        f.write("    }\n")
        f.write('    LOGI("resolved %zu forward symbols", sizeof(kSymbols)/sizeof(kSymbols[0]));\n')
        f.write("}\n\n")
        f.write("} // namespace pvr_shim\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--functions", required=True)
    parser.add_argument("--hooks", required=True)
    parser.add_argument("--stubs", required=True)
    parser.add_argument("--vars", required=True)
    args = parser.parse_args()

    with open(args.functions) as f:
        functions = json.load(f)
    with open(args.hooks) as f:
        hooks = json.load(f)

    os.makedirs(os.path.dirname(args.stubs), exist_ok=True)
    os.makedirs(os.path.dirname(args.vars), exist_ok=True)
    generate(functions, hooks, args.stubs, args.vars)


if __name__ == "__main__":
    main()
