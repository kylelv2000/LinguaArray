#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>

namespace linguaarray {

// Run mode enumeration
enum class RunMode {
    FULL,           // Full run: LLM + solver
    SOLVER_ONLY,    // Run solver only
    GENERATE_JSON,  // Generate JSON file
    SOLVE_WITH_JSON // Solve with JSON file
};

// Log level
enum class LogLevel {
    QUIET = 0,    // Silent mode
    ERROR = 1,    // Errors only
    WARN = 2,     // Warnings and above
    INFO = 3,     // Info and above
    DEBUG = 4,    // Debug info
    VERBOSE = 5   // Verbose output
};

struct Config {
    // === Run Mode Configuration ===
    RunMode run_mode = RunMode::FULL;
    LogLevel log_level = LogLevel::INFO;
    bool show_timing = true;
    bool save_intermediate = false;
    std::string output_dir = "output";
    bool result_only_output = false;
    
    // === LLM Configuration ===
    struct {
        std::string api_key;
        std::string api_base = "https://api.openai.com/v1";  // Support custom API endpoint
        std::string model = "gpt-4";
        double temperature = 0.1;
        int max_tokens = 4096;
        double timeout = 60.0;  // LLM request timeout (seconds)
        int max_retries = 3;
        int intermediate_timeout = 5;  // Solver timeout for LLM intermediate verification (seconds)
        
        // LLM prompt configuration
        std::string system_prompt;
        std::string function_prompt;
        std::string optimization_prompt;
        
        // Advanced configuration
        bool enable_streaming = false;
        std::string organization_id;
        std::map<std::string, std::string> custom_headers;
        
        // vLLM local deployment configuration
        bool is_local_vllm = false;  // Whether using locally deployed vLLM model
        bool force_json_mode = true; // Whether to force JSON mode (vLLM may not support)
        
        // Multi-round iteration hints file
        std::string hints_file;  // JSON file path for previous failed constraints
    } llm;
    
    // Solving control
    bool no_fallback = false;  // Disable fallback to original solver
    
    // === Solver Configuration ===
    struct {
        std::string binary;  // Solver binary path
        int timeout = 30;    // Solver timeout (seconds)
    } solver;
    
    // === Configuration Management Methods ===
    
    // Load configuration from multiple sources (priority: command line > env vars > config file > defaults)
    static std::unique_ptr<Config> load(
        const std::string& config_file = "",
        const std::vector<std::string>& cmd_args = {},
        bool load_env = true
    );
    
    // Load from config file
    bool load_from_file(const std::string& file_path);
    
    // Load from command line arguments
    bool load_from_args(const std::vector<std::string>& args);
    

    
    // Validate configuration
    bool validate() const;
    
    // Print configuration info
    void print_config() const;
    
    // Get configuration summary
    std::string get_summary() const;
    
    // Set defaults
    void set_defaults();
    
    // Merge configuration
    void merge(const Config& other);
    
    // Get run mode string
    std::string get_run_mode_string() const;
    
    // Get log level string
    std::string get_log_level_string() const;
    
    // Create separate output folder for each case
    std::string create_case_output_dir(const std::string& input_file) const;

    // Command line argument parsing
    static std::unique_ptr<Config> parse_command_line(int argc, char* argv[]);

private:
    // Helper methods for parsing config file
    bool parse_config_line(const std::string& line);
    std::string trim(const std::string& str) const;
    std::vector<std::string> split(const std::string& str, char delimiter) const;
    bool parse_bool(const std::string& value) const;
    LogLevel parse_log_level(const std::string& value) const;
    RunMode parse_run_mode(const std::string& value) const;
};

// Global configuration instance
extern std::unique_ptr<Config> g_config;

// Helper functions
std::string run_mode_to_string(RunMode mode);
std::string log_level_to_string(LogLevel level);
RunMode string_to_run_mode(const std::string& str);
LogLevel string_to_log_level(const std::string& str);

// Configuration validation error
class ConfigError : public std::exception {
public:
    explicit ConfigError(const std::string& message) : message_(message) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

// Command line parsing result
struct CommandLineResult {
    bool show_help = false;
    bool print_config = false;
    std::string config_file;
    std::string input_file;
    std::string output_file;
    std::unique_ptr<Config> config;
    std::string error_message;
};

// Command line parser
class CommandLineParser {
public:
    static CommandLineResult parse(int argc, char* argv[]);
    static void print_usage(const char* program_name);

private:
    static std::vector<std::string> extract_non_option_args(const std::vector<std::string>& args);
    static bool is_option_with_value(const std::string& arg);
};

} // namespace linguaarray 
