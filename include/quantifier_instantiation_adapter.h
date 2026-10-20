#pragma once

#include <string>
#include <vector>

namespace linguaarray {

/**
 * Quantifier Instantiation Adapter
 * 
 * Wraps Python quantifier instantiation module, providing C++ interface
 * 
 * Usage:
 *   QuantifierInstantiationAdapter adapter("python_bridge/quantifier_instantiation.py");
 *   auto result = adapter.parse_quantifiers("test.smt2");
 */
class QuantifierInstantiationAdapter {
public:
    /**
     * Constructor
     * @param python_script_path Python script path
     */
    explicit QuantifierInstantiationAdapter(const std::string& python_script_path);
    
    /**
     * Parse quantifiers in SMT file
     * 
     * @param smt_file SMT file path
     * @return JSON string {"has_quantifiers": bool, "llm_prompt": str}
     */
    std::string parse_quantifiers(const std::string& smt_file);
    
    /**
     * Instantiate quantifiers using LLM response
     * 
     * @param smt_file Original SMT file
     * @param llm_response LLM's JSON response
     * @param output_file Output file path
     * @return Instantiated file path, or empty string on failure
     */
    std::string instantiate_quantifiers(
        const std::string& smt_file,
        const std::string& llm_response,
        const std::string& output_file
    );
    
    /**
     * Check if Python is available
     */
    static bool check_python_available();

private:
    std::string python_script_path_;
    
    /**
     * Execute Python command and return output
     */
    std::string exec_python_command(const std::string& cmd);
};

} // namespace linguaarray

