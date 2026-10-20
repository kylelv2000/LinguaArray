#!/usr/bin/env python3
"""
SMTParser - SMT parser based on pysmt and z3

Core Design:
1. Use pysmt to parse SMT-LIB commands (declare-fun, set-logic, etc.)
2. Use z3 for syntax validation and expression manipulation
3. Get function declarations directly from declare-fun commands without recursive expression traversal
"""

import sys
import json
import re
from typing import List, Dict, Optional, Tuple, Set
from io import StringIO

try:
    from pysmt.smtlib.parser import SmtLibParser
    from pysmt.exceptions import PysmtSyntaxError, UnknownSmtLibCommandError
    from pysmt.environment import reset_env
except ImportError:
    print("Error: pysmt required\nPlease run: pip install pysmt", file=sys.stderr)
    sys.exit(1)

try:
    import z3
except ImportError:
    print("Error: z3-solver required\nPlease run: pip install z3-solver", file=sys.stderr)
    sys.exit(1)


class SMTParser:
    """SMT parser based on pysmt"""
    
    def parse_file(self, filepath: str) -> Dict:
        """Parse SMT file"""
        try:
            with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                return self.parse_content(f.read())
        except Exception as e:
            return self._error_result(f"Unable to open file: {e}")
    
    def parse_content(self, content: str) -> Dict:
        """Parse SMT commands using pysmt directly"""
        try:
            # Reset pysmt global environment to avoid symbol conflicts
            reset_env()
            pysmt_parser = SmtLibParser()
            script = pysmt_parser.get_script(StringIO(content))
            
            logic = None
            ufs = []
            arrays = []
            custom_sorts = set()
            defined_funcs = set()
            
            for cmd in script.commands:
                if cmd.name == 'set-logic':
                    logic = str(cmd.args[0])
                
                elif cmd.name == 'declare-sort':
                    custom_sorts.add(str(cmd.args[0]))
                
                elif cmd.name in ('define-fun', 'define-fun-rec'):
                    defined_funcs.add(str(cmd.args[0]))
                
                elif cmd.name == 'declare-fun':
                    symbol = cmd.args[0]
                    name = symbol.symbol_name()
                    
                    if name in defined_funcs:
                        continue
                    
                    stype = symbol.symbol_type()
                    
                    if hasattr(stype, 'is_function_type') and stype.is_function_type():
                        ufs.append({
                            "name": name,
                            "parameters": [str(t) for t in stype.param_types],
                            "return_type": str(stype.return_type),
                            "usage_contexts": []
                        })
                    elif hasattr(stype, 'is_array_type') and stype.is_array_type():
                        index_types, elem_type = self._parse_pysmt_array_type(stype)
                        arrays.append({
                            "name": name,
                            "index_types": index_types,
                            "element_type": elem_type,
                            "usage_contexts": []
                        })
            
            return {
                "success": True,
                "logic": logic,
                "uninterpreted_functions": ufs,
                "array_variables": arrays,
                "custom_types": list(custom_sorts),
                "content": content,
                "error_message": ""
            }
        except (PysmtSyntaxError, UnknownSmtLibCommandError) as e:
            # pysmt unsupported syntax, fallback to z3
            return self._parse_with_z3_fallback(content, str(e))
        except Exception as e:
            # Other exceptions also try z3 fallback
            return self._parse_with_z3_fallback(content, str(e))
    
    def _parse_pysmt_array_type(self, stype) -> Tuple[List[str], str]:
        """Parse pysmt array type"""
        index_types = []
        while hasattr(stype, 'is_array_type') and stype.is_array_type():
            index_types.append(str(stype.index_type))
            stype = stype.elem_type
        return index_types, str(stype)
    
    def _parse_with_z3_fallback(self, content: str, pysmt_error: str) -> Dict:
        """Use text parsing as fallback when pysmt parsing fails (consistent with old version)"""
        try:
            # Extract all declare-fun from text (consistent with old version)
            declared_funcs = self._extract_declarations_from_text(content)
            defined_funcs = self._extract_definitions_from_text(content)
            
            ufs, arrays = [], []
            for name, info in declared_funcs.items():
                if name in defined_funcs:
                    continue
                
                params_str = info.get('params', '')
                return_type = info.get('return_type', '')
                
                if params_str.strip():  # Has parameters -> UF
                    param_types = self._parse_param_types(params_str)
                    ufs.append({
                        "name": name,
                        "parameters": param_types,
                        "return_type": return_type,
                        "usage_contexts": []
                    })
                elif 'Array' in return_type:  # Array variable
                    index_types, elem_type = self._parse_array_type(return_type)
                    arrays.append({
                        "name": name,
                        "index_types": index_types,
                        "element_type": elem_type,
                        "usage_contexts": []
                    })
            
            logic_match = re.search(r'\(set-logic\s+(\S+)\s*\)', content)
            sorts = set(m.group(1) for m in re.finditer(r'\(declare-sort\s+(\S+)', content))
            
            return {
                "success": True,
                "logic": logic_match.group(1) if logic_match else None,
                "uninterpreted_functions": ufs,
                "array_variables": arrays,
                "custom_types": list(sorts),
                "content": content,
                "error_message": f"(Using z3 fallback parsing)"
            }
        except Exception as e:
            return self._error_result(f"Parsing failed: {e}")
    
    def _collect_decls_z3(self, exprs) -> List[z3.FuncDeclRef]:
        """Recursively collect function declarations using z3"""
        decls = {}
        visited = set()
        worklist = [exprs[i] for i in range(len(exprs))] if hasattr(exprs, '__len__') else [exprs]
        
        while worklist:
            expr = worklist.pop()
            if not z3.is_expr(expr) or expr.get_id() in visited:
                continue
            visited.add(expr.get_id())
            
            if z3.is_app(expr):
                d = expr.decl()
                if d.name() not in decls:
                    decls[d.name()] = d
                worklist.extend(expr.children())
            elif z3.is_quantifier(expr):
                worklist.append(expr.body())
        
        return list(decls.values())
    
    def _get_array_index_types_z3(self, sort: z3.SortRef) -> List[str]:
        types = []
        while sort.kind() == z3.Z3_ARRAY_SORT:
            types.append(str(sort.domain()))
            sort = sort.range()
        return types
    
    def _get_array_elem_type_z3(self, sort: z3.SortRef) -> z3.SortRef:
        while sort.kind() == z3.Z3_ARRAY_SORT:
            sort = sort.range()
        return sort
    
    def _extract_declarations_from_text(self, content: str) -> Dict[str, Dict]:
        """Extract all declare-fun declarations from text"""
        declarations = {}
        lines = content.split('\n')
        i = 0
        while i < len(lines):
            line = lines[i]
            if '(declare-fun' in line:
                full_decl, end_i = self._collect_sexp(lines, i)
                match = re.search(r'\(declare-fun\s+(\|[^|]+\||\S+)', full_decl)
                if match:
                    name = match.group(1)
                    params, return_type = self._parse_declare_fun(full_decl)
                    declarations[name] = {'params': params, 'return_type': return_type}
                i = end_i + 1
            else:
                i += 1
        return declarations
    
    def _extract_definitions_from_text(self, content: str) -> Set[str]:
        """Extract all define-fun defined function names from text"""
        defined = set()
        for match in re.finditer(r'\(define-fun(?:-rec)?\s+(\S+)', content):
            defined.add(match.group(1))
        return defined
    
    def _parse_declare_fun(self, decl: str) -> Tuple[str, str]:
        """Parse declare-fun, return (params_str, return_type)"""
        match = re.search(r'\(declare-fun\s+\S+\s*\(', decl)
        if not match:
            return '', ''
        
        pos = match.end() - 1
        depth = 1
        start = pos + 1
        while pos + 1 < len(decl) and depth > 0:
            pos += 1
            if decl[pos] == '(':
                depth += 1
            elif decl[pos] == ')':
                depth -= 1
        params = decl[start:pos].strip()
        
        pos += 1
        while pos < len(decl) and decl[pos] in ' \t\n':
            pos += 1
        
        start = pos
        if pos < len(decl) and decl[pos] == '(':
            depth = 1
            pos += 1
            while pos < len(decl) and depth > 0:
                if decl[pos] == '(':
                    depth += 1
                elif decl[pos] == ')':
                    depth -= 1
                pos += 1
            return_type = decl[start:pos].strip()
        else:
            end = pos
            while end < len(decl) and decl[end] not in ' \t\n)':
                end += 1
            return_type = decl[start:end].strip()
        
        return params, return_type
    
    def _parse_param_types(self, params_str: str) -> List[str]:
        """Parse parameter type list"""
        if not params_str.strip():
            return []
        
        types = []
        pos = 0
        params_str = params_str.strip()
        
        while pos < len(params_str):
            while pos < len(params_str) and params_str[pos] in ' \t':
                pos += 1
            if pos >= len(params_str):
                break
            
            if params_str[pos] == '(':
                start = pos
                depth = 1
                pos += 1
                while pos < len(params_str) and depth > 0:
                    if params_str[pos] == '(':
                        depth += 1
                    elif params_str[pos] == ')':
                        depth -= 1
                    pos += 1
                types.append(params_str[start:pos])
            else:
                start = pos
                while pos < len(params_str) and params_str[pos] not in ' \t()':
                    pos += 1
                if start < pos:
                    types.append(params_str[start:pos])
        
        return types
    
    def _parse_array_type(self, type_str: str) -> Tuple[List[str], str]:
        """Parse Array type, return (index_types, element_type)"""
        type_str = type_str.strip()
        if not type_str.startswith('(') or 'Array' not in type_str:
            return [], type_str
        
        inner = type_str[1:-1].strip()
        if inner.startswith('Array'):
            inner = inner[5:].strip()
        
        types = self._parse_param_types(inner)
        
        if len(types) >= 2:
            return types[:-1], types[-1]
        elif len(types) == 1:
            return [], types[0]
        return [], ''
    
    def replace_function_definitions(
        self, 
        original_content: str,
        function_definitions: List[Tuple[str, str]]
    ) -> Dict:
        """Replace UF definitions"""
        try:
            if not function_definitions:
                return {"success": True, "new_content": original_content, 
                        "error_message": "", "instantiated_functions": []}
            
            parse_result = self.parse_content(original_content)
            uf_names = {uf["name"] for uf in parse_result.get("uninterpreted_functions", [])}
            
            definitions_map = {}
            instantiated = []
            for func_name, define_str in function_definitions:
                definitions_map[func_name] = define_str
                info = self._parse_define_fun(define_str)
                if info.get("name"):
                    instantiated.append(info)
            
            new_content = self._rebuild_with_definitions(original_content, definitions_map, uf_names)
            
            return {"success": True, "new_content": new_content,
                    "error_message": "", "instantiated_functions": instantiated}
        except Exception as e:
            return {"success": False, "new_content": original_content,
                    "error_message": str(e), "instantiated_functions": []}
    
    def _parse_define_fun(self, define_str: str) -> Dict:
        """Parse define-fun string"""
        result = {"name": "", "param_names": [], "param_types": [], 
                  "return_type": "", "body": "", "full_definition": define_str}
        
        try:
            reset_env()
            pysmt_parser = SmtLibParser()
            script = pysmt_parser.get_script(StringIO(define_str + "\n(check-sat)"))
            for cmd in script.commands:
                if cmd.name in ('define-fun', 'define-fun-rec'):
                    symbol = cmd.args[0]
                    result["name"] = symbol.symbol_name()
                    params = cmd.args[1]
                    for p in params:
                        result["param_names"].append(p.symbol_name())
                        result["param_types"].append(str(p.symbol_type()))
                    result["return_type"] = str(cmd.args[2])
                    result["body"] = str(cmd.args[3])
                    break
        except:
            match = re.match(r'\(define-fun(?:-rec)?\s+(\S+)\s*\((.*?)\)\s*(\S+)\s+', 
                            define_str.replace('\n', ' '))
            if match:
                result["name"] = match.group(1)
                result["return_type"] = match.group(3)
                for pm in re.finditer(r'\(\s*(\S+)\s+(\S+)\s*\)', match.group(2)):
                    result["param_names"].append(pm.group(1))
                    result["param_types"].append(pm.group(2))
                result["body"] = self._extract_body(define_str, match.end())
        
        return result
    
    def _extract_body(self, text: str, start: int) -> str:
        depth, pos = 1, start
        while pos < len(text) and depth > 0:
            if text[pos] == '(':
                depth += 1
            elif text[pos] == ')':
                depth -= 1
            pos += 1
        return text[start:pos-1].strip() if pos > start else ""
    
    def _rebuild_with_definitions(self, content: str, definitions_map: Dict[str, str], uf_names: Set[str]) -> str:
        lines = content.split('\n')
        new_lines = []
        i = 0
        
        while i < len(lines):
            line = lines[i]
            if '(declare-fun' in line:
                full, end_i = self._collect_sexp(lines, i)
                match = re.search(r'\(declare-fun\s+(\|[^|]+\||\S+)', full)
                name = match.group(1) if match else ""
                
                if name in definitions_map and name in uf_names:
                    new_lines.append(definitions_map[name])
                else:
                    new_lines.append(full)
                i = end_i + 1
            else:
                new_lines.append(line)
                i += 1
        
        return '\n'.join(new_lines)
    
    def simplify_smt(self, content: str) -> Dict:
        """Simplify SMT file"""
        try:
            reset_env()
            pysmt_parser = SmtLibParser()
            script = pysmt_parser.get_script(StringIO(content))
            
            constants = {}
            all_names = set()
            
            for cmd in script.commands:
                if cmd.name == 'declare-fun':
                    symbol = cmd.args[0]
                    name = symbol.symbol_name()
                    stype = symbol.symbol_type()
                    all_names.add(name)
                    
                    if not (hasattr(stype, 'is_function_type') and stype.is_function_type()):
                        if not (hasattr(stype, 'is_array_type') and stype.is_array_type()):
                            constants[name] = stype
            
            const_values = {}
            try:
                ctx = z3.Context()
                assertions = z3.parse_smt2_string(content, ctx=ctx)
                for a in assertions:
                    if z3.is_eq(a):
                        lhs, rhs = a.arg(0), a.arg(1)
                        self._check_const_assign(lhs, rhs, constants, const_values)
                        self._check_const_assign(rhs, lhs, constants, const_values)
            except:
                pass
            
            var_mapping = self._generate_rename_mapping_pysmt(script, const_values, all_names)
            simplified = self._rebuild_simplified(content, var_mapping, {k: str(v) for k, v in const_values.items()})
            
            return {"success": True, "simplified_content": simplified, "error_message": "",
                    "variable_mapping": var_mapping,
                    "constant_substitutions": {k: str(v) for k, v in const_values.items()}}
        except Exception as e:
            return {"success": False, "simplified_content": content, "error_message": str(e),
                    "variable_mapping": {}, "constant_substitutions": {}}
    
    def _check_const_assign(self, expr, value, constants, result):
        if (z3.is_const(expr) and expr.decl().name() in constants 
            and (z3.is_int_value(value) or z3.is_rational_value(value))
            and expr.decl().name() not in result):
            result[expr.decl().name()] = value
    
    def _generate_rename_mapping_pysmt(self, script, const_values, all_names) -> Dict[str, str]:
        THRESHOLD = 8
        counter = [0]
        mapping = {}
        
        def gen(prefix):
            while f"{prefix}{counter[0]}" in all_names:
                counter[0] += 1
            name = f"{prefix}{counter[0]}"
            all_names.add(name)
            counter[0] += 1
            return name
        
        for cmd in script.commands:
            if cmd.name != 'declare-fun':
                continue
            
            symbol = cmd.args[0]
            name = symbol.symbol_name()
            stype = symbol.symbol_type()
            
            if name in const_values or len(name.strip('|')) <= THRESHOLD:
                continue
            
            if hasattr(stype, 'is_array_type') and stype.is_array_type():
                prefix = 'A'
            elif hasattr(stype, 'is_function_type') and stype.is_function_type():
                prefix = 'F'
            else:
                prefix = 'V'
            mapping[name] = gen(prefix)
        
        return mapping
    
    def _rebuild_simplified(self, content: str, var_mapping: Dict, const_subs: Dict) -> str:
        lines = content.split('\n')
        new_lines = []
        skipped = set()
        i = 0
        
        while i < len(lines):
            line = lines[i]
            
            if '(set-info' in line:
                _, end_i = self._collect_sexp(lines, i)
                i = end_i + 1
                continue
            
            if '(declare-fun' in line:
                full, end_i = self._collect_sexp(lines, i)
                match = re.search(r'\(declare-fun\s+(\|[^|]+\||\S+)', full)
                name = match.group(1) if match else ""
                # Strip bars to match const_subs key
                name_stripped = name.strip('|')
                
                if name_stripped in const_subs:
                    i = end_i + 1
                    continue
                
                new_lines.append(self._apply_subs(full, var_mapping))
                i = end_i + 1
                continue
            
            if '(assert' in line:
                full, end_i = self._collect_sexp(lines, i)
                
                is_const = False
                for v in const_subs:
                    # Check both with and without bars
                    patterns = [f'(= {v} ', f' {v})', f'(= |{v}| ', f' |{v}|)']
                    if v not in skipped and any(p in full for p in patterns):
                        is_const = True
                        skipped.add(v)
                        break
                
                if not is_const:
                    new_assert = self._apply_subs(full, const_subs)
                    remaining = {k: v for k, v in var_mapping.items() if k not in const_subs}
                    new_lines.append(self._apply_subs(new_assert, remaining))
                i = end_i + 1
                continue
            
            new_lines.append(self._apply_subs(line, var_mapping))
            i += 1
        
        return '\n'.join(new_lines)
    
    def _collect_sexp(self, lines: List[str], start: int) -> Tuple[str, int]:
        result = lines[start]
        depth = result.count('(') - result.count(')')
        i = start
        while depth > 0 and i + 1 < len(lines):
            i += 1
            result += '\n' + lines[i]
            depth += lines[i].count('(') - lines[i].count(')')
        return result, i
    
    def _apply_subs(self, text: str, mapping: Dict[str, str]) -> str:
        for old, new in sorted(mapping.items(), key=lambda x: -len(x[0])):
            # Match variable name with bars |name| or without bars name
            pattern_with_bars = r'\|' + re.escape(old) + r'\|'
            pattern_without_bars = r'(?<![a-zA-Z0-9_|])' + re.escape(old) + r'(?![a-zA-Z0-9_])'
            text = re.sub(pattern_with_bars, new, text)
            text = re.sub(pattern_without_bars, new, text)
        return text
    
    def generate_negation_constraint(self, instantiated_functions: List[Dict]) -> str:
        constraints = []
        for func in instantiated_functions:
            if not func.get("name") or not func.get("param_names"):
                continue
            
            params = list(zip(func["param_names"], func.get("param_types", [])))
            exists_vars = " ".join(f"({p}_neg {t})" for p, t in params)
            call_args = " ".join(f"{p}_neg" for p, _ in params)
            
            body = func.get("body", "")
            for p, _ in params:
                body = re.sub(r'(?<![a-zA-Z0-9_])' + re.escape(p) + r'(?![a-zA-Z0-9_])', f"{p}_neg", body)
            
            constraints.append(f"(exists ({exists_vars}) (not (= ({func['name']} {call_args}) {body})))")
        
        if len(constraints) == 1:
            return constraints[0]
        return "(or " + " ".join(constraints) + ")" if constraints else ""
    
    def add_negation_constraint_to_content(self, content: str, constraint: str) -> str:
        if not constraint:
            return content
        insert = f"(assert {constraint})\n"
        return content.replace("(check-sat)", insert + "(check-sat)") if "(check-sat)" in content else content + "\n" + insert
    
    def _error_result(self, msg: str) -> Dict:
        return {"success": False, "logic": None, "uninterpreted_functions": [],
                "array_variables": [], "custom_types": [], "content": "", "error_message": msg}


def main():
    if len(sys.argv) < 3:
        print("Usage:\n  Parse: python smt_parser.py parse <file>\n  Simplify: python smt_parser.py simplify <file>", file=sys.stderr)
        sys.exit(1)
    
    cmd, parser = sys.argv[1], SMTParser()
    
    if cmd == "parse":
        print(json.dumps(parser.parse_file(sys.argv[2]), indent=2, ensure_ascii=False))
    elif cmd == "simplify":
        with open(sys.argv[2], 'r') as f:
            print(json.dumps(parser.simplify_smt(f.read()), indent=2, ensure_ascii=False))
    else:
        print(f"Unknown command: {cmd}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
