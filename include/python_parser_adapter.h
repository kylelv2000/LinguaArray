#pragma once

#include <string>
#include <vector>
#include <fstream>
#include "smt_types.h"

namespace linguaarray {

/**
 * Python Parser Adapter
 * 
 * Uses Python+z3py for SMT parsing, providing the same interface as C++ SMTParser
 * 
 * Advantages:
 * - Based on mature z3-solver library, high robustness
 * - Supports all SMT-LIB logics (no Unknown logic warnings)
 * - Automatically handles all SMT-LIB formats (BitVec, Array, special identifiers, etc.)
 * - Clean code, easy to maintain
 * 
 * Usage:
 *   PythonParserAdapter parser("parser/smt_parser.py");
 *   auto result = parser.parse_file("test.smt2");
 */
class PythonParserAdapter {
public:
    /**
     * Constructor
     * @param python_script_path Path to the Python parsing script
     */
    explicit PythonParserAdapter(const std::string& python_script_path);
    
    /**
     * Parse SMT file
     * 
     * Aligns with C++ SMTParser::parse_file interface
     * 
     * @param filepath File path
     * @return SMTParseResult Parsing result
     */
    SMTParseResult parse_file(const std::string& filepath);
    
    /**
     * Parse SMT content
     * 
     * Aligns with C++ SMTParser::parse_content interface
     * 
     * @param content SMT content string
     * @return SMTParseResult Parsing result
     */
    SMTParseResult parse_content(const std::string& content);
    
    /**
     * Replace function definitions
     * 
     * Aligns with C++ SMTParser::replace_function_definitions interface
     * 
     * @param original_content Original SMT content
     * @param function_definitions Function definition list (function name, define-fun definition)
     * @return ReplaceResult Replacement result
     */
    ReplaceResult replace_function_definitions(
        const std::string& original_content,
        const std::vector<std::pair<std::string, std::string>>& function_definitions
    );
    
    /**
     * Generate negation constraint
     * 
     * Aligns with C++ SMTParser::generate_negation_constraint interface
     * 
     * @param instantiated_functions Instantiated function information
     * @return Negation constraint string
     */
    std::string generate_negation_constraint(
        const std::vector<InstantiatedFunction>& instantiated_functions
    );
    
    /**
     * Add negation constraint to SMT content
     * 
     * Aligns with C++ SMTParser::add_negation_constraint_to_content interface
     * 
     * @param original_content Original SMT content
     * @param negation_constraint Negation constraint
     * @return SMT content with added constraint
     */
    std::string add_negation_constraint_to_content(
        const std::string& original_content,
        const std::string& negation_constraint
    );
    
    /**
     * Simplify SMT file
     * 
     * Aligns with C++ SMTParser::simplify_smt interface
     * 
     * @param content Original SMT content
     * @return SimplifyResult Simplification result
     */
    SimplifyResult simplify_smt(const std::string& content);

private:
    std::string python_script_path_;
    
    /**
     * Execute Python command and return output
     */
    std::string exec_python_command(const std::string& cmd);
};

} // namespace linguaarray

