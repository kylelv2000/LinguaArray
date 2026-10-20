#include "config.h"
#include "engine.h"
#include <iostream>
#include <filesystem>
#include <vector>
#include <string>

using namespace linguaarray;

void print_usage(const char* program_name) {
    std::cout << "LinguaArray - LLM-enhanced SMT Solver with Array Logic\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " [options] <smt_file>\n";
    std::cout << "  " << program_name << " --generate-json <smt_file> <output_json>\n";
    std::cout << "  " << program_name << " --solve-with-json <smt_file> <json_file>\n";
    std::cout << "\nBasic Options:\n";
    std::cout << "  --help, -h              Show this help message\n";
    std::cout << "  --config <file>         Specify config file path\n";
    std::cout << "  --print-config          Print current configuration\n";

    
    std::cout << "\nRun Modes:\n";
    std::cout << "  --mode <mode>           Run mode (full|solver-only|generate-json|solve-with-json)\n";
    std::cout << "  --solver-only           Run solver only\n";
    std::cout << "  --generate-json         Generate LLM constraint JSON (auto simplify and save)\n";
    std::cout << "  --solve-with-json       Solve with JSON file (auto use simplified file)\n";
    
    std::cout << "\nLog Options:\n";
    std::cout << "  --log-level <level>     Log level (quiet|error|warn|info|debug|verbose)\n";
    std::cout << "  --verbose, -v           Verbose output (equals --log-level verbose)\n";
    std::cout << "  --quiet, -q             Quiet mode (equals --log-level quiet)\n";
    std::cout << "  --debug                 Debug mode (equals --log-level debug)\n";
    
    std::cout << "\nLLM Options:\n";
    std::cout << "  --llm-model <model>     LLM model name\n";
    std::cout << "  --llm-api-key <key>     LLM API key\n";
    std::cout << "  --llm-temperature <val> LLM temperature (0.0-2.0)\n";
    std::cout << "  --llm-timeout <sec>     LLM request timeout\n";
    std::cout << "  --llm-intermediate-timeout <sec>  LLM intermediate validation solver timeout\n";
    
    std::cout << "\nSolver Options:\n";
    std::cout << "  --solver <path>         Solver binary file path\n";
    std::cout << "  --solver-args <args>    Solver extra arguments\n";
    std::cout << "  --timeout <sec>         Solver timeout\n";
    std::cout << "  --solver-timeout <sec>  Solver timeout (same as --timeout)\n";
    
    std::cout << "\nOutput Options:\n";
    std::cout << "  --output-dir <dir>      Output directory\n";
    std::cout << "  --save-intermediate     Save intermediate result files\n";
    std::cout << "  --no-timing             Don't show execution time\n";
    std::cout << "  --result-only           Only output final result (sat|unsat|unknown|timeout|error)\n";
    
    std::cout << "\nEnvironment Variables:\n";
    std::cout << "  LINGUAARRAY_LLM_API_KEY   LLM API key\n";
    std::cout << "  OPENAI_API_KEY          OpenAI API key (compatible)\n";
    std::cout << "  LINGUAARRAY_LLM_MODEL     LLM model name\n";
    std::cout << "  LINGUAARRAY_SOLVER_BINARY Solver binary path\n";
    std::cout << "  LINGUAARRAY_LOG_LEVEL     Log level\n";
    std::cout << "  VERBOSE                 Verbose output (compatible)\n";
    
    std::cout << "\nConfig File:\n";
    std::cout << "  Default config file: linguaarray.conf\n";
    std::cout << "  Priority: Command line args > Environment vars > Config file > Defaults\n";
    
    std::cout << "\nExamples:\n";
    std::cout << "  # Basic usage\n";
    std::cout << "  " << program_name << " test/example.smt2\n\n";
    std::cout << "  # Solver only mode\n";
    std::cout << "  " << program_name << " --solver-only test/example.smt2\n\n";
    std::cout << "  # Use custom solver\n";
    std::cout << "  " << program_name << " --solver /usr/bin/cvc5 test/example.smt2\n\n";
    std::cout << "  # Generate JSON file (auto simplify)\n";
    std::cout << "  " << program_name << " --generate-json test/example.smt2 output/functions.json\n\n";
    std::cout << "  # Solve with JSON file (auto use simplified file)\n";
    std::cout << "  " << program_name << " --solve-with-json test/example.smt2 output/functions.json\n\n";
    std::cout << "  # Verbose output mode\n";
    std::cout << "  " << program_name << " --verbose test/example.smt2\n\n";
    std::cout << "  # Use config file\n";
    std::cout << "  " << program_name << " --config my_config.conf test/example.smt2\n";
}

void print_result(const LinguaArrayResult& result, const std::string& file_path) {
    if (g_config->result_only_output) {
        std::cout << solver_result_to_string(result.result) << std::endl;
        return;
    }
    
    std::cout << "\n======== Solve Result ========\n";
    std::cout << "File: " << file_path << "\n";
    std::cout << "Result: " << solver_result_to_string(result.result) << "\n";
    if (g_config->show_timing) {
        std::cout << "Time: " << result.runtime.count() << "ms\n";
    }
    std::cout << "LLM Enhanced: " << (result.llm_used ? "Yes" : "No") << "\n";
    if (result.llm_used) {
        std::cout << "Constraints Count: " << result.functions_instantiated << "\n";
    }
    if (result.unsat_accelerated) {
        std::cout << "UNSAT Accelerated: Yes\n";
    }
    if (!result.error_message.empty()) {
        std::cout << "Error Message: " << result.error_message << "\n";
    }
    std::cout << "========================\n\n";
}

int main(int argc, char* argv[]) {
    try {
        // Parse command line arguments
        auto cmd_result = CommandLineParser::parse(argc, argv);
        
        // Handle errors
        if (!cmd_result.error_message.empty()) {
            std::cerr << "Error: " << cmd_result.error_message << "\n\n";
            CommandLineParser::print_usage(argv[0]);
            return 1;
        }
        
        // Handle help
        if (cmd_result.show_help) {
            CommandLineParser::print_usage(argv[0]);
            return 0;
        }
        
        // Set global config
        g_config = std::move(cmd_result.config);
        
        // Handle special commands
        if (cmd_result.print_config) {
            g_config->print_config();
            return 0;
        }
        

        
        // Output config summary
        if (g_config->log_level >= LogLevel::INFO) {
            std::cout << "LinguaArray Config: " << g_config->get_summary() << "\n\n";
        }
        
        // Create engine
        LinguaArrayEngine engine(*g_config);
        
        // Execute based on run mode
        switch (g_config->run_mode) {
            case RunMode::GENERATE_JSON: {
                // Use configured output directory directly
                std::string output_dir_base = g_config->output_dir;

                // If user specified output file, use that path, otherwise generate in main output dir
                std::string final_output_file = cmd_result.output_file;
                if (final_output_file.find(std::filesystem::path::preferred_separator) == std::string::npos) {
                    // If only filename provided, put it in configured output dir
                    final_output_file = (std::filesystem::path(output_dir_base) / final_output_file).string();
                }
                
                // Create output directory
                auto output_dir_path = std::filesystem::path(final_output_file).parent_path();
                if (!output_dir_path.empty()) {
                    std::filesystem::create_directories(output_dir_path);
                }
                
                if (g_config->log_level >= LogLevel::INFO) {
                    std::cout << "Generating LLM constraint JSON...\n";
                }
                bool success = engine.generate_llm_json(cmd_result.input_file, final_output_file);
                
                if (success) {
                    if (g_config->log_level > LogLevel::QUIET) {
                        std::cout << "JSON file generated: " << final_output_file << "\n";
                    }
                    return 0;
                } else {
                    if (g_config->log_level > LogLevel::QUIET) {
                        std::cerr << "JSON file generation failed\n";
                    }
                    return 1;
                }
            }
            
            case RunMode::SOLVE_WITH_JSON: {
                if (g_config->log_level >= LogLevel::INFO) {
                    std::cout << "Solving with JSON file...\n";
                }
                auto result = engine.solve_with_json(cmd_result.input_file, cmd_result.output_file);
                print_result(result, cmd_result.input_file);
                break;
            }
            
            case RunMode::SOLVER_ONLY: {
                if (g_config->log_level >= LogLevel::INFO) {
                    std::cout << "Running solver only...\n";
                }
                
                // Create temp engine config, disable LLM
                auto temp_config = *g_config;
                temp_config.llm.api_key = "";  // Clear API key to disable LLM
                
                LinguaArrayEngine solver_engine(temp_config);
                auto result = solver_engine.solve_file(cmd_result.input_file);
                print_result(result, cmd_result.input_file);
                break;
            }
            
            case RunMode::FULL:
            default: {
                if (g_config->log_level >= LogLevel::INFO) {
                    std::cout << "Running in full mode...\n";
                }
                auto result = engine.solve_file(cmd_result.input_file);
                print_result(result, cmd_result.input_file);
                break;
            }
        }
        
    } catch (const ConfigError& e) {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
} 
