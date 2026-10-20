#include "quantifier_instantiation_adapter.h"
#include "config.h"
#include <cstdlib>
#include <array>
#include <memory>
#include <filesystem>
#include <iostream>
#include <fstream>

namespace linguaarray {

QuantifierInstantiationAdapter::QuantifierInstantiationAdapter(
    const std::string& python_script_path) 
    : python_script_path_(python_script_path) {
}

std::string QuantifierInstantiationAdapter::exec_python_command(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    
    if (!pipe) {
        std::cerr << "Error: Unable to execute Python command" << std::endl;
        return "";
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    
    return result;
}

std::string QuantifierInstantiationAdapter::parse_quantifiers(const std::string& smt_file) {
    if (!check_python_available()) {
        std::cerr << "Error: Python3 not available" << std::endl;
        return "";
    }
    
    // Call Python script: quantifier_instantiation.py <smt_file>
    // Note: Don't use 2>&1 because z3 may output unsupported logic warnings (like ABVFP) which would break JSON output
    std::string cmd = "python3 " + python_script_path_ + " \"" + smt_file + "\" 2>/dev/null";
    
    if (g_config && g_config->log_level >= LogLevel::DEBUG) {
        std::cout << "Executing command: " << cmd << std::endl;
    }
    
    return exec_python_command(cmd);
}

std::string QuantifierInstantiationAdapter::instantiate_quantifiers(
    const std::string& smt_file,
    const std::string& llm_response,
    const std::string& output_file) {
    
    if (!check_python_available()) {
        std::cerr << "Error: Python3 not available" << std::endl;
        return "";
    }
    
    // Save LLM response to temp file
    std::string temp_response = output_file + ".llm_response.json";
    std::ofstream temp_file(temp_response);
    if (!temp_file.is_open()) {
        std::cerr << "Error: Unable to create temp file" << std::endl;
        return "";
    }
    temp_file << llm_response;
    temp_file.close();
    
    // Call Python script: quantifier_instantiation.py <smt_file> <llm_response_json> <output_smt2>
    // Note: Don't use 2>&1 because z3 may output unsupported logic warnings (like ABVFP) which would break JSON output
    std::string cmd = "python3 " + python_script_path_ + 
                      " \"" + smt_file + "\"" +
                      " \"" + temp_response + "\"" +
                      " \"" + output_file + "\" 2>/dev/null";
    
    if (g_config && g_config->log_level >= LogLevel::DEBUG) {
        std::cout << "Executing command: " << cmd << std::endl;
    }
    
    std::string result = exec_python_command(cmd);
    
    // Clean up temp file
    std::filesystem::remove(temp_response);
    
    if (result.empty()) {
        return "";
    }
    
    // Check if output file was generated
    if (std::filesystem::exists(output_file)) {
        return output_file;
    }
    
    return "";
}

bool QuantifierInstantiationAdapter::check_python_available() {
    int ret = system("python3 --version > /dev/null 2>&1");
    if (ret != 0) {
        ret = system("python --version > /dev/null 2>&1");
    }
    return ret == 0;
}

} // namespace linguaarray

