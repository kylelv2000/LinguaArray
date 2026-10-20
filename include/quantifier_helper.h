#pragma once

#include <string>

namespace linguaarray {

/**
 * Quantifier Instantiation Helper Class
 * 
 * Provides static methods to access Python quantifier instantiation functionality
 * Internally uses QuantifierInstantiationAdapter
 */
class QuantifierHelper {
public:
    /**
     * Parse quantifiers in SMT file
     * 
     * @param smt_file SMT file path
     * @return JSON string {"has_quantifiers": bool, "llm_prompt": str}
     */
    static std::string parse_quantifiers(const std::string& smt_file);
    
    /**
     * Instantiate quantifiers based on LLM suggestions
     * 
     * @param smt_file Original SMT file
     * @param instantiation_json LLM's JSON response
     * @param output_file Output file path
     * @return Instantiated file path, or empty string on failure
     */
    static std::string instantiate_quantifiers(
        const std::string& smt_file,
        const std::string& instantiation_json,
        const std::string& output_file
    );
    
    /**
     * Check if Python is available
     */
    static bool check_python_available();
};

} // namespace linguaarray

