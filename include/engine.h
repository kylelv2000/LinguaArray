#pragma once

#include "config.h"
#include "smt_types.h"
#include "python_parser_adapter.h"
#include "smt_solver.h"
#include "llm_client.h"
#include <chrono>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

namespace linguaarray {

struct LinguaArrayResult {
    SolverResult result;
    std::chrono::milliseconds runtime{0};
    bool llm_used = false;
    int functions_instantiated = 0;
    std::string error_message;
    bool unsat_accelerated = false;  // Whether UNSAT acceleration was used
};

class LinguaArrayEngine {
public:
    explicit LinguaArrayEngine(const Config& config);
    
    // Main solving interface
    LinguaArrayResult solve_file(const std::string& file_path);
    
    // Step-by-step solving interface
    bool generate_llm_json(const std::string& file_path, const std::string& output_json);
    LinguaArrayResult solve_with_json(const std::string& file_path, const std::string& json_file);
    
    // UNSAT acceleration interface
    bool generate_unsat_instantiation_json(const std::string& file_path, const std::string& output_json);
    LinguaArrayResult solve_with_unsat_acceleration(const std::string& file_path);

private:
    const Config& config_;
    std::unique_ptr<PythonParserAdapter> parser_;
    std::unique_ptr<SMTSolver> solver_;
    std::unique_ptr<LLMClient> llm_client_;
    
    LinguaArrayResult solve_with_llm(const SMTParseResult& parse_result, const std::string& file_path);
    LinguaArrayResult solve_direct(const std::string& smt_content, const std::string& file_path = "", const std::string& json_file = "");
    
    // UNSAT acceleration helper function
    void unsat_acceleration_thread(
        const std::string& smt_file,
        const std::string& case_name,
        std::atomic<bool>& finished,
        std::atomic<bool>& unsat_found,
        std::mutex& result_mutex,
        LinguaArrayResult& unsat_result
    );
};

} // namespace linguaarray 
