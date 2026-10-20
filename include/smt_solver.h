#pragma once

#include <string>
#include <chrono>

namespace linguaarray {

enum class SolverResult {
    UNKNOWN,
    SAT,
    UNSAT,
    TIMEOUT,
    ERROR
};

struct SMTSolveResult {
    SolverResult result = SolverResult::UNKNOWN;
    std::string model;
    std::chrono::milliseconds runtime{0};
    std::string solver_output;
    std::string error_message;
};

class SMTSolver {
public:
    explicit SMTSolver(const std::string& solver_binary = "z3", 
                      const std::string& solver_args = "",
                      int timeout = 30);
    
    SMTSolveResult solve_file(const std::string& file_path);
    SMTSolveResult solve_content(const std::string& smt_content, 
                                const std::string& output_dir = "/tmp",
                                bool save_intermediate = false,
                                const std::string& case_name = "temp",
                                int custom_timeout = -1);
    
    // Check if solver is available
    bool is_solver_available() const;
    
    // Get and set timeout
    int get_timeout() const { return timeout_; }
    void set_timeout(int timeout) { timeout_ = timeout; }

private:
    std::string solver_binary_;
    std::string solver_args_;
    int timeout_;
    
    SMTSolveResult execute_solver(const std::string& file_path, int custom_timeout = -1);
    SolverResult parse_solver_output(const std::string& output);
    std::string build_command(const std::string& file_path, int custom_timeout = -1) const;
};

// Helper function
std::string solver_result_to_string(SolverResult result);

} // namespace linguaarray 
