"""Mechanical half of the parser's port from C++ to C.

Reads the method bodies out of cc/gen.cpp and turns them into C: methods become
`gen_foo(generator* g, ...)`, members and method calls get the `g` they now
need, C++ references become pointers - in the signature, in the body and at
every call site - and a default argument becomes an explicit one.

What it deliberately does not touch: std::string, std::vector, std::map,
lambdas and throw. Those need decisions, not substitutions, and the compiler
lists every one of them.

    python tools/port-gen.py --reset    put gen.c back to its hand-written prelude
    python tools/port-gen.py            append the translated body
"""

import re
import sys

SOURCE = "cc/gen.cpp"
TARGET = "cc/gen.c"
SENTINEL = "/* PORT: the rest of the file is appended here */"

MEMBERS = [
    "tokens_", "options_", "target_", "e_", "data_fixups_", "data_", "types_", "records_",
    "signatures_", "symbols_", "strings_", "function_fixups_", "labels_", "pending_gotos_",
    "break_labels_", "continue_labels_", "switches_", "at_", "scope_", "frame_patch_", "frame_",
    "prologue_slots_", "body_start_", "epilogue_slots_", "stack_patch_", "paragraph_patch_",
    "return_type_", "hoisted_", "current_function_", "inline_returns_", "inline_stack_",
    "helper_label_", "t_void_", "t_char_", "t_uchar_", "t_int_", "t_uint_", "t_long_",
    "t_ulong_", "t_real_",
]

# Written by hand in the prelude: (argument count, defaults, which are pointers).
PRELUDE = {
    "gen_reserve_data": (3, [None, None, "2"], []),
    "gen_settled": (2, [None, None], [1]),
    "gen_fits_cx": (2, [None, None], [1]),
    "gen_fits_cx_bx": (2, [None, None], [1]),
    "gen_add_address_to_ax": (2, [None, None], [1]),
    "gen_discard": (2, [None, None], []),
    "gen_emitted_since": (2, [None, None], []),
    "make_deref": (2, [None, None], [0]),
    "in_memory": (1, [None], [0]),
    "source_of": (1, [None], [0]),
}

METHODS = [
    "run", "finish", "report", "helper_label", "fail", "kind", "advance", "accept", "expect",
    "add_type", "type_size", "type", "is_pointer", "is_array", "is_record", "is_scalar_pointer",
    "is_unsigned", "is_wide", "is_real", "target_of", "pointer_to", "array_of", "function_type",
    "is_function_type", "is_function_pointer", "signature_of", "label_of", "find", "define",
    "leave_scope", "reserve_data", "add_string", "data_reference", "data_pointer", "mark",
    "discard", "emitted_since", "load_address", "load_byte_widened", "load_converted",
    "load_to_cx_bx", "load_to_cx", "load", "store_indirect", "store", "address_to_bx",
    "test_value", "apply_operator_wide_constant", "apply_operator_wide", "apply_operator_constant",
    "apply_operator", "shift_ax_constant", "shift_wide_constant", "scale_ax", "widen",
    "call_helper", "emit_helpers", "settled", "fits_cx_bx", "fits_cx", "add_address_to_ax",
    "single_operand_ahead", "skip_qualifiers", "is_type_start", "parse_type_specifier",
    "parse_declarator_suffix", "parse_declarator", "parse_record", "parse_enum",
    "constant_expression", "declaration", "function_body", "statement", "compound_statement",
    "local_declaration", "global_initialiser", "initialiser_length", "address_constant",
    "address_operand", "address_offset", "for_statement", "switch_statement", "emit_return",
    "in_place_statement", "place_of", "memory_operation", "operate_from_memory", "promote_locals",
    "fold_address_copies", "live_registers", "expression", "assignment_expression",
    "conditional_expression", "binary_expression", "unary_expression", "postfix_expression",
    "primary_expression", "indirect_call", "can_inline", "assigned_in_body", "inline_call",
    "inline_arguments", "push_arguments", "call", "emit_runtime", "emit_interrupt_call",
    "emit_startup", "drop_unreachable",
]

ACCESSORS = {
    "symbols_": "SYMBOL_AT(g, %s)",
    "types_": "TYPE_AT(g, %s)",
    "records_": "RECORD_AT(g, %s)",
    "data_": "DATA_AT(g, %s)",
}


def body_of(text):
    at = text.find("\t// Code generation")
    if at < 0:
        raise SystemExit("cannot find the start of the method bodies")
    return text[text.rindex("\n", 0, at) + 1:]


def matching(text, start, opening="(", closing=")"):
    depth = 0
    i = start
    while i < len(text):
        if text[i] == opening:
            depth += 1
        elif text[i] == closing:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise SystemExit("unbalanced %s at %d" % (opening, start))


def split_arguments(text):
    parts = []
    depth = 0
    start = 0
    for i, c in enumerate(text):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    if text.strip():
        parts.append(text[start:])
    return parts


REFERENCE = re.compile(r"^\s*(const\s+)?([\w:]+(?:<[^<>]*>)?)\s*&\s*(\w+)\s*$")

# A const reference to one of these is cheaper to pass by value than to take
# the address of at every call site.
BY_VALUE = {"operand", "emission_mark"}


def convert_parameters(arguments):
    out = []
    defaults = []
    pointers = []
    for index, argument in enumerate(arguments):
        default = None
        if "=" in argument:
            argument, default = argument.split("=", 1)
            default = default.strip()
        found = REFERENCE.match(argument)
        if found and found.group(2) in BY_VALUE:
            argument = " %s %s" % (found.group(2), found.group(3))
        elif found:
            pointers.append(index)
            argument = " %s%s* %s" % (found.group(1) or "", found.group(2), found.group(3))
        else:
            argument = argument.rstrip()
        out.append(argument)
        defaults.append(default)
    return out, defaults, pointers


def rewrite_definitions(text):
    signatures = {}
    out = []
    at = 0
    while True:
        found = text.find("generator::", at)
        if found < 0:
            out.append(text[at:])
            break

        line_start = text.rfind("\n", 0, found) + 1
        out.append(text[at:line_start])
        prefix = text[line_start:found].strip()

        open_paren = text.index("(", found)
        name = "gen_" + text[found + len("generator::"):open_paren]
        close = matching(text, open_paren)
        arguments, defaults, pointers = convert_parameters(split_arguments(text[open_paren + 1:close]))

        # `g` is the first parameter now, so every recorded position shifts.
        signatures[name] = (len(arguments) + 1, [None] + defaults, [p + 1 for p in pointers])

        tail = text[close + 1:]
        if tail.lstrip().startswith("const"):
            tail = tail.lstrip()[len("const"):]

        separator = "" if not arguments else ","
        out.append("static %s %s(generator* g%s%s)" % (prefix, name, separator, ",".join(arguments)))
        text = text[:close + 1] + tail
        at = close + 1
    return "".join(out), signatures


def is_definition(text, found):
    """A definition starts a line with `static`; a call does not."""
    line_start = text.rfind("\n", 0, found) + 1
    return text[line_start:found].lstrip().startswith("static ")


def fix_calls(text, signatures):
    """Pad a call that relied on a default argument, and pass by address."""
    for name, (count, defaults, pointers) in signatures.items():
        at = 0
        while True:
            found = text.find(name + "(", at)
            if found < 0:
                break
            if found > 0 and (text[found - 1].isalnum() or text[found - 1] == "_"):
                at = found + 1
                continue
            if is_definition(text, found):
                at = found + 1
                continue

            open_paren = found + len(name)
            close = matching(text, open_paren)
            arguments = split_arguments(text[open_paren + 1:close])

            for index in range(len(arguments), count):
                if defaults[index] is None:
                    break
                arguments.append(" " + defaults[index])

            for index in pointers:
                if index >= len(arguments):
                    continue
                argument = arguments[index].strip()
                if re.match(r"^[\w\.\[\]]+$", argument) and not argument[0].isdigit():
                    arguments[index] = " &" + argument

            replacement = name + "(" + ",".join(arguments) + ")"
            text = text[:found] + replacement + text[close + 1:]
            at = found + len(replacement)
    return text


def fix_pointer_bodies(text, signatures):
    """Inside a definition, a parameter that became a pointer needs -> not `.`"""
    for name, (_, _, pointers) in signatures.items():
        if not pointers:
            continue
        at = 0
        while True:
            found = text.find(name + "(generator* g", at)
            if found < 0:
                found = text.find(name + "(const value* ", at)  # the static helpers
            if found < 0 or not is_definition(text, found):
                break

            open_paren = text.index("(", found)
            close = matching(text, open_paren)
            arguments = split_arguments(text[open_paren + 1:close])
            names = [arguments[i].strip().split()[-1].lstrip("*") for i in pointers
                     if i < len(arguments)]

            start = text.index("{", close)
            finish = matching(text, start, "{", "}")
            body = text[start:finish]
            for member in names:
                body = re.sub(r"\b%s\.(?!\.)" % member, member + "->", body)
            text = text[:start] + body + text[finish:]
            at = start + len(body)
    return text


def rewrite(text):
    text = re.sub(r"\btok::(\w+)", r"tok_\1", text)
    text, signatures = rewrite_definitions(text)
    for name, entry in PRELUDE.items():
        signatures.setdefault(name, entry)

    for member in MEMBERS:
        text = re.sub(r"(?<![\w.>])%s\b" % member, "g->" + member, text)

    text = re.sub(r"(?<![\w.>])peek\(\)", "(*gen_peek(g, 0))", text)
    text = re.sub(r"(?<![\w.>])peek\(([^()]*)\)", r"(*gen_peek(g, \1))", text)
    text = re.sub(r"(?<![\w.>])type\(([^()]*)\)", r"(*gen_type(g, \1))", text)

    for method in METHODS:
        text = re.sub(r"(?<![\w.>:])%s\(\)" % method, "gen_%s(g)" % method, text)
        text = re.sub(r"(?<![\w.>:])%s\(" % method, "gen_%s(g, " % method, text)

    text = fix_pointer_bodies(text, signatures)
    text = fix_calls(text, signatures)

    for member, macro in ACCESSORS.items():
        text = re.sub(r"g->%s\[size_t\(([^\[\]]+?)\)\]" % member,
                      lambda m, f=macro: f % ("(int)(" + m.group(1) + ")"), text)
        text = re.sub(r"g->%s\[(\w+)\]" % member, lambda m, f=macro: f % m.group(1), text)
    text = re.sub(r"g->(\w+)\.size\(\)", r"g->\1.count", text)
    text = re.sub(r"g->(\w+)\.empty\(\)", r"(g->\1.count == 0)", text)

    text = re.sub(r"\bnullptr\b", "NULL", text)
    text = re.sub(r"\btrue\b", "1", text)
    text = re.sub(r"\bfalse\b", "0", text)
    text = re.sub(r"\bbool\b", "int", text)
    text = re.sub(r"\b(int|size_t|uint8_t|uint16_t|int32_t|long|condition|char)\(", r"(\1)(", text)
    return text


def main():
    with open(TARGET, "r", encoding="utf-8", newline="") as file:
        target = file.read()

    if "--reset" in sys.argv:
        at = target.find("\t// ---")
        if at < 0:
            print("already reset")
            return 0
        with open(TARGET, "w", encoding="utf-8", newline="") as file:
            file.write(target[:at] + SENTINEL + "\n")
        print("gen.c is back to its prelude")
        return 0

    if SENTINEL not in target:
        raise SystemExit("the sentinel is gone; run with --reset first")

    with open(SOURCE, "r", encoding="utf-8", newline="") as file:
        source = file.read()

    body = rewrite(body_of(source))
    with open(TARGET, "w", encoding="utf-8", newline="") as file:
        file.write(target.replace(SENTINEL, body))
    print("appended %d lines" % body.count("\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
