"""Mechanical half of the parser's port from C++ to C.

Reads the method bodies out of cc/gen.cpp, turns `generator::foo(...)` into
`gen_foo(generator* g, ...)`, gives every member and every method call the `g`
it now needs, and appends the result to the hand-written prelude of cc/gen.c.

What it deliberately does not touch: std::string, std::vector, std::map,
lambdas and throw. Those need decisions, not substitutions, and the compiler
will list every one of them.
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

# Containers that became a cc_list, and the macro that reaches into each.
ACCESSORS = {
    "symbols_": ("SYMBOL_AT(g, %s)", "symbol"),
    "types_": ("TYPE_AT(g, %s)", "type_info"),
    "records_": ("RECORD_AT(g, %s)", "record_info"),
    "data_": ("DATA_AT(g, %s)", "uint8_t"),
}


def body_of(text):
    """Everything after the class declaration, which is rewritten by hand."""
    at = text.find("\t// Code generation")
    if at < 0:
        raise SystemExit("cannot find the start of the method bodies")
    at = text.rindex("\n", 0, at) + 1  # the divider line above it
    return text[at:]


def matching_paren(text, start):
    """Index of the ')' closing the '(' at `start`."""
    depth = 0
    i = start
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise SystemExit("unbalanced parentheses")


def rewrite_definitions(text):
    """RET generator::name(args) [const] -> static RET gen_name(generator* g, args)"""
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
        name = text[found + len("generator::"):open_paren]
        close = matching_paren(text, open_paren)
        arguments = text[open_paren + 1:close].strip()

        tail = text[close + 1:]
        if tail.lstrip().startswith("const"):
            tail = tail.lstrip()[len("const"):]

        separator = "" if not arguments else ", "
        out.append("static %s gen_%s(generator* g%s%s)" % (prefix, name, separator, arguments))
        at = close + 1
        text = text[:at] + tail
        at = len(text) - len(tail)
    return "".join(out)


def rewrite(text):
    text = rewrite_definitions(text)

    # Members.
    for member in MEMBERS:
        text = re.sub(r"(?<![\w.>])%s\b" % member, "g->" + member, text)

    # peek() hands back a pointer now, so the dot syntax needs the star.
    text = re.sub(r"(?<![\w.>])peek\(\)", "(*gen_peek(g, 0))", text)
    text = re.sub(r"(?<![\w.>])peek\(([^()]*)\)", r"(*gen_peek(g, \1))", text)
    text = re.sub(r"(?<![\w.>])type\(([^()]*)\)", r"(*gen_type(g, \1))", text)

    # Method calls.
    for method in METHODS:
        text = re.sub(r"(?<![\w.>:])%s\(\)" % method, "gen_%s(g)" % method, text)
        text = re.sub(r"(?<![\w.>:])%s\(" % method, "gen_%s(g, " % method, text)

    # Container access.
    for member, (macro, _) in ACCESSORS.items():
        text = re.sub(r"g->%s\[size_t\(([^\[\]]+?)\)\]" % member, lambda m, f=macro: f % ("(int)(" + m.group(1) + ")"), text)
        text = re.sub(r"g->%s\[(\w+)\]" % member, lambda m, f=macro: f % m.group(1), text)
    text = re.sub(r"g->(\w+)\.size\(\)", r"g->\1.count", text)
    text = re.sub(r"g->(\w+)\.empty\(\)", r"(g->\1.count == 0)", text)

    # C spellings.
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

    with open(SOURCE, "r", encoding="utf-8", newline="") as file:
        source = file.read()

    if SENTINEL not in target:
        raise SystemExit("the sentinel is gone; run with --reset first")

    body = rewrite(body_of(source))
    with open(TARGET, "w", encoding="utf-8", newline="") as file:
        file.write(target.replace(SENTINEL, body))
    print("appended %d lines" % body.count("\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
