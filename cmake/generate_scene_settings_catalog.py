
#!/usr/bin/env python3

import argparse
import ast
import math
import re
from pathlib import Path


PRIMITIVE_TYPES = {
    "bool": "Boolean",
    "float": "Float",
    "double": "Float",
    "int": "Integer",
    "int8_t": "Integer",
    "int16_t": "Integer",
    "int32_t": "Integer",
    "int64_t": "Integer",
    "uint": "Integer",
    "uint8_t": "Integer",
    "uint16_t": "Integer",
    "uint32_t": "Integer",
    "uint64_t": "Integer",
    "std::int8_t": "Integer",
    "std::int16_t": "Integer",
    "std::int32_t": "Integer",
    "std::int64_t": "Integer",
    "std::uint8_t": "Integer",
    "std::uint16_t": "Integer",
    "std::uint32_t": "Integer",
    "std::uint64_t": "Integer",
    "std::string": "String",
    "string": "String",
}

STRUCT_DECL_RE = r"\bstruct\s+(?:alignas\s*\([^)]*\)\s+)?(\w+)([^;{]*)\{"

CATEGORY_CONTROL_NAMES = {
    "BeginTabItem",
    "CollapsingHeader",
    "TreeNode",
    "TreeNodeEx",
}

PERSISTENT_CATEGORY_CONTROL_NAMES = {
    "DrawSectionHeader",
    "SeparatorText",
}

DIRECT_UI_CONTROL_RE = re.compile(
    r"(?:ImGui|Util)::(Checkbox|InvertedCheckbox|CheckboxFlags|RadioButton|Combo|BeginCombo|"
    r"Drag(?:Float[234]?|Int[234]?|ScalarN?)|"
    r"Slider(?:Float[234]?|Int[234]?|ScalarN?|Angle)|"
    r"Input(?:Float[234]?|Int[234]?|ScalarN?)|ColorEdit[34]|PercentageSlider)\s*\(")


def find_ui_control(line: str) -> tuple[str, int] | None:
    direct = DIRECT_UI_CONTROL_RE.search(line)
    if direct:
        return direct.group(1), direct.end()

    helper = re.search(r"\b(Draw[A-Za-z_]\w*)\s*\(", line)
    if helper and any(token in helper.group(1) for token in ("Checkbox", "Slider", "Combo", "RadioButton")):
        return helper.group(1), helper.end()
    return None


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def cpp_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def prettify(identifier: str) -> str:
    if not identifier:
        return identifier
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", identifier)
    text = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", text)
    text = text.replace("_", " ")
    return text[:1].upper() + text[1:]


def split_args(arg_text: str) -> list[str]:
    args = []
    current = []
    depth = 0
    in_string = False
    escaped = False
    for ch in arg_text:
        if in_string:
            current.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
            current.append(ch)
        elif ch in "([{<":
            depth += 1
            current.append(ch)
        elif ch in ")]}>":
            depth = max(0, depth - 1)
            current.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        args.append("".join(current).strip())
    return args


def find_matching_paren(text: str, open_index: int) -> int:
    depth = 0
    in_string = False
    escaped = False
    for i in range(open_index, len(text)):
        ch = text[i]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def find_matching_brace(text: str, open_index: int) -> int:
    depth = 0
    in_string = False
    escaped = False
    for i in range(open_index, len(text)):
        ch = text[i]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def collect_nlohmann_macros(paths: list[Path]) -> dict[str, list[str]]:
    macros: dict[str, list[str]] = {}
    token = "NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT"
    for path in paths:
        text = read_text(path)
        pos = 0
        while True:
            start = text.find(token, pos)
            if start < 0:
                break
            open_index = text.find("(", start + len(token))
            if open_index < 0:
                break
            close_index = find_matching_paren(text, open_index)
            if close_index < 0:
                break
            args = split_args(text[open_index + 1:close_index])
            if len(args) >= 2:
                type_name = args[0].strip()
                fields = [a.strip().rstrip(";") for a in args[1:] if a.strip()]
                macros[type_name] = fields
            pos = close_index + 1
    return macros


def collect_struct_bodies(paths: list[Path]) -> dict[str, str]:
    bodies: dict[str, str] = {}
    for path in paths:
        text = read_text(path)
        for match in re.finditer(STRUCT_DECL_RE, text):
            name = match.group(1)
            body_start = match.end()
            depth = 1
            i = body_start
            in_string = False
            escaped = False
            while i < len(text):
                ch = text[i]
                if in_string:
                    if escaped:
                        escaped = False
                    elif ch == "\\":
                        escaped = True
                    elif ch == '"':
                        in_string = False
                elif ch == '"':
                    in_string = True
                elif ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        bodies.setdefault(name, text[body_start:i])
                        break
                i += 1
    return bodies


def clean_type(type_name: str) -> str:
    type_name = re.sub(r"\b(const|volatile|mutable|static|inline|constexpr)\b", "", type_name)
    type_name = type_name.replace("&", "").replace("*", "").strip()
    type_name = re.sub(r"\s+", " ", type_name)
    return type_name


def parse_struct_fields(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for raw_stmt in mask_cpp_source(body).split(";"):
        parsed = parse_field_statement(raw_stmt)
        if parsed:
            fields[parsed[0]] = parsed[1]
    return fields


def parse_field_statement(raw_statement: str) -> tuple[str, str] | None:
    statement = raw_statement.strip()
    statement = re.sub(r"\b(public|private|protected)\s*:\s*", "", statement).strip()
    if not statement or statement.startswith(
            ("using ", "enum ", "static ", "static_assert", "STATIC_ASSERT", "return ")):
        return None
    statement = re.sub(r"=\s*.*$", "", statement, flags=re.DOTALL).strip()
    statement = re.sub(r"\s*\{.*\}\s*$", "", statement, flags=re.DOTALL).strip()
    statement = re.sub(r"\[[^\]]*\]", "", statement).strip()
    if not statement or "(" in statement or ")" in statement:
        return None
    match = re.match(r"(.+?)\s+([A-Za-z_]\w*)$", statement)
    if not match:
        return None
    return match.group(2), clean_type(match.group(1))


def parse_top_level_fields(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    masked = mask_cpp_source(body)
    statement_start = 0
    index = 0
    while index < len(masked):
        if masked[index] == "{":
            end = find_matching_brace(masked, index)
            if end < 0:
                break
            prefix = masked[statement_start:index]
            if not re.search(r"\b(struct|class|enum)\b", prefix):
                parsed = parse_field_statement(prefix)
                if parsed:
                    fields[parsed[0]] = parsed[1]
            statement_start = end + 1
            index = end + 1
            continue
        if masked[index] == ";":
            parsed = parse_field_statement(masked[statement_start:index])
            if parsed:
                fields[parsed[0]] = parsed[1]
            statement_start = index + 1
        index += 1
    return fields


def parse_struct_bases(declaration_tail: str) -> list[str]:
    if ":" not in declaration_tail:
        return []
    bases = []
    for base in split_args(declaration_tail.split(":", 1)[1]):
        base = re.sub(r"\b(public|private|protected|virtual)\b", "", base)
        base = clean_type(base)
        if base:
            bases.append(base)
    return bases


def collect_feature_struct_fields(paths: list[Path], features: dict[str, dict[str, str]]) -> dict[str, dict[str, dict[str, str]]]:
    raw_feature_fields: dict[str, dict[str, dict[str, str]]] = {}
    feature_bases: dict[str, dict[str, list[str]]] = {}
    for path in paths:
        text = read_text(path)
        for feature_class in features:
            feature_match = re.search(rf"\b(?:struct|class)\s+{re.escape(feature_class)}\b[^\{{;]*\{{", text)
            if not feature_match:
                continue
            feature_end = find_matching_brace(text, feature_match.end() - 1)
            if feature_end < 0:
                continue

            feature_body = text[feature_match.end():feature_end]
            for struct_match in re.finditer(STRUCT_DECL_RE, feature_body):
                body_start = struct_match.end()
                body_end = find_matching_brace(feature_body, body_start - 1)
                if body_end < 0:
                    continue
                fields = parse_struct_fields(feature_body[body_start:body_end])
                struct_name = struct_match.group(1)
                raw_feature_fields.setdefault(feature_class, {})[struct_name] = fields
                feature_bases.setdefault(feature_class, {})[struct_name] = parse_struct_bases(struct_match.group(2))

    feature_fields: dict[str, dict[str, dict[str, str]]] = {}
    for feature_class, structs in raw_feature_fields.items():
        resolved: dict[str, dict[str, str]] = {}

        def resolve_fields(struct_name: str, active: set[str] | None = None) -> dict[str, str]:
            if struct_name in resolved:
                return resolved[struct_name]
            active = set() if active is None else active
            if struct_name in active:
                return dict(structs.get(struct_name, {}))
            merged: dict[str, str] = {}
            for base in feature_bases.get(feature_class, {}).get(struct_name, []):
                base_name = base.split("::")[-1]
                if base_name in structs:
                    merged.update(resolve_fields(base_name, active | {struct_name}))
            merged.update(structs.get(struct_name, {}))
            resolved[struct_name] = merged
            return merged

        for struct_name in structs:
            resolve_fields(struct_name)
        feature_fields[feature_class] = resolved
    return feature_fields


def collect_feature_member_fields(paths: list[Path], features: dict[str, dict[str, str]]) -> dict[str, dict[str, str]]:
    feature_members: dict[str, dict[str, str]] = {}
    for path in paths:
        text = read_text(path)
        for feature_class in features:
            feature_match = re.search(rf"\b(?:struct|class)\s+{re.escape(feature_class)}\b[^\{{;]*\{{", text)
            if not feature_match:
                continue
            feature_end = find_matching_brace(text, feature_match.end() - 1)
            if feature_end < 0:
                continue

            raw_feature_body = text[feature_match.end():feature_end]
            members = parse_top_level_fields(raw_feature_body)
            for struct_match in re.finditer(STRUCT_DECL_RE, raw_feature_body):
                body_end = find_matching_brace(raw_feature_body, struct_match.end() - 1)
                if body_end < 0:
                    continue
                tail = raw_feature_body[body_end + 1:raw_feature_body.find(";", body_end)]
                for declarator in split_args(tail):
                    declarator = re.sub(r"[={].*$", "", declarator).strip().lstrip("*&")
                    member_match = re.match(r"([A-Za-z_]\w*)", declarator)
                    if member_match:
                        members[member_match.group(1)] = struct_match.group(1)
            feature_members[feature_class] = members
    return feature_members


def collect_save_roots(paths: list[Path]) -> dict[str, str]:
    roots: dict[str, str] = {}
    for path in paths:
        text = read_text(path)
        for match in re.finditer(r"\bvoid\s+(\w+)::SaveSettings\s*\([^)]*\)\s*\{", text):
            feature_class = match.group(1)
            body_end = find_matching_brace(text, match.end() - 1)
            if body_end < 0:
                continue
            body = text[match.end():body_end]
            assignment = re.search(r"\b(?:\w+|this->\w+)\s*=\s*(?:this->)?([A-Za-z_]\w*)\s*;", body)
            if assignment:
                roots[feature_class] = assignment.group(1)
    return roots


def collect_direct_persisted_fields(
        paths: list[Path],
        feature_members: dict[str, dict[str, str]]) -> dict[str, list[tuple[str, str, str]]]:
    persisted_fields: dict[str, list[tuple[str, str, str]]] = {}
    for path in paths:
        text = read_text(path)
        for match in re.finditer(r"\bvoid\s+(\w+)::SaveSettings\s*\([^)]*\)\s*\{", text):
            feature_class = match.group(1)
            body_end = find_matching_brace(text, match.end() - 1)
            if body_end < 0:
                continue
            body = text[match.end():body_end]
            for assignment in re.finditer(
                    r'\b[A-Za-z_]\w*\s*\[\s*"([^"]+)"\s*\]\s*=\s*(?:this->)?([A-Za-z_]\w*)\s*;',
                    body):
                key = assignment.group(1)
                member = assignment.group(2)
                value_type = type_to_value_type(
                    feature_members.get(feature_class, {}).get(member, ""))
                if value_type:
                    persisted_fields.setdefault(feature_class, []).append(
                        (key, value_type, member))
    return persisted_fields


def collect_string_constants(text: str) -> dict[str, str]:
    constants: dict[str, str] = {}
    pattern = re.compile(
        r'\bconstexpr\b[^;=\n]*?\b([A-Za-z_]\w*)\s*=\s*"([^"]*)"(?:sv)?\s*;')
    for match in pattern.finditer(text):
        constants[match.group(1)] = match.group(2)
    return constants


def get_function_return_expression(text: str, function_name: str) -> str | None:
    match = re.search(rf'\b{re.escape(function_name)}\s*\([^)]*\)[^{{;]*\{{', text)
    if not match:
        return None
    body_end = find_matching_brace(text, match.end() - 1)
    if body_end < 0:
        return None
    body = text[match.end():body_end]
    return_match = re.search(r'\breturn\s+(.+?)\s*;', body, flags=re.DOTALL)
    return return_match.group(1).strip() if return_match else None


def resolve_string_expression(expression: str, text: str, constants: dict[str, str],
                              visited: set[str] | None = None) -> str | None:
    visited = set() if visited is None else visited
    expression = expression.strip()
    literal = re.fullmatch(r'"([^"]*)"(?:sv)?', expression)
    if literal:
        return literal.group(1)

    wrapper = re.fullmatch(r'(?:std::)?(?:string|string_view)\s*[({]\s*(.+?)\s*[)}]', expression, flags=re.DOTALL)
    if wrapper:
        return resolve_string_expression(wrapper.group(1), text, constants, visited)

    identifier = re.fullmatch(r'(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)', expression)
    if identifier:
        name = identifier.group(1)
        return constants.get(name)

    helper = re.fullmatch(r'(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)\s*\(\s*\)', expression)
    if helper:
        name = helper.group(1)
        if name in visited:
            return None
        helper_expression = get_function_return_expression(text, name)
        if helper_expression:
            return resolve_string_expression(helper_expression, text, constants, visited | {name})
    return None


def collect_features(paths: list[Path]) -> dict[str, dict[str, str]]:
    features: dict[str, dict[str, str]] = {}
    for path in paths:
        text = read_text(path)
        class_match = re.search(r"\b(?:struct|class)\s+(\w+)[^{:;]*(?::[^{]+Feature)", text)
        if not class_match:
            continue
        class_name = class_match.group(1)
        constants = collect_string_constants(text)
        short_expression = get_function_return_expression(text, "GetShortName")
        short_name = resolve_string_expression(short_expression, text, constants) if short_expression else None
        if not short_name:
            continue
        name_expression = get_function_return_expression(text, "GetName")
        display_name = resolve_string_expression(name_expression, text, constants) if name_expression else None
        features[class_name] = {
            "short": short_name,
            "name": display_name or short_name,
            "source": str(path),
        }
    return features


def extract_i18n_calls(text: str, prefix: str) -> list[tuple[str, str]]:
    patterns = (
        (r'T\(\s*TKEY\(\s*"([^"]+)"\s*\)\s*,\s*"([^"]*)"', True),
        (r'T\(\s*"([^"]+)"\s*,\s*"([^"]*)"', False),
    )
    matches = []
    for pattern, uses_prefix in patterns:
        for match in re.finditer(pattern, text):
            key = f"{prefix}{match.group(1)}" if uses_prefix else match.group(1)
            matches.append((match.start(), key, match.group(2)))
    return [(key, fallback) for _, key, fallback in sorted(matches)]


def extract_i18n_call(text: str, prefix: str) -> tuple[str, str] | None:
    matches = extract_i18n_calls(text, prefix)
    return matches[0] if matches else None


def resolve_control_translation(
        body: str,
        control_position: int,
        label_expression: str,
        prefix: str,
        setting_key: str) -> tuple[str, str] | None:
    translated = extract_i18n_call(label_expression, prefix)
    if translated:
        return translated

    label_variable = re.fullmatch(r"[A-Za-z_]\w*", label_expression.strip())
    if not label_variable:
        return None

    variable = label_variable.group(0)
    assignment_pattern = re.compile(
        rf"(?:\b(?:const\s+)?char\s*\*\s+)?\b{re.escape(variable)}\s*=")
    assignments = list(assignment_pattern.finditer(body, 0, control_position))
    if not assignments:
        return None
    assignment = assignments[-1]
    end = body.find(";", assignment.end())
    if end < 0 or end > control_position:
        return None
    candidates = extract_i18n_calls(body[assignment.end():end], prefix)
    if not candidates:
        return None

    fallback = prettify(setting_key)
    return min(
        candidates,
        key=lambda item: (item[1] != fallback, len(item[1]), item[1]))


def mask_cpp_source(text: str) -> str:
    result = list(text)
    i = 0
    while i < len(text):
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = len(text) if end < 0 else end
            for j in range(i, end):
                result[j] = " "
            i = end
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = len(text) if end < 0 else end + 2
            for j in range(i, end):
                if result[j] not in "\r\n":
                    result[j] = " "
            i = end
            continue
        if text[i] in ('"', "'"):
            quote = text[i]
            i += 1
            while i < len(text):
                if text[i] == "\\":
                    result[i] = " "
                    if i + 1 < len(text):
                        result[i + 1] = " "
                    i += 2
                    continue
                if text[i] == quote:
                    break
                if result[i] not in "\r\n":
                    result[i] = " "
                i += 1
        i += 1
    return "".join(result)


def extract_draw_settings_body(text: str) -> tuple[str, str] | None:
    match = re.search(r"\b(\w+)::DrawSettings\s*\([^)]*\)[^{;]*\{", text)
    if not match:
        return None
    end = find_matching_brace(text, match.end() - 1)
    if end < 0:
        return None
    return match.group(1), text[match.end():end]


def collect_numeric_constants(text: str) -> dict[str, float]:
    expressions: dict[str, str] = {}
    pattern = re.compile(
        r"\b(?:constexpr|const)\b[^;=\n]*?\b([A-Za-z_]\w*)\s*=\s*([^;]+);")
    for match in pattern.finditer(text):
        declarators = split_args(match.group(2))
        if not declarators:
            continue
        expressions[match.group(1)] = declarators[0]
        for declarator in declarators[1:]:
            additional = re.match(r"(?:[A-Za-z_]\w*\s+)*([A-Za-z_]\w*)\s*=\s*(.+)", declarator)
            if additional:
                expressions[additional.group(1)] = additional.group(2).strip()

    enum_pattern = re.compile(r"\benum(?:\s+class)?(?:\s+\w+)?(?:\s*:\s*[^{]+)?\s*\{([^}]*)\}")
    for enum_match in enum_pattern.finditer(mask_cpp_source(text)):
        previous_name = ""
        for enumerator in split_args(enum_match.group(1)):
            declaration = enumerator.strip()
            if not declaration:
                continue
            name, separator, expression = declaration.partition("=")
            name = name.strip()
            if not re.fullmatch(r"[A-Za-z_]\w*", name):
                continue
            if separator:
                expressions[name] = expression.strip()
            elif previous_name:
                expressions[name] = f"{previous_name} + 1"
            else:
                expressions[name] = "0"
            previous_name = name

    constants: dict[str, float] = {}
    for _ in range(len(expressions) + 1):
        changed = False
        for name, expression in expressions.items():
            if name in constants:
                continue
            value = resolve_numeric_expression(expression, constants)
            if value is not None:
                constants[name] = value
                changed = True
        if not changed:
            break
    return constants


def resolve_numeric_expression(expression: str, constants: dict[str, float]) -> float | None:
    expression = expression.strip()
    expression = re.sub(r"^&", "", expression).strip()
    expression = re.sub(r"\b(static_cast|reinterpret_cast|const_cast)\s*<[^>]+>\s*\((.*)\)$", r"\2", expression)
    expression = re.sub(
        r"(?<![A-Za-z_])((?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)(?:[fFuUlL]+)\b",
        r"\1", expression)

    identifiers = set(re.findall(r"\b[A-Za-z_]\w*\b", expression))
    if any(identifier not in constants for identifier in identifiers):
        return None
    for identifier in sorted(identifiers, key=len, reverse=True):
        expression = re.sub(rf"\b{re.escape(identifier)}\b", repr(constants[identifier]), expression)

    try:
        tree = ast.parse(expression, mode="eval")
    except SyntaxError:
        return None

    def evaluate(node: ast.AST) -> float:
        if isinstance(node, ast.Expression):
            return evaluate(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
            return float(node.value)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
            value = evaluate(node.operand)
            return value if isinstance(node.op, ast.UAdd) else -value
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub, ast.Mult, ast.Div)):
            lhs = evaluate(node.left)
            rhs = evaluate(node.right)
            if isinstance(node.op, ast.Add):
                return lhs + rhs
            if isinstance(node.op, ast.Sub):
                return lhs - rhs
            if isinstance(node.op, ast.Mult):
                return lhs * rhs
            return lhs / rhs
        raise ValueError

    try:
        value = evaluate(tree)
    except (ValueError, ZeroDivisionError, OverflowError):
        return None
    return value if math.isfinite(value) else None


def parse_helper_parameters(parameter_text: str) -> list[tuple[str, str | None]]:
    parameters: list[tuple[str, str | None]] = []
    for parameter in split_args(parameter_text):
        declaration, separator, default = parameter.partition("=")
        name_match = re.search(r"([A-Za-z_]\w*)\s*$", declaration.strip())
        if name_match:
            parameters.append((
                name_match.group(1),
                default.strip() if separator else None))
    return parameters


def collect_ui_helper_definitions(text: str) -> dict[str, tuple[list[tuple[str, str | None]], str]]:
    helpers: dict[str, tuple[list[tuple[str, str | None]], str]] = {}
    pattern = re.compile(
        r"\b(?:static\s+)?(?:inline\s+)?(?:bool|void)\s+"
        r"(Draw[A-Za-z_]\w*)\s*\(([^)]*)\)\s*\{")
    for match in pattern.finditer(text):
        body_end = find_matching_brace(text, match.end() - 1)
        if body_end >= 0:
            helpers[match.group(1)] = (
                parse_helper_parameters(match.group(2)),
                text[match.end():body_end])
    return helpers


def substitute_helper_parameters(
        expression: str,
        arguments: dict[str, str]) -> str:
    for name in sorted(arguments, key=len, reverse=True):
        expression = re.sub(
            rf"\b{re.escape(name)}\b",
            lambda _: f"({arguments[name]})",
            expression)
    return expression


def resolve_helper_control(
        helper_name: str,
        invocation_args: list[str],
        helpers: dict[str, tuple[list[tuple[str, str | None]], str]],
        active: set[str] | None = None) -> tuple[str, list[str]] | None:
    if helper_name not in helpers:
        return None
    active = set() if active is None else active
    if helper_name in active:
        return None

    parameters, body = helpers[helper_name]
    arguments: dict[str, str] = {}
    for index, (name, default) in enumerate(parameters):
        if index < len(invocation_args):
            arguments[name] = invocation_args[index]
        elif default is not None:
            arguments[name] = default

    masked = mask_cpp_source(body)
    direct = DIRECT_UI_CONTROL_RE.search(masked)
    if direct:
        close = find_matching_paren(body, direct.end() - 1)
        if close >= 0:
            args = [
                substitute_helper_parameters(argument, arguments)
                for argument in split_args(body[direct.end():close])
            ]
            return direct.group(1), args

    nested_pattern = re.compile(r"\b(Draw[A-Za-z_]\w*)\s*\(")
    for nested in nested_pattern.finditer(masked):
        close = find_matching_paren(body, nested.end() - 1)
        if close < 0:
            continue
        args = [
            substitute_helper_parameters(argument, arguments)
            for argument in split_args(body[nested.end():close])
        ]
        resolved = resolve_helper_control(
            nested.group(1), args, helpers, active | {helper_name})
        if resolved:
            return resolved
    return None


def get_control_numeric_metadata(control_kind: str, args: list[str],
                                 constants: dict[str, float]) -> tuple[float, float, float] | None:
    bounds: tuple[str, str] | None = None
    if control_kind.startswith("Slider") and control_kind != "SliderScalarN" and len(args) >= 4:
        if control_kind.startswith("SliderScalar") and len(args) >= 5:
            bounds = (args[3], args[4])
        else:
            bounds = (args[2], args[3])
    elif control_kind.startswith("Drag") and len(args) >= 5:
        bounds = (args[3], args[4])
    elif control_kind == "PercentageSlider":
        bounds = (args[2] if len(args) >= 3 else "0.0", args[3] if len(args) >= 4 else "100.0")
    if not bounds:
        return None
    minimum = resolve_numeric_expression(bounds[0], constants)
    maximum = resolve_numeric_expression(bounds[1], constants)
    if minimum is None or maximum is None or minimum >= maximum:
        return None
    display_scale = 1.0
    if control_kind == "PercentageSlider":
        minimum /= 100.0
        maximum /= 100.0
        display_scale = 100.0
    elif control_kind == "SliderAngle":
        minimum = math.radians(minimum)
        maximum = math.radians(maximum)
        display_scale = 180.0 / math.pi
    return minimum, maximum, display_scale


def collect_ui_labels(paths: list[Path]) -> dict[tuple[str, tuple[str, ...]], tuple[str, str, str, str, str, float | None, float | None, float]]:
    labels: dict[tuple[str, tuple[str, ...]], tuple[str, str, str, str, str, float | None, float | None, float]] = {}
    category_pattern = re.compile(
        r"\b(?:ImGui|Util)::(" + "|".join(sorted(CATEGORY_CONTROL_NAMES | PERSISTENT_CATEGORY_CONTROL_NAMES)) + r")\s*\(")
    helper_pattern = re.compile(r"\b(Draw[A-Za-z_]\w*(?:Checkbox|Slider|Combo|RadioButton)[A-Za-z_]*)\s*\(")

    for path in paths:
        text = read_text(path)
        draw_settings = extract_draw_settings_body(text)
        if not draw_settings:
            continue
        feature, body = draw_settings
        prefix_match = re.search(r'#define\s+I18N_KEY_PREFIX\s+"([^"]*)"', text)
        prefix = prefix_match.group(1) if prefix_match else ""
        companion_header = path.with_suffix(".h")
        constants = collect_numeric_constants(
            text + (read_text(companion_header) if companion_header.exists() else ""))
        helpers = collect_ui_helper_definitions(text)
        masked = mask_cpp_source(body)
        events: list[tuple[int, int, str, object]] = []

        for match in category_pattern.finditer(masked):
            close = find_matching_paren(body, match.end() - 1)
            if close < 0:
                continue
            translated = extract_i18n_call(body[match.start():close + 1], prefix)
            if translated:
                events.append((match.start(), 0, "category", (match.group(1), translated)))

        seen_controls: set[int] = set()
        for pattern in (DIRECT_UI_CONTROL_RE, helper_pattern):
            for match in pattern.finditer(masked):
                if match.start() in seen_controls:
                    continue
                seen_controls.add(match.start())
                close = find_matching_paren(body, match.end() - 1)
                if close < 0:
                    continue
                invocation = body[match.start():close + 1]
                setting_match = re.search(
                    r"&?(?:settings|debugSettings|[A-Za-z_]\w*Settings)\.([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)",
                    invocation)
                if not setting_match:
                    continue
                args = split_args(body[match.end():close])
                control_kind = match.group(1)
                if pattern is helper_pattern:
                    resolved = resolve_helper_control(
                        control_kind, args, helpers)
                    if resolved:
                        control_kind, args = resolved
                events.append((match.start(), 1, "control", (
                    control_kind, tuple(setting_match.group(1).split(".")), invocation, args)))

        for position, ch in enumerate(masked):
            if ch in "{}":
                events.append((position, 2, "brace", ch))
        events.sort(key=lambda event: (event[0], event[1]))

        depth = 0
        category_scopes: list[tuple[int, str, str]] = []
        pending_category: tuple[str, str] | None = None
        for event_position, _, event_kind, payload in events:
            if event_kind == "brace":
                if payload == "{":
                    depth += 1
                    if pending_category:
                        category_scopes.append((depth, pending_category[0], pending_category[1]))
                        pending_category = None
                else:
                    category_scopes = [scope for scope in category_scopes if scope[0] < depth]
                    depth = max(0, depth - 1)
                continue
            if event_kind == "category":
                control_kind, translated = payload
                if control_kind in CATEGORY_CONTROL_NAMES:
                    pending_category = (translated[1], translated[0])
                else:
                    category_scopes = [scope for scope in category_scopes if scope[0] != depth]
                    category_scopes.append((depth, translated[1], translated[0]))
                continue

            control_kind, setting_path, invocation, args = payload
            translated = resolve_control_translation(
                body,
                event_position,
                args[0] if args else "",
                prefix,
                setting_path[-1])
            if not translated:
                translated = extract_i18n_call(invocation, prefix)
            label_key = translated[0] if translated else ""
            if translated:
                label = translated[1]
            else:
                literal_match = re.match(r'\s*"([^"]+)"', args[0] if args else "")
                label = literal_match.group(1) if literal_match else prettify(setting_path[-1])
            label = label.split("##", 1)[0]
            current_category = category_scopes[-1] if category_scopes else (0, "", "")
            numeric_metadata = get_control_numeric_metadata(control_kind, args, constants)
            labels.setdefault((feature, setting_path), (
                label, current_category[1], label_key, current_category[2], control_kind,
                numeric_metadata[0] if numeric_metadata else None,
                numeric_metadata[1] if numeric_metadata else None,
                numeric_metadata[2] if numeric_metadata else 1.0))
    return labels


def collect_ui_choices(
        paths: list[Path]) -> dict[tuple[str, tuple[str, ...]], tuple[tuple[int, str, str], ...]]:
    collected: dict[tuple[str, tuple[str, ...]], list[tuple[int, str, str]]] = {}
    for path in paths:
        text = read_text(path)
        draw_settings = extract_draw_settings_body(text)
        if not draw_settings:
            continue
        feature, body = draw_settings
        prefix_match = re.search(r'#define\s+I18N_KEY_PREFIX\s+"([^"]*)"', text)
        prefix = prefix_match.group(1) if prefix_match else ""
        companion_header = path.with_suffix(".h")
        constants = collect_numeric_constants(
            text + (read_text(companion_header) if companion_header.exists() else ""))
        masked = mask_cpp_source(body)
        for match in DIRECT_UI_CONTROL_RE.finditer(masked):
            if match.group(1) != "RadioButton":
                continue
            close = find_matching_paren(body, match.end() - 1)
            if close < 0:
                continue
            invocation = body[match.start():close + 1]
            setting_match = re.search(
                r"&?(?:settings|debugSettings|[A-Za-z_]\w*Settings)\.([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)",
                invocation)
            args = split_args(body[match.end():close])
            if not setting_match or len(args) < 3:
                continue
            value = resolve_numeric_expression(args[2], constants)
            if value is None or not value.is_integer():
                continue
            setting_path = tuple(setting_match.group(1).split("."))
            translated = resolve_control_translation(
                body, match.start(), args[0], prefix, setting_path[-1])
            if translated:
                label_key, label = translated
            else:
                literal_match = re.match(r'\s*"([^"]+)"', args[0])
                label = literal_match.group(1) if literal_match else prettify(setting_path[-1])
                label_key = ""
            identity = (feature, setting_path)
            choice = (int(value), label.split("##", 1)[0], label_key)
            if choice not in collected.setdefault(identity, []):
                collected[identity].append(choice)
    return {
        identity: tuple(sorted(choices, key=lambda choice: choice[0]))
        for identity, choices in collected.items()
    }


def type_to_value_type(type_name: str) -> str | None:
    cleaned = clean_type(type_name)
    if cleaned.startswith("std::atomic<"):
        inner = cleaned[len("std::atomic<"):-1].strip()
        cleaned = inner
    if cleaned in PRIMITIVE_TYPES:
        return PRIMITIVE_TYPES[cleaned]
    if cleaned.endswith("::value_type"):
        return None
    if cleaned in {"RE::NiColor", "RE::NiPoint2", "RE::NiPoint3", "DirectX::XMFLOAT2", "DirectX::XMFLOAT3", "DirectX::XMFLOAT4"}:
        return None
    return None


def nested_type_candidates(owner: str, field_type: str) -> list[str]:
    cleaned = clean_type(field_type)
    return [cleaned, f"{owner}::{cleaned}"]


def build_entries(source_dir: Path) -> list[dict[str, object]]:
    src_paths = list((source_dir / "src").rglob("*.cpp")) + list((source_dir / "src").rglob("*.h"))
    src_paths += [source_dir / "src" / "TruePBR.cpp", source_dir / "src" / "TruePBR.h"]
    src_paths = sorted({p for p in src_paths if p.exists()})

    macros = collect_nlohmann_macros(src_paths)
    struct_bodies = collect_struct_bodies(src_paths)
    struct_fields = {name: parse_struct_fields(body) for name, body in struct_bodies.items()}
    features = collect_features([p for p in src_paths if p.suffix == ".h"])
    feature_fields = collect_feature_struct_fields([p for p in src_paths if p.suffix == ".h"], features)
    feature_members = collect_feature_member_fields([p for p in src_paths if p.suffix == ".h"], features)
    save_roots = collect_save_roots([p for p in src_paths if p.suffix == ".cpp"])
    direct_persisted_fields = collect_direct_persisted_fields(
        [p for p in src_paths if p.suffix == ".cpp"], feature_members)
    labels = collect_ui_labels([p for p in src_paths if p.suffix == ".cpp"])
    ui_choices = collect_ui_choices([p for p in src_paths if p.suffix == ".cpp"])

    entries: list[dict[str, object]] = []
    seen: set[tuple[str, tuple[str, ...], str]] = set()
    discovery_errors: list[str] = []

    def add_entry(feature_class: str, path: list[str], key: str, value_type: str, access: str):
        feature = features.get(feature_class)
        if not feature:
            return
        feature_short = feature["short"]
        identity = (feature_short, tuple(path), key)
        if identity in seen:
            return
        seen.add(identity)
        source_path = Path(feature["source"])
        try:
            include_path = source_path.relative_to(source_dir / "src").as_posix()
        except ValueError:
            include_path = source_path.as_posix()

        label_metadata = labels.get(
            (feature_class, tuple(path + [key])),
            (prettify(key), "", "", "", "", None, None, 1.0))
        label, ui_category, label_key, category_key, control_kind, minimum, maximum, display_scale = label_metadata
        choices = ui_choices.get((feature_class, tuple(path + [key])), ())
        if choices and control_kind == "RadioButton":
            label = prettify(key)
            label_key = ""
        is_toggle = control_kind == "Checkbox" or control_kind.endswith("Checkbox")
        is_numeric_control = (control_kind.startswith(("Slider", "Drag")) or
                              control_kind == "PercentageSlider" or
                              minimum is not None or maximum is not None)
        if choices:
            editor_semantic = "Choice"
        elif is_toggle and value_type in {"Boolean", "Integer"}:
            editor_semantic = "Toggle"
        elif is_numeric_control and value_type in {"Float", "Integer"} and minimum is not None and maximum is not None:
            editor_semantic = "Numeric"
        else:
            editor_semantic = "None"

        flags = ["SceneSettingsCatalog::SettingFlag::Persisted"]
        if value_type == "Float" and editor_semantic == "Numeric":
            flags.append("SceneSettingsCatalog::SettingFlag::Transitionable")
        if editor_semantic == "Toggle":
            flags.append("SceneSettingsCatalog::SettingFlag::BooleanControl")
        lowered = " ".join(path + [ui_category, key]).lower()
        if editor_semantic == "None" or "debug" in lowered:
            flags.append("SceneSettingsCatalog::SettingFlag::Hidden")
        else:
            flags.append("SceneSettingsCatalog::SettingFlag::SceneControllable")

        display_path = [p for p in path]
        if ui_category:
            if display_path and prettify(display_path[0]) == ui_category:
                display_path[0] = ui_category
            elif not display_path or display_path[0] != ui_category:
                display_path.insert(0, ui_category)

        entries.append({
            "featureClass": feature_class,
            "feature": feature_short,
            "featureName": feature["name"],
            "include": include_path,
            "path": "/".join(path),
            "key": key,
            "displayName": label,
            "displayNameKey": label_key,
            "displayPath": "/".join(display_path),
            "displayCategoryKey": category_key,
            "type": value_type,
            "flags": " | ".join(flags),
            "editorSemantic": editor_semantic,
            "minimum": minimum if minimum is not None else 0.0,
            "maximum": maximum if maximum is not None else 0.0,
            "displayScale": display_scale,
            "hasNumericBounds": minimum is not None and maximum is not None,
            "invertedDisplay": control_kind == "InvertedCheckbox",
            "choices": choices,
            "access": access,
        })

    def emit_type(feature_class: str, full_type: str, path: list[str], access: str):
        fields = macros.get(full_type)
        if not fields:
            return
        simple_type = full_type.split("::")[-1]
        declared_fields = feature_fields.get(feature_class, {}).get(simple_type, struct_fields.get(simple_type, {}))
        for field in fields:
            field_type = declared_fields.get(field, "")
            if not field_type:
                discovery_errors.append(f"{full_type}.{field} has no discovered declaration")
                continue
            value_type = type_to_value_type(field_type)
            field_access = f"{access}.{field}"
            if value_type:
                add_entry(feature_class, path, field, value_type, field_access)
                continue
            emitted_nested = False
            for candidate in nested_type_candidates(feature_class, field_type):
                if candidate in macros:
                    emit_type(feature_class, candidate, path + [field], field_access)
                    emitted_nested = True
                    break
            if not emitted_nested and field_type:
                continue

    for feature_class in sorted(features):
        members = feature_members.get(feature_class, {})
        root_member = save_roots.get(feature_class)
        if not root_member:
            for member_name, member_type in members.items():
                if clean_type(member_type).split("::")[-1] == "Settings":
                    root_member = member_name
                    break
        if not root_member:
            if f"{feature_class}::Settings" in macros:
                discovery_errors.append(
                    f"{feature_class} has persisted Settings but no discovered settings member")
            continue

        root_type = clean_type(members.get(root_member, ""))
        if not root_type:
            if f"{feature_class}::Settings" in macros:
                discovery_errors.append(
                    f"{feature_class}.{root_member} has no discovered settings type")
            continue
        root_full_type = f"{feature_class}::{root_type.split('::')[-1]}"
        emit_type(feature_class, root_full_type, [], root_member)

    for feature_class, persisted_fields in direct_persisted_fields.items():
        for key, value_type, access in persisted_fields:
            add_entry(feature_class, [], key, value_type, access)

    if discovery_errors:
        raise ValueError("scene settings catalog discovery failed: " + "; ".join(sorted(set(discovery_errors))))

    entries.sort(key=lambda e: (e["feature"], e["displayPath"], e["displayName"], e["path"], e["key"]))
    return entries


def write_catalog(entries: list[dict[str, object]], out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    header = out_dir / "SceneSettingsCatalog.generated.h"
    source = out_dir / "SceneSettingsCatalog.generated.cpp"
    adapters = out_dir / "FeatureSceneSettingsAdapters.generated.cpp"

    header.write_text("""#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

struct Feature;

namespace SceneSettingsCatalog
{
\tenum class ValueType : std::uint8_t
\t{
\t\tBoolean,
\t\tInteger,
\t\tFloat,
\t\tString,
\t};

\tenum class EditorSemantic : std::uint8_t
\t{
\t\tNone,
\t\tToggle,
\t\tNumeric,
\t\tChoice,
\t};

\tenum class SettingFlag : std::uint32_t
\t{
\t\tNone = 0,
\t\tPersisted = 1u << 0,
\t\tTransitionable = 1u << 1,
\t\tHidden = 1u << 2,
\t\tBooleanControl = 1u << 3,
\t\tSceneControllable = 1u << 4,
\t};

\tconstexpr SettingFlag operator|(SettingFlag lhs, SettingFlag rhs)
\t{
\t\treturn static_cast<SettingFlag>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
\t}

\tconstexpr bool HasFlag(SettingFlag flags, SettingFlag flag)
\t{
\t\treturn (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(flag)) != 0;
\t}

\tstruct ChoiceMetadata
\t{
\t\tstd::int64_t value;
\t\tstd::string_view displayName;
\t\tstd::string_view displayNameKey;
\t};

\tstruct SettingMetadata
\t{
\t\tstd::string_view featureShortName;
\t\tstd::string_view featureDisplayName;
\t\tstd::string_view settingPath;
\t\tstd::string_view settingKey;
\t\tstd::string_view displayName;
\t\tstd::string_view displayNameKey;
\t\tstd::string_view displayPath;
\t\tstd::string_view displayCategoryKey;
\t\tValueType valueType;
\t\tSettingFlag flags;
\t\tEditorSemantic editorSemantic;
\t\tdouble minimumValue;
\t\tdouble maximumValue;
\t\tdouble displayScale;
\t\tbool hasNumericBounds;
\t\tbool invertedDisplay;
\t\tconst ChoiceMetadata* choices;
\t\tstd::size_t choiceCount;
\t};

\tconstexpr bool IsSceneControllable(const SettingMetadata& setting)
\t{
\t\treturn HasFlag(setting.flags, SettingFlag::SceneControllable) &&
\t\t       !HasFlag(setting.flags, SettingFlag::Hidden);
\t}

\tstd::span<const SettingMetadata> GetSettings();
\tconst SettingMetadata* FindSetting(std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey);
\tusing ControlResolver = const SettingMetadata* (*)(Feature*, const void*);
\tbool RegisterControlResolver(std::string_view featureShortName, ControlResolver resolver);
\tconst SettingMetadata* FindSettingForControl(Feature* feature, const void* valueAddress);
}
""", encoding="utf-8")
    rows = []
    choice_arrays = []
    for index, e in enumerate(entries):
        choices = e["choices"]
        choice_pointer = "nullptr"
        choice_count = 0
        if choices:
            choice_name = f"kSceneSettingChoices{index}"
            choice_rows = ",\n".join(
                f'\t\t{{ {value}, "{cpp_escape(label)}", "{cpp_escape(label_key)}" }}'
                for value, label, label_key in choices)
            choice_arrays.append(
                f"\tstatic constexpr std::array<SceneSettingsCatalog::ChoiceMetadata, {len(choices)}> {choice_name} = {{{{\n"
                f"{choice_rows}\n\t}}}};")
            choice_pointer = f"{choice_name}.data()"
            choice_count = len(choices)
        rows.append(
            f'\t\t{{ "{cpp_escape(e["feature"])}", "{cpp_escape(e["featureName"])}", '
            f'"{cpp_escape(e["path"])}", "{cpp_escape(e["key"])}", "{cpp_escape(e["displayName"])}", "{cpp_escape(e["displayNameKey"])}", '
            f'"{cpp_escape(e["displayPath"])}", "{cpp_escape(e["displayCategoryKey"])}", '
            f'SceneSettingsCatalog::ValueType::{e["type"]}, {e["flags"]}, '
            f'SceneSettingsCatalog::EditorSemantic::{e["editorSemantic"]}, '
            f'{e["minimum"]!r}, {e["maximum"]!r}, {e["displayScale"]!r}, '
            f'{str(e["hasNumericBounds"]).lower()}, {str(e["invertedDisplay"]).lower()}, '
            f'{choice_pointer}, {choice_count} }},'
        )
    joined_rows = "\n".join(rows)
    joined_choice_arrays = "\n".join(choice_arrays)
    includes = "\n".join(f'#include "{cpp_escape(include_path)}"' for include_path in sorted({e["include"] for e in entries}))
    feature_blocks = []
    for feature_short in sorted({e["feature"] for e in entries}):
        feature_entries = [e for e in entries if e["feature"] == feature_short]
        feature_class = feature_entries[0]["featureClass"]
        checks = "\n".join(
            f'\t\tif (valueAddress == static_cast<const void*>(&typedFeature->{e["access"]}))\n'
            f'\t\t\treturn SceneSettingsCatalog::FindSetting("{cpp_escape(e["feature"])}", '
            f'"{cpp_escape(e["path"])}", "{cpp_escape(e["key"])}");'
            for e in feature_entries)
        resolver_name = f"Resolve{feature_class}SceneSettingControl"
        feature_blocks.append(f'''\tconst SceneSettingsCatalog::SettingMetadata* {resolver_name}(
\t\tFeature* feature, const void* valueAddress)
\t{{
\t\tauto* typedFeature = static_cast<{feature_class}*>(feature);
{checks}
\t\treturn nullptr;
\t}}

\t[[maybe_unused]] const bool registered{feature_class}SceneSettings =
\t\tSceneSettingsCatalog::RegisterControlResolver(
\t\t\t"{cpp_escape(feature_short)}", {resolver_name});''')
    joined_feature_blocks = "\n".join(feature_blocks)
    source.write_text(f"""#include "SceneSettingsCatalog.generated.h"

#include <array>

namespace
{{
{joined_choice_arrays}
\tstatic constexpr std::array<SceneSettingsCatalog::SettingMetadata, {len(entries)}> kSceneSettings = {{{{
{joined_rows}
\t}}}};
}}

namespace SceneSettingsCatalog
{{
\tstd::span<const SettingMetadata> GetSettings()
\t{{
\t\treturn kSceneSettings;
\t}}

\tconst SettingMetadata* FindSetting(std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey)
\t{{
\t\tfor (const auto& setting : kSceneSettings) {{
\t\t\tif (setting.featureShortName == featureShortName &&
\t\t\t\tsetting.settingPath == settingPath &&
\t\t\t\tsetting.settingKey == settingKey) {{
\t\t\t\treturn &setting;
\t\t\t}}
\t\t}}
\t\treturn nullptr;
\t}}

}}
""", encoding="utf-8")

    adapters.write_text(f"""#include "SceneSettingsCatalog.generated.h"

#include "Feature.h"
{includes}

#include <algorithm>
#include <utility>
#include <vector>

namespace
{{
\tusing ControlResolverEntry = std::pair<std::string_view, SceneSettingsCatalog::ControlResolver>;

\tstd::vector<ControlResolverEntry>& GetControlResolvers()
\t{{
\t\tstatic std::vector<ControlResolverEntry> resolvers;
\t\treturn resolvers;
\t}}

{joined_feature_blocks}
}}

namespace SceneSettingsCatalog
{{
\tbool RegisterControlResolver(std::string_view featureShortName, ControlResolver resolver)
\t{{
\t\tif (featureShortName.empty() || !resolver)
\t\t\treturn false;

\t\tauto& resolvers = GetControlResolvers();
\t\tif (std::ranges::any_of(resolvers,
\t\t\t\t[&](const auto& entry) {{ return entry.first == featureShortName; }}))
\t\t\treturn false;
\t\tresolvers.emplace_back(featureShortName, resolver);
\t\treturn true;
\t}}

\tconst SettingMetadata* FindSettingForControl(Feature* feature, const void* valueAddress)
\t{{
\t\tif (!feature || !valueAddress)
\t\t\treturn nullptr;

\t\tconst auto featureShortName = feature->GetShortName();
\t\tauto& resolvers = GetControlResolvers();
\t\tauto resolver = std::ranges::find_if(resolvers,
\t\t\t[&](const auto& entry) {{ return entry.first == featureShortName; }});
\t\treturn resolver != resolvers.end() ? resolver->second(feature, valueAddress) : nullptr;
\t}}
}}
""", encoding="utf-8")


def validate_entries(entries: list[dict[str, object]], min_entries: int) -> None:
    if len(entries) < min_entries:
        raise ValueError(
            f"scene settings catalog found {len(entries)} entries, expected at least {min_entries}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--min-entries", type=int, default=1)
    args = parser.parse_args()

    source_dir = Path(args.source_dir)
    out_dir = Path(args.out_dir)
    entries = build_entries(source_dir)
    try:
        validate_entries(entries, args.min_entries)
    except ValueError as error:
        parser.error(str(error))
    write_catalog(entries, out_dir)
    print(f"Generated {len(entries)} scene setting catalog entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
