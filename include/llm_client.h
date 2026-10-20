#pragma once

#include <string>
#include <vector>
#include <memory>

namespace linguaarray {

// Forward declaration
struct Config;

// LLM-returned constraint (assertions to accelerate solving)
struct LLMConstraint {
    std::string name;          // Constraint name (descriptive)
    std::string constraint;    // Expression in SMT-LIB format
    std::string type;          // Constraint type: assignment/lemma/property etc.
    double confidence = 0.8;
};

// UF function instantiation definition
struct FunctionInstantiation {
    std::string function_name;
    std::vector<std::string> parameters;
    std::string return_type;
    std::string definition;  // define-fun or define-fun-rec
    double confidence = 0.8;
};

// LLM response structure
struct LLMResponse {
    bool success = false;
    std::string satisfiability;  // LLM-determined satisfiability: sat/unsat/unknown
    std::string chain_of_thought;
    std::vector<LLMConstraint> constraints;
    std::vector<FunctionInstantiation> functions;  // UF definitions
};

class LLMClient {
public:
    explicit LLMClient(const Config& config);
    
    // Generate constraints JSON file
    bool generate_constraints_json(
        const std::string& smt_content,
        const std::vector<struct UninterpretedFunction>& functions,
        const std::vector<struct ArrayVariable>& arrays,
        const std::string& output_file,
        const std::string& original_smt_file = ""  // Original SMT file path for hints matching
    );
    
    // Read LLM response from JSON file
    static LLMResponse read_llm_response_from_json(
        const std::string& json_file
    );
    
    // Call LLM with given prompt and save response to file
    bool call_llm_with_prompt(
        const std::string& prompt,
        const std::string& output_file,
        const std::string& original_smt_file = ""  // For hints matching
    );

private:
    // LLM configuration parameters
    std::string api_key_;
    std::string api_base_;
    std::string model_;
    double temperature_;
    int max_tokens_;
    double timeout_;
    int max_retries_;
    bool enable_streaming_;
    std::string organization_id_;
    std::string system_prompt_;
    std::string function_prompt_;
    std::string optimization_prompt_;
    
    // vLLM local deployment configuration
    bool is_local_vllm_;
    bool force_json_mode_;
    
    // Multi-round iteration hints
    std::string hints_file_;
    
    // Build prompt from hints file
    std::string build_hints_prompt(const std::string& smt_file, bool is_sat_path) const;
    
    std::string build_prompt(
        const std::string& smt_content,
        const std::vector<struct UninterpretedFunction>& functions,
        const std::vector<struct ArrayVariable>& arrays,
        const std::string& original_smt_file = ""
    );
    
    std::string call_openai_api(const std::string& prompt);
    
    // Extract JSON from non-standard response (handle vLLM/DeepSeek special formats)
    std::string extract_json_from_response(const std::string& response);
    
    // Remove <think>...</think> tags
    std::string remove_think_tags(const std::string& content);
    
    // Extract JSON from Markdown code block
    std::string extract_json_from_markdown(const std::string& content);
    
    // Try to fix incomplete or malformed JSON
    std::string try_fix_json(const std::string& content);
    
    // SAT branch JSON fallback extraction (constraints format)
    std::string extract_sat_json_fallback(const std::string& content);
    
    // UNSAT branch JSON fallback extraction (instantiations format)
    std::string extract_unsat_json_fallback(const std::string& content);
};

} // namespace linguaarray 
