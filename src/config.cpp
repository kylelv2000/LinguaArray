#include "config.h"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <iomanip>
#include <chrono>

namespace linguaarray {

std::unique_ptr<Config> g_config;

// Helper function implementations
std::string run_mode_to_string(RunMode mode) {
    switch (mode) {
        case RunMode::FULL: return "full";
        case RunMode::SOLVER_ONLY: return "solver-only";
        case RunMode::GENERATE_JSON: return "generate-json";
        case RunMode::SOLVE_WITH_JSON: return "solve-with-json";
        default: return "unknown";
    }
}

std::string log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::QUIET: return "quiet";
        case LogLevel::ERROR: return "error";
        case LogLevel::WARN: return "warn";
        case LogLevel::INFO: return "info";
        case LogLevel::DEBUG: return "debug";
        case LogLevel::VERBOSE: return "verbose";
        default: return "unknown";
    }
}

RunMode string_to_run_mode(const std::string& str) {
    std::string lower_str = str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
    
    if (lower_str == "full") return RunMode::FULL;
    if (lower_str == "solver-only" || lower_str == "solver_only") return RunMode::SOLVER_ONLY;
    if (lower_str == "generate-json" || lower_str == "generate_json") return RunMode::GENERATE_JSON;
    if (lower_str == "solve-with-json" || lower_str == "solve_with_json") return RunMode::SOLVE_WITH_JSON;
    
    throw ConfigError("Invalid run mode: " + str);
}

LogLevel string_to_log_level(const std::string& str) {
    std::string lower_str = str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
    
    if (lower_str == "quiet" || lower_str == "0") return LogLevel::QUIET;
    if (lower_str == "error" || lower_str == "1") return LogLevel::ERROR;
    if (lower_str == "warn" || lower_str == "warning" || lower_str == "2") return LogLevel::WARN;
    if (lower_str == "info" || lower_str == "3") return LogLevel::INFO;
    if (lower_str == "debug" || lower_str == "4") return LogLevel::DEBUG;
    if (lower_str == "verbose" || lower_str == "5") return LogLevel::VERBOSE;
    
    throw ConfigError("Invalid log level: " + str);
}

// Config class method implementations
/*
std::unique_ptr<Config> Config::load(
    const std::string& config_file,
    const std::vector<std::string>& cmd_args,
    bool load_env
) {
    auto config = std::make_unique<Config>();
    
    // 1. Set defaults
    config->set_defaults();
    
    // 2. Load from config file (if specified)
    if (!config_file.empty() && std::filesystem::exists(config_file)) {
        if (!config->load_from_file(config_file)) {
            std::cerr << "Warning: Unable to load config file " << config_file << std::endl;
        }
    }
    
    // 3. Load from command line args (highest priority)
    if (!cmd_args.empty()) {
        config->load_from_args(cmd_args);
    }
    
    return config;
}
*/

void Config::set_defaults() {
    // Run mode configuration
    run_mode = RunMode::FULL;
    log_level = LogLevel::INFO;
    show_timing = true;
    save_intermediate = false;
    output_dir = "output";
    result_only_output = false;
    
    // LLM configuration defaults
    llm.api_key = "";
    llm.api_base = "https://api.openai.com/v1";
    llm.model = "gpt-4";
    llm.temperature = 0.1;
    llm.max_tokens = 4096;
    llm.timeout = 60.0;
    llm.max_retries = 3;
    llm.intermediate_timeout = 5;
    llm.enable_streaming = false;
    llm.is_local_vllm = false;
    llm.force_json_mode = true;
    
    // Solver configuration defaults
    solver.binary = "";  // No default, user must specify
    solver.timeout = 30;
}

bool Config::load_from_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return false;
    }
    
    std::string line;
    int line_number = 0;
    
    while (std::getline(file, line)) {
        line_number++;
        try {
            if (!parse_config_line(line)) {
                std::cerr << "Warning: Config file line " << line_number << " format error: " << line << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: Config file line " << line_number << ": " << e.what() << std::endl;
            return false;
        }
    }
    
    return true;
}

bool Config::parse_config_line(const std::string& line) {
    std::string trimmed = trim(line);
    
    // Skip empty lines and comments
    if (trimmed.empty() || trimmed[0] == '#') {
        return true;
    }
    
    // Find equals sign
    size_t eq_pos = trimmed.find('=');
    if (eq_pos == std::string::npos) {
        return false;
    }
    
    std::string key = trim(trimmed.substr(0, eq_pos));
    std::string value = trim(trimmed.substr(eq_pos + 1));
    
    // Remove quotes from value
    if (value.length() >= 2 && 
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.length() - 2);
    }
    
    // Parse config items
    try {
        // Run mode configuration
        if (key == "run_mode") {
            run_mode = string_to_run_mode(value);
        } else if (key == "log_level") {
            log_level = string_to_log_level(value);
        } else if (key == "show_timing") {
            show_timing = parse_bool(value);
        } else if (key == "save_intermediate") {
            save_intermediate = parse_bool(value);
        } else if (key == "output_dir") {
            output_dir = value;
        } else if (key == "result_only_output") {
            result_only_output = parse_bool(value);
        }
        
        // LLM configuration
        else if (key == "llm_api_key") {
            llm.api_key = value;
        } else if (key == "llm_api_base") {
            llm.api_base = value;
        } else if (key == "llm_model") {
            llm.model = value;
        } else if (key == "llm_temperature") {
            llm.temperature = std::stod(value);
        } else if (key == "llm_max_tokens") {
            llm.max_tokens = std::stoi(value);
        } else if (key == "llm_timeout") {
            llm.timeout = std::stod(value);
        } else if (key == "llm_max_retries") {
            llm.max_retries = std::stoi(value);
        } else if (key == "llm_intermediate_timeout") {
            llm.intermediate_timeout = std::stoi(value);
        } else if (key == "llm_system_prompt") {
            llm.system_prompt = value;
        } else if (key == "llm_function_prompt") {
            llm.function_prompt = value;
        } else if (key == "llm_optimization_prompt") {
            llm.optimization_prompt = value;
        } else if (key == "llm_enable_streaming") {
            llm.enable_streaming = parse_bool(value);
        } else if (key == "llm_organization_id") {
            llm.organization_id = value;
        } else if (key == "llm_is_local_vllm") {
            llm.is_local_vllm = parse_bool(value);
        } else if (key == "llm_force_json_mode") {
            llm.force_json_mode = parse_bool(value);
        }
        
        // Solver configuration
        else if (key == "solver_binary") {
            solver.binary = value;
        } else if (key == "solver_timeout") {
            solver.timeout = std::stoi(value);
        }
        
        else {
            std::cerr << "Warning: Unknown config item: " << key << std::endl;
            return false;
        }
        
    } catch (const std::exception& e) {
        throw ConfigError("Failed to parse config item '" + key + "': " + e.what());
    }
    
    return true;
}

bool Config::load_from_args(const std::vector<std::string>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        
        // Run mode
        if (arg == "--mode" && i + 1 < args.size()) {
            try { run_mode = string_to_run_mode(args[++i]); } catch (...) {}
        } else if (arg == "--solver-only") {
            run_mode = RunMode::SOLVER_ONLY;
        } else if (arg == "--generate-json") {
            run_mode = RunMode::GENERATE_JSON;
        } else if (arg == "--solve-with-json") {
            run_mode = RunMode::SOLVE_WITH_JSON;
        }
        
        // Log level
        else if (arg == "--log-level" && i + 1 < args.size()) {
            try { log_level = string_to_log_level(args[++i]); } catch (...) {}
        } else if (arg == "--verbose" || arg == "-v") {
            log_level = LogLevel::VERBOSE;
        } else if (arg == "--quiet" || arg == "-q") {
            log_level = LogLevel::QUIET;
        } else if (arg == "--debug") {
            log_level = LogLevel::DEBUG;
        }
        
        // LLM configuration
        else if (arg == "--llm-model" && i + 1 < args.size()) {
            llm.model = args[++i];
        } else if (arg == "--llm-temperature" && i + 1 < args.size()) {
            try { llm.temperature = std::stod(args[++i]); } catch (...) {}
        } else if (arg == "--llm-api-key" && i + 1 < args.size()) {
            llm.api_key = args[++i];
        } else if (arg == "--llm-api-base" && i + 1 < args.size()) {
            llm.api_base = args[++i];
        } else if (arg == "--llm-timeout" && i + 1 < args.size()) {
            try { llm.timeout = std::stod(args[++i]); } catch (...) {}
        } else if (arg == "--llm-intermediate-timeout" && i + 1 < args.size()) {
            try { llm.intermediate_timeout = std::stoi(args[++i]); } catch (...) {}
        } else if (arg == "--llm-local-vllm") {
            llm.is_local_vllm = true;
            llm.force_json_mode = false; // vLLM usually doesn't support JSON mode
        } else if (arg == "--llm-no-json-mode") {
            llm.force_json_mode = false;
        } else if (arg == "--hints-file" && i + 1 < args.size()) {
            llm.hints_file = args[++i];
        }
        
        // Solver configuration
        else if (arg == "--solver" && i + 1 < args.size()) {
            solver.binary = args[++i];
        } else if (arg == "--timeout" && i + 1 < args.size()) {
            try { solver.timeout = std::stoi(args[++i]); } catch (...) {}
        } else if (arg == "--no-fallback") {
            no_fallback = true;
        }
        
        // Output configuration
        else if (arg == "--output-dir" && i + 1 < args.size()) {
            output_dir = args[++i];
        } else if (arg == "--save-intermediate") {
            save_intermediate = true;
        } else if (arg == "--no-timing") {
            show_timing = false;
        }
        
        // Config file
        else if (arg == "--config" && i + 1 < args.size()) {
            // This logic moved to CommandLineParser
            // load_from_file(args[++i]);
            ++i; // Skip filename
        }
    }
    
    return true;
}

bool Config::validate() const {
    std::vector<std::string> errors;
    
    // Validate solver configuration
    if (solver.binary.empty()) {
        errors.push_back("Solver binary cannot be empty");
    }
    if (solver.timeout <= 0) {
        errors.push_back("Solver timeout must be positive");
    }
    
    // Validate LLM configuration
    if (llm.temperature < 0.0 || llm.temperature > 2.0) {
        errors.push_back("LLM temperature should be in range 0.0-2.0");
    }
    if (llm.max_tokens <= 0) {
        errors.push_back("LLM max tokens must be positive");
    }
    if (llm.timeout <= 0.0) {
        errors.push_back("LLM timeout must be positive");
    }
    if (llm.max_retries < 0) {
        errors.push_back("LLM max retries cannot be negative");
    }
    if (llm.intermediate_timeout <= 0) {
        errors.push_back("LLM intermediate timeout must be positive");
    }
    
    // Output errors
    if (!errors.empty()) {
        std::cerr << "Config validation failed:\n";
        for (const auto& error : errors) {
            std::cerr << "  - " << error << "\n";
        }
        return false;
    }
    
    return true;
}

void Config::print_config() const {
    std::cout << "\n======== LinguaArray Configuration ========\n";
    std::cout << "Run mode: " << run_mode_to_string(run_mode) << "\n";
    std::cout << "Log level: " << log_level_to_string(log_level) << "\n";
    std::cout << "Show timing: " << (show_timing ? "Yes" : "No") << "\n";
    std::cout << "Save intermediate: " << (save_intermediate ? "Yes" : "No") << "\n";
    std::cout << "Output directory: " << output_dir << "\n\n";
    
    std::cout << "=== LLM Configuration ===\n";
    std::cout << "API key: " << (llm.api_key.empty() ? "Not set" : "Set") << "\n";
    std::cout << "API endpoint: " << llm.api_base << "\n";
    std::cout << "Model: " << llm.model << "\n";
    std::cout << "Temperature: " << llm.temperature << "\n";
    std::cout << "Max tokens: " << llm.max_tokens << "\n";
    std::cout << "Timeout: " << llm.timeout << "s\n";
    std::cout << "Max retries: " << llm.max_retries << "\n";
    std::cout << "Intermediate timeout: " << llm.intermediate_timeout << "s\n";
    std::cout << "Local vLLM mode: " << (llm.is_local_vllm ? "Yes" : "No") << "\n";
    std::cout << "Force JSON mode: " << (llm.force_json_mode ? "Yes" : "No") << "\n\n";
    
    std::cout << "=== Solver Configuration ===\n";
    std::cout << "Binary: " << solver.binary << "\n";
    std::cout << "Timeout: " << solver.timeout << "s\n";
    
    std::cout << "================================\n\n";
}

std::string Config::get_summary() const {
    std::ostringstream oss;
    oss << "Mode:" << run_mode_to_string(run_mode) 
        << " Log:" << log_level_to_string(log_level)
        << " Solver:" << solver.binary 
        << " Timeout:" << solver.timeout << "s";
    if (!llm.api_key.empty()) {
        oss << " LLM:" << llm.model;
    }
    return oss.str();
}

std::string Config::get_run_mode_string() const {
    return run_mode_to_string(run_mode);
}

std::string Config::get_log_level_string() const {
    return log_level_to_string(log_level);
}

void Config::merge(const Config& other) {
    // Config merge logic can be implemented here
    // Current simple implementation: directly overwrite non-default values
    // May need more complex merge strategy in actual use
}

// Helper method implementations
std::string Config::trim(const std::string& str) const {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> Config::split(const std::string& str, char delimiter) const {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    
    while (std::getline(ss, item, delimiter)) {
        item = trim(item);
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    
    return result;
}

bool Config::parse_bool(const std::string& value) const {
    std::string lower_value = value;
    std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
    
    return lower_value == "true" || lower_value == "1" || lower_value == "yes" || lower_value == "on";
}

LogLevel Config::parse_log_level(const std::string& value) const {
    return string_to_log_level(value);
}

RunMode Config::parse_run_mode(const std::string& value) const {
    return string_to_run_mode(value);
}

// CommandLineParser implementation
CommandLineResult CommandLineParser::parse(int argc, char* argv[]) {
    CommandLineResult result;
    
    // Convert command line args to vector
    std::vector<std::string> cmd_args;
    for (int i = 1; i < argc; ++i) {
        cmd_args.push_back(argv[i]);
    }
    
    // Check if showing help
    for (const auto& arg : cmd_args) {
        if (arg == "--help" || arg == "-h") {
            result.show_help = true;
            return result;
        }
    }
    
    // Create config and set defaults
    result.config = std::make_unique<Config>();
    result.config->set_defaults();
    
    // Find and load config file
    for (size_t i = 0; i < cmd_args.size(); ++i) {
        if (cmd_args[i] == "--config" && i + 1 < cmd_args.size()) {
            result.config_file = cmd_args[i + 1];
            if (std::filesystem::exists(result.config_file)) {
                if (!result.config->load_from_file(result.config_file)) {
                     std::cerr << "Warning: Unable to load config file " << result.config_file << std::endl;
                }
            } else {
                std::cerr << "Warning: Config file does not exist " << result.config_file << std::endl;
            }
            break; // Only load first found config file
        }
    }
    
    // Load command line args (overrides defaults and config file values)
    try {
        result.config->load_from_args(cmd_args);
    } catch (const ConfigError& e) {
        result.error_message = "Config error: " + std::string(e.what());
        return result;
    }
    
    // Handle special commands and flags without values
    for (size_t i = 0; i < cmd_args.size(); ++i) {
        if (cmd_args[i] == "--print-config") {
            result.print_config = true;
            return result; // Return immediately
        } else if (cmd_args[i] == "--save-intermediate") {
            result.config->save_intermediate = true;
        } else if (cmd_args[i] == "--no-timing") {
            result.config->show_timing = false;
        } else if (cmd_args[i] == "--result-only") {
            result.config->result_only_output = true;
            result.config->show_timing = false; // No timing in result-only mode
            result.config->log_level = LogLevel::QUIET; // Set to quiet in result-only mode
        }
    }
    
    // Extract non-option arguments (input file and output file)
    auto non_option_args = extract_non_option_args(cmd_args);
    
    // Determine required arguments based on run mode
    if (result.config->run_mode == RunMode::GENERATE_JSON || 
        result.config->run_mode == RunMode::SOLVE_WITH_JSON) {
        
        if (non_option_args.size() >= 1) {
            result.input_file = non_option_args[0];
        }
        if (non_option_args.size() >= 2) {
            result.output_file = non_option_args[1];
        }
        
        if (result.input_file.empty()) {
            result.error_message = "Input file not specified";
            return result;
        }
        
        if (result.output_file.empty()) {
            if (result.config->run_mode == RunMode::GENERATE_JSON) {
                result.error_message = "Generate JSON mode requires output file";
                return result;
            } else if (result.config->run_mode == RunMode::SOLVE_WITH_JSON) {
                result.error_message = "Solve with JSON mode requires JSON file";
                return result;
            }
        }
    } else {
        // FULL and SOLVER_ONLY modes only need input file
        if (non_option_args.size() >= 1) {
            result.input_file = non_option_args[0];
        }
        
        if (result.input_file.empty()) {
            result.error_message = "Input file not specified";
            return result;
        }
    }
    
    // Validate input file
    if (!result.input_file.empty() && !std::filesystem::exists(result.input_file)) {
        result.error_message = "File does not exist: " + result.input_file;
        return result;
    }
    
    // Validate config
    if (!result.config->validate()) {
        result.error_message = "Config validation failed";
        return result;
    }
    
    return result;
}

void CommandLineParser::print_usage(const char* program_name) {
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
    std::cout << "  --generate-json         Generate LLM function definition JSON\n";
    std::cout << "  --solve-with-json       Solve with JSON file\n";
    
    std::cout << "\nLog Options:\n";
    std::cout << "  --log-level <level>     Log level (quiet|error|warn|info|debug|verbose)\n";
    std::cout << "  --verbose, -v           Verbose output (equals --log-level verbose)\n";
    std::cout << "  --quiet, -q             Quiet mode (equals --log-level quiet)\n";
    std::cout << "  --debug                 Debug mode (equals --log-level debug)\n";
    
    std::cout << "\nLLM Options:\n";
    std::cout << "  --llm-model <model>     LLM model name\n";
    std::cout << "  --llm-api-key <key>     LLM API key\n";
    std::cout << "  --llm-api-base <URL>    LLM API endpoint (e.g. http://localhost:8088/v1)\n";
    std::cout << "  --llm-temperature <val> LLM temperature (0.0-2.0)\n";
    std::cout << "  --llm-timeout <sec>     LLM request timeout\n";
    std::cout << "  --llm-local-vllm        Enable local vLLM mode (auto disables JSON forced mode)\n";
    std::cout << "  --llm-no-json-mode      Disable JSON forced mode (for APIs that don't support response_format)\n";
    
    std::cout << "\nSolver Options (Required):\n";
    std::cout << "  --solver <path>         Solver binary file path (required)\n";
    std::cout << "  --timeout <sec>         Solver timeout\n";
    
    std::cout << "\nOutput Options:\n";
    std::cout << "  --output-dir <dir>      Output directory\n";
    std::cout << "  --save-intermediate     Save intermediate result files\n";
    std::cout << "  --no-timing             Don't show execution time\n";
    
    std::cout << "\nConfig File:\n";
    std::cout << "  Priority: Command line args > Config file > Defaults\n";
    std::cout << "  Use --config to specify config file path\n";
    
    std::cout << "\nExamples:\n";
    std::cout << "  # Basic usage (solver required)\n";
    std::cout << "  " << program_name << " --solver solvers/z3 test/example.smt2\n\n";
    std::cout << "  # Solver only mode\n";
    std::cout << "  " << program_name << " --solver-only --solver solvers/z3 test/example.smt2\n\n";
    std::cout << "  # Use custom solver\n";
    std::cout << "  " << program_name << " --solver /usr/bin/cvc5 test/example.smt2\n\n";
    std::cout << "  # Generate JSON file\n";
    std::cout << "  " << program_name << " --generate-json --solver solvers/z3 test/example.smt2 output/functions.json\n\n";
    std::cout << "  # Solve with JSON file\n";
    std::cout << "  " << program_name << " --solve-with-json --solver solvers/z3 test/example.smt2 output/functions.json\n\n";
    std::cout << "  # Verbose output mode\n";
    std::cout << "  " << program_name << " --verbose --solver solvers/z3 test/example.smt2\n\n";
    std::cout << "  # Use config file\n";
    std::cout << "  " << program_name << " --config my_config.conf test/example.smt2\n";
}

std::vector<std::string> CommandLineParser::extract_non_option_args(const std::vector<std::string>& args) {
    std::vector<std::string> non_option_args;
    
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        
        if (arg[0] != '-') {
            // Non-option argument
            non_option_args.push_back(arg);
        } else if (is_option_with_value(arg) && i + 1 < args.size()) {
            // Skip option value
            ++i;
        }
    }
    
    return non_option_args;
}

bool CommandLineParser::is_option_with_value(const std::string& arg) {
    return arg == "--config" || 
           arg == "--mode" || arg == "--log-level" ||
           arg == "--llm-model" || arg == "--llm-api-key" ||
           arg == "--llm-api-base" ||
           arg == "--llm-temperature" || arg == "--llm-timeout" ||
           arg == "--llm-intermediate-timeout" ||
           arg == "--solver" || arg == "--solver-args" || 
           arg == "--timeout" || arg == "--solver-timeout" ||
           arg == "--output-dir";
}

std::string Config::create_case_output_dir(const std::string& input_file) const {
    // Extract filename (without extension) from input file path
    std::filesystem::path input_path(input_file);
    std::string case_name = input_path.stem().string();
    
    // Create directory in format output_dir/case_name_timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
    
    std::string case_dir = output_dir + "/" + case_name + "_" + ss.str();
    
    // Create directory
    try {
        std::filesystem::create_directories(case_dir);
        if (log_level >= LogLevel::INFO) {
            std::cout << "Created case output directory: " << case_dir << std::endl;
        }
    } catch (const std::exception& e) {
        if (log_level >= LogLevel::ERROR) {
            std::cerr << "Failed to create output directory: " << e.what() << std::endl;
        }
        // If creation fails, return default output directory
        return output_dir;
    }
    
    return case_dir;
}

} // namespace linguaarray
