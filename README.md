# LinguaArray

LLM-guided SMT solving for arrays, uninterpreted functions, and quantified formulas. LinguaArray generates candidate constraints and quantifier instantiations, then checks them with an SMT solver.

## Build

Requires a C++17 compiler, CMake 3.10+, libcurl, Python 3, and GNU `timeout`. Install Z3 or cvc5 as the solver backend.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install pysmt z3-solver
./build.sh
```

## Usage

Set `llm_api_key`, `llm_api_base`, `llm_model`, and `solver_binary` in `linguaarray.conf`, then run:

```sh
./build/linguaarray --config linguaarray.conf input.smt2
```

The LLM endpoint must support the OpenAI Chat Completions API.

Run the solver directly:

```sh
./build/linguaarray --solver-only --solver /path/to/z3 input.smt2
```

Generate constraints and solve separately:

```sh
./build/linguaarray --config linguaarray.conf --generate-json input.smt2 output/constraints.json
./build/linguaarray --config linguaarray.conf --solve-with-json input.smt2 output/constraints_sat.json
```

See `./build/linguaarray --help` for all options.
