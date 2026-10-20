#!/usr/bin/env python3
"""
Quantifier Instantiation Module
Handles forall and not-exists quantifiers nested in and/or expressions
"""

import z3
import json
import sys
import re
from typing import List, Dict, Tuple

_var_counter = 0
_quantifier_id = 0

def get_unique_var(base: str) -> str:
    """Generate unique variable name"""
    global _var_counter
    _var_counter += 1
    return f"_v{_var_counter}_{base}"


def find_quantifiers(expr, neg_count: int, results: List, assert_idx: int):
    """
    Recursively find quantifiers, tracking the number of preceding negations
    
    - forall + even number of negations -> can instantiate (accelerate UNSAT)
    - exists + odd number of negations -> can instantiate (equivalent to not exists)
    """
    global _quantifier_id
    
    if z3.is_quantifier(expr):
        is_forall = expr.is_forall()
        # Check if suitable for instantiation
        # forall + even negations, or exists + odd negations
        suitable = (is_forall and neg_count % 2 == 0) or (not is_forall and neg_count % 2 == 1)
        
        if suitable:
            variables = []
            for i in range(expr.num_vars()):
                variables.append({
                    'name': expr.var_name(i),
                    'type': str(expr.var_sort(i))
                })
            
            qtype = 'forall' if is_forall else 'not_exists'
            results.append({
                'id': _quantifier_id,
                'assert_idx': assert_idx,
                'type': qtype,
                'vars': variables,
                'body': str(expr.body()),
                'neg_count': neg_count,
                '_expr': expr
            })
            _quantifier_id += 1
        
        # Continue recursing into body (may have nested quantifiers)
        find_quantifiers(expr.body(), neg_count, results, assert_idx)
    
    elif z3.is_not(expr):
        # Encountered not, increment negation count
        find_quantifiers(expr.children()[0], neg_count + 1, results, assert_idx)
    
    elif z3.is_and(expr) or z3.is_or(expr):
        # Encountered and/or, recurse into all children
        for child in expr.children():
            find_quantifiers(child, neg_count, results, assert_idx)
    
    elif z3.is_app(expr):
        # Other expressions, recurse into children
        for child in expr.children():
            find_quantifiers(child, neg_count, results, assert_idx)


def parse_smt_asserts(smt_file: str) -> Dict:
    """Parse SMT2 file and find all instantiable quantifiers"""
    global _quantifier_id
    _quantifier_id = 0
    
    with open(smt_file, 'r') as f:
        content = f.read()
    
    ctx = z3.Context()
    assertions = z3.parse_smt2_string(content, ctx=ctx)
    
    # Extract all declaration statements (set-logic, declare-fun, declare-const, etc.)
    # Handle multi-line cases by matching parentheses to find complete statements
    decl_stmts = []
    i = 0
    while i < len(content):
        # Skip whitespace
        while i < len(content) and content[i].isspace():
            i += 1
        
        if i >= len(content):
            break
        
        # Only process top-level '('
        if content[i] == '(':
            # Find the keyword for this parenthesis
            j = i + 1
            while j < len(content) and content[j].isspace():
                j += 1
            
            # Extract keyword
            keyword_start = j
            while j < len(content) and not content[j].isspace() and content[j] != ')':
                j += 1
            keyword = content[keyword_start:j]
            
            # Match parentheses to find complete top-level statement
            depth = 1
            k = i + 1
            while k < len(content) and depth > 0:
                if content[k] == '(':
                    depth += 1
                elif content[k] == ')':
                    depth -= 1
                k += 1
            
            # If declaration statement, save it
            if keyword in ['set-logic', 'declare-fun', 'declare-const', 'declare-sort', 'declare-datatype']:
                if depth == 0:
                    decl_stmts.append(content[i:k])
            
            # Skip entire top-level expression regardless (O(n) instead of O(n^2))
            i = k
        else:
            i += 1
    
    decl_part = '\n'.join(decl_stmts)
    
    # Collect declared variable names (only names, not full definitions)
    # declared: Set[str] - for quick lookup to avoid duplicate declarations
    declared = set(re.findall(r'\(declare-(?:fun|const)\s+(\S+)', content))
    
    # Recursively find all quantifiers
    instantiable = []
    for idx, assertion in enumerate(assertions):
        find_quantifiers(assertion, 0, instantiable, idx)
    
    return {
        'file': smt_file,
        'content': content,
        'decl_part': decl_part,
        'declared': declared,
        'instantiable': instantiable,
        '_assertions': assertions,
        '_ctx': ctx
    }


def replace_quantifier_in_expr(expr, target_expr, replacement):
    """Recursively replace quantifier in expression with instantiated result"""
    if z3.eq(expr, target_expr):
        return replacement
    
    if z3.is_app(expr):
        children = expr.children()
        if not children:
            return expr
        
        new_children = [replace_quantifier_in_expr(c, target_expr, replacement) for c in children]
        # Check if any changes were made
        if any(not z3.eq(c1, c2) for c1, c2 in zip(children, new_children)):
            return expr.decl()(*new_children)
    
    return expr


def instantiate_with_witness(parse_result: Dict, witnesses: List[Dict]) -> Tuple[str, List[str]]:
    """
    Instantiate quantifiers based on witness - replace quantifier in-place with instantiated body
    
    Format: [{"quantifier_id": 0, "witness_map": {...}}]
    """
    ctx = parse_result['_ctx']
    decl_part = parse_result['decl_part']
    instantiable = parse_result['instantiable']
    assertions = list(parse_result['_assertions'])  # Make a copy
    existing_decls = parse_result.get('declared', set())
    
    new_decls = []
    local_decls = set()
    modified_asserts = set()  # Track modified assert indices
    
    for w in witnesses:
        q_id = w.get('quantifier_id')
        witness_map = w.get('witness_map', {})
        
        # Find corresponding quantifier
        info = next((i for i in instantiable if i['id'] == q_id), None)
        if not info:
            continue
        
        expr = info['_expr']
        assert_idx = info['assert_idx']
        variables = info['vars']
        
        # Check if witness covers all variables
        var_names = {v['name'] for v in variables}
        if not var_names.issubset(set(witness_map.keys())):
            continue
        
        # Build substitution list (Z3's substitute_vars uses reverse order)
        subs = []
        skip_quantifier = False
        for i in range(expr.num_vars()):
            var_name = expr.var_name(expr.num_vars() - 1 - i)
            var_sort = expr.var_sort(expr.num_vars() - 1 - i)
            val_str = witness_map.get(var_name, '')
            
            # Parse witness value
            sub_expr = None
            if val_str.lstrip('-').isdigit():
                sub_expr = z3.IntVal(int(val_str), ctx)
            elif val_str.startswith('(') or val_str in existing_decls or val_str in local_decls:
                try:
                    sort_str = var_sort.sexpr()
                    temp = f"{decl_part}(declare-const __tmp__ {sort_str})\n(assert (= __tmp__ {val_str}))\n"
                    parsed = z3.parse_smt2_string(temp, ctx=ctx)
                    if parsed and z3.is_eq(parsed[-1]):
                        sub_expr = parsed[-1].children()[1]
                except Exception as e:
                    import sys
                    print(f"Warning: Failed to parse witness '{val_str}': {e}", file=sys.stderr)
            
            if sub_expr is None:
                if val_str.startswith('('):
                    import sys
                    print(f"Error: Cannot parse expression '{val_str}', skipping this quantifier", file=sys.stderr)
                    skip_quantifier = True
                    break
                # Treat as new variable name
                new_var = val_str if val_str else get_unique_var(var_name)
                if new_var not in existing_decls and new_var not in local_decls:
                    new_decls.append(f"(declare-const {new_var} {var_sort.sexpr()})")
                    local_decls.add(new_var)
                sub_expr = z3.Const(new_var, var_sort)
            
            subs.append(sub_expr)
        
        if skip_quantifier:
            continue
        
        # Substitute variables to get instantiated body
        body = expr.body()
        instantiated = z3.substitute_vars(body, *subs) if len(subs) > 1 else z3.substitute_vars(body, subs[0])
        
        # Replace quantifier with instantiated body in original assert
        assertions[assert_idx] = replace_quantifier_in_expr(assertions[assert_idx], expr, instantiated)
        modified_asserts.add(assert_idx)
    
    # Generate new SMT content
    # Preserve declaration part from original file
    new_content = decl_part.rstrip() + '\n'
    
    # Add new declarations
    for decl in new_decls:
        new_content += decl + '\n'
    
    # Add all asserts (using modified versions)
    for assertion in assertions:
        new_content += f"(assert {assertion.sexpr()})\n"
    
    # Preserve tail commands from original file (check-sat, exit, etc.)
    original = parse_result['content']
    tail_match = re.search(r'\(check-sat\).*', original, re.DOTALL)
    if tail_match:
        new_content += tail_match.group()
    else:
        new_content += "(check-sat)\n"
    
    return new_content, [str(i) for i in modified_asserts]


def generate_llm_prompt(parse_result: Dict) -> str:
    """Generate LLM prompt, consistent with C++ code format"""
    instantiable = parse_result['instantiable']
    if not instantiable:
        return ""
    
    smt_content = parse_result['content']
    
    # Build quantifier info JSON
    quantifier_info = {
        'quantifiers': [{
            'id': info['id'],
            'type': info['type'],
            'variables': info['vars'],
            'body': info['body'],
            'assert_idx': info['assert_idx'],
            'suitable_for_instantiation': True
        } for info in instantiable],
        'constants': list(parse_result.get('declared', set()))
    }
    quantifier_info_json = json.dumps(quantifier_info, indent=2, ensure_ascii=False)
    
    prompt = f"""# UNSAT Acceleration: Quantifier Instantiation

## Objective
Accelerate UNSAT proof through semantic reasoning and quantifier instantiation. If the instantiated problem is UNSAT, the original problem must be UNSAT.

## Strategy
1. **Deep Analysis**: Understand the internal structure and logical relationships
2. **Identify Contradictions**: Find key conflicts that may lead to UNSAT
3. **Quantifier Instantiation**: Replace quantified variables in forall/not-exists with concrete values or expressions
4. **Soundness**: Instantiation must strengthen the problem (easier to prove UNSAT), not introduce new satisfiability

## SMT Problem
```smt2
{smt_content}
```

## Quantifier Information
```json
{quantifier_info_json}
```

## Instantiation Principles (Witness Method)
For both **forall** and **not-exists** quantifiers, provide concrete witness to instantiate:

- **Witness types**: concrete values, existing variables, or SMT expressions
  * Constants: "0", "1", "(_ bv0 32)"
  * Declared variables: "existingVar", "main_~p~0.base"
  * Expressions: "(select arr idx)", "(store arr i v)", "(bvadd x y)"

- **Example**:
  * `(forall ((x Int)) (> (f x) 0))` with witness_map: {{"x": "(+ a 1)"}}
  * Result: `(> (f (+ a 1)) 0)`

- **Multi-variable quantifiers**: Provide witness for each variable
  * witness_map: {{"v1": "expr1", "v2": "expr2", ...}}

- **Selection Strategy**: 
  * Prefer existing declared variables/constants
  * Use expressions that expose contradictions
  * Avoid introducing new declarations unless necessary

## Output Format (Using Witness Map)
Return a JSON object with witness_map for each variable:
```json
{{
  "success": true,
  "reasoning": "(200-400 tokens max, do NOT overthink). Detailed derivation: analyze constraint structure, identify contradiction points...",
  "instantiations": [
    {{
      "quantifier_id": 0,
      "witness_map": {{
        "x": "0",
        "y": "existingConst"
      }},
      "confidence": 0.85,
      "new_declarations": {{}}
    }},
    {{
      "quantifier_id": 1,
      "witness_map": {{
        "v_arr": "(select oldArray idx)",
        "v_val": "(select someArray x)"
      }},
      "confidence": 0.8
    }}
  ],
  "unsat_prediction": "high"
}}
```

**Key Notes**:
- `quantifier_id`: Quantifier ID (from the `id` field in quantifier info JSON)
- `witness_map`: Dictionary mapping variable names to witness expressions
  * Simple values: "0", "1", "someConst"
  * Select expressions: "(select arrayName index)"
  * Store expressions: "(store arrayName index value)"
  * Other SMT-LIB expressions

## Valid Witness Examples
- Simple values: "0", "1", "(_ bv0 32)", "(_ bv255 8)"
- Declared variables: "existingConst", "c_main_~p~0.base"
- Select: "(select arrayName index)"
- Store: "(store arrayName index value)"
- Arithmetic: "(+ a 1)", "(bvadd x y)", "(bvmul a b)"
- Nested: "(select (select mem base) offset)"

## Instructions
Brief thinking (200-400 tokens max, do NOT overthink), then return ONLY the JSON object."""
    
    return prompt


def validate_llm_response(parse_result: Dict, response: str) -> Tuple[str, List[str]]:
    """Validate LLM response and apply"""
    try:
        # Extract JSON object
        match = re.search(r'\{.*\}', response, re.DOTALL)
        if not match:
            return parse_result['content'], []
        
        data = json.loads(match.group())
        if not isinstance(data, dict) or 'instantiations' not in data:
            return parse_result['content'], []
        
        witnesses = data['instantiations']
        if not isinstance(witnesses, list):
            return parse_result['content'], []
        
        # Filter valid witnesses
        valid = [w for w in witnesses if isinstance(w, dict) 
                 and 'quantifier_id' in w and isinstance(w.get('witness_map'), dict)]
        
        if not valid:
            return parse_result['content'], []
        
        return instantiate_with_witness(parse_result, valid)
    except:
        return parse_result['content'], []


def main():
    """
    Usage:
      Parse: python quantifier_instantiation.py <smt2_file>
            -> Output JSON {llm_prompt: "...", has_quantifiers: true/false}
      
      Instantiate: python quantifier_instantiation.py <smt2_file> <llm_response_json> <output_smt2>
            -> Generate instantiated SMT2 file
    """
    if len(sys.argv) < 2:
        print("Usage:", file=sys.stderr)
        print("  Parse: python quantifier_instantiation.py <smt2_file>", file=sys.stderr)
        print("  Instantiate: python quantifier_instantiation.py <smt2_file> <llm_response_json> <output_smt2>", file=sys.stderr)
        sys.exit(1)
    
    smt_file = sys.argv[1]
    result = parse_smt_asserts(smt_file)
    
    if len(sys.argv) == 2:
        # Parse mode: output prompt for LLM use
        has_quantifiers = len(result['instantiable']) > 0
        output = {
            'has_quantifiers': has_quantifiers,
            'llm_prompt': generate_llm_prompt(result) if has_quantifiers else ""
        }
        print(json.dumps(output, ensure_ascii=False))
    else:
        # Instantiate mode: generate SMT2 file
        llm_response_file = sys.argv[2]
        output_file = sys.argv[3] if len(sys.argv) > 3 else smt_file.replace('.smt2', '_instantiated.smt2')
        
        with open(llm_response_file, 'r') as f:
            llm_response = f.read()
        
        # Parse LLM response
        new_content, new_asserts = validate_llm_response(result, llm_response)
        
        # Write output file
        with open(output_file, 'w') as f:
            f.write(new_content)
        
        print(json.dumps({
            'success': len(new_asserts) > 0,
            'output_file': output_file,
            'instantiated_count': len(new_asserts)
        }))


if __name__ == '__main__':
    main()
