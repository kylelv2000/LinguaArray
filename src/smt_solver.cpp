#include "smt_solver.h"
#include "config.h"
#include <fstream>
#include <cstdio>
#include <memory>
#include <array>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <chrono>

namespace linguaarray {

SMTSolver::SMTSolver(const std::string& solver_binary, const std::string& solver_args, int timeout)
    : solver_binary_(solver_binary), solver_args_(solver_args), timeout_(timeout) {
}

SMTSolveResult SMTSolver::solve_file(const std::string& file_path) {
    auto start = std::chrono::high_resolution_clock::now();
    
    if (!std::filesystem::exists(file_path)) {
        SMTSolveResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "File does not exist: " + file_path;
        return result;
    }
    
    auto solve_result = execute_solver(file_path);
    
    auto end = std::chrono::high_resolution_clock::now();
    solve_result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    return solve_result;
}

SMTSolveResult SMTSolver::solve_content(const std::string& smt_content, 
                                        const std::string& output_dir,
                                        bool save_intermediate,
                                        const std::string& case_name,
                                        int custom_timeout) {
    auto start = std::chrono::high_resolution_clock::now();
    
    // Generate timestamp to ensure filename uniqueness
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    // Create meaningful temp filename
    std::string temp_file;
    if (save_intermediate) {
        // When saving intermediate files, use output directory with meaningful naming
        std::filesystem::create_directories(output_dir);
        if (custom_timeout > 0) {
            temp_file = output_dir + "/" + case_name + "_timeout" + std::to_string(custom_timeout) + 
                       "_solver_input_" + std::to_string(timestamp) + ".smt2";
        } else {
            temp_file = output_dir + "/" + case_name + "_solver_input_" + std::to_string(timestamp) + ".smt2";
        }
    } else {
        // When not saving intermediate files, use temp directory
        if (custom_timeout > 0) {
            temp_file = "/tmp/linguaarray_timeout" + std::to_string(custom_timeout) + 
                       "_" + case_name + "_" + std::to_string(timestamp) + ".smt2";
        } else {
            temp_file = "/tmp/linguaarray_" + case_name + "_" + std::to_string(timestamp) + ".smt2";
        }
    }
    
    std::ofstream file(temp_file);
    if (!file.is_open()) {
        SMTSolveResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "Unable to create temp file: " + temp_file;
        return result;
    }
    
    file << smt_content;
    file.close();
    
    if (!std::filesystem::exists(temp_file)) {
        SMTSolveResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "File does not exist: " + temp_file;
        return result;
    }
    
    auto solve_result = execute_solver(temp_file, custom_timeout);
    
    auto end = std::chrono::high_resolution_clock::now();
    solve_result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Decide whether to keep file based on save_intermediate config
    if (!save_intermediate) {
        std::filesystem::remove(temp_file);
    } else {
        if (g_config && g_config->log_level >= LogLevel::INFO) {
            std::cout << "Intermediate SMT file saved: " << temp_file << std::endl;
        }
    }
    
    return solve_result;
}

bool SMTSolver::is_solver_available() const {
    // Try to execute solver's help command
    std::string test_command = solver_binary_ + " --help > /dev/null 2>&1";
    int result = std::system(test_command.c_str());
    return (result == 0);
}

SMTSolveResult SMTSolver::execute_solver(const std::string& file_path, int custom_timeout) {
    SMTSolveResult result;
    
    // Build command, use custom timeout or default timeout
    std::string command = build_command(file_path, custom_timeout);
    
    // Execute command
    std::array<char, 128> buffer;
    std::string output;
    
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    
    if (!pipe) {
        result.result = SolverResult::ERROR;
        result.error_message = "Unable to execute solver command: " + command;
        return result;
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        output += buffer.data();
    }
    
    int exit_code = pclose(pipe.release()) / 256;  // Get exit code
    
    result.solver_output = output;
    
    // Check if timeout
    if (exit_code == 124) {  // timeout command's timeout exit code
        int used_timeout = (custom_timeout > 0) ? custom_timeout : timeout_;
        result.result = SolverResult::TIMEOUT;
        result.error_message = "Solver timeout (" + std::to_string(used_timeout) + "s)";
    } else if (exit_code != 0) {
        result.result = SolverResult::ERROR;
        result.error_message = "Solver execution error, exit code: " + std::to_string(exit_code);
    } else {
        result.result = parse_solver_output(output);
    }
    
    return result;
}

std::string SMTSolver::build_command(const std::string& file_path, int custom_timeout) const {
    std::string command;
    
    // Determine timeout to use
    int used_timeout = (custom_timeout > 0) ? custom_timeout : timeout_;
    
    // Add timeout control
#ifdef _WIN32
    // Windows timeout command
    command = "timeout /t " + std::to_string(used_timeout) + " ";
#else
    // Unix/Linux timeout command
    command = "timeout " + std::to_string(used_timeout) + " ";
#endif
    
    // Add solver binary
    command += solver_binary_;
    
    // Add user custom arguments
    if (!solver_args_.empty()) {
        command += " " + solver_args_;
    }
    
    // Add input file
    command += " " + file_path;
    
    return command;
}

SolverResult SMTSolver::parse_solver_output(const std::string& output) {
    std::string lower_output = output;
    std::transform(lower_output.begin(), lower_output.end(), lower_output.begin(), ::tolower);
    
    if (lower_output.find("unsat") != std::string::npos) {
        return SolverResult::UNSAT;
    } else if (lower_output.find("sat") != std::string::npos) {
        return SolverResult::SAT;
    } else if (lower_output.find("unknown") != std::string::npos) {
        return SolverResult::UNKNOWN;
    } else {
        return SolverResult::ERROR;
    }
}

std::string solver_result_to_string(SolverResult result) {
    switch (result) {
        case SolverResult::SAT: return "sat";
        case SolverResult::UNSAT: return "unsat";
        case SolverResult::UNKNOWN: return "unknown";
        case SolverResult::TIMEOUT: return "timeout";
        case SolverResult::ERROR: return "error";
        default: return "unknown";
    }
}

} // namespace linguaarray 
