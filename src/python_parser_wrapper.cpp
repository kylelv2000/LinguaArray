#include "python_parser_wrapper.h"
#include <cstdlib>
#include <sstream>
#include <array>
#include <memory>
#include "nlohmann/json.hpp"

namespace linguaarray {

using json = nlohmann::json;

std::string exec_command(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    
    return result;
}

PythonParserWrapper::PythonParserWrapper(const std::string& python_script_path) 
    : python_script_path_(python_script_path) {
}

ParseResult PythonParserWrapper::parse_file(const std::string& filepath) {
    ParseResult result;
    
    try {
        // Call Python script
        std::string cmd = "python3 " + python_script_path_ + " \"" + filepath + "\"";
        std::string output = exec_command(cmd);
        
        // Parse JSON output
        auto j = json::parse(output);
        
        result.success = j.value("success", false);
        
        if (!result.success) {
            result.error_message = j.value("error", "Unknown error");
            return result;
        }
        
        // Parse UF
        if (j.contains("uninterpreted_functions")) {
            for (const auto& uf : j["uninterpreted_functions"]) {
                UninterpretedFunction func;
                func.name = uf.value("name", "");
                
                if (uf.contains("parameters")) {
                    for (const auto& param : uf["parameters"]) {
                        func.parameters.push_back(param.get<std::string>());
                    }
                }
                
                func.return_type = uf.value("return_type", "");
                result.uninterpreted_functions.push_back(func);
            }
        }
        
        // Parse Array
        if (j.contains("array_variables")) {
            for (const auto& arr : j["array_variables"]) {
                ArrayVariable array_var;
                array_var.name = arr.value("name", "");
                
                if (arr.contains("index_types")) {
                    for (const auto& idx : arr["index_types"]) {
                        array_var.index_types.push_back(idx.get<std::string>());
                    }
                }
                
                array_var.element_type = arr.value("element_type", "");
                result.array_variables.push_back(array_var);
            }
        }
        
        // Parse regular variables
        if (j.contains("regular_variables")) {
            for (const auto& var : j["regular_variables"]) {
                Variable regular_var;
                regular_var.name = var.value("name", "");
                regular_var.type = var.value("type", "");
                result.regular_variables.push_back(regular_var);
            }
        }
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Failed to parse: ") + e.what();
    }
    
    return result;
}

ParseResult PythonParserWrapper::parse_content(const std::string& content) {
    // Create temp file
    std::string temp_file = "/tmp/smt_temp_" + std::to_string(std::rand()) + ".smt2";
    
    std::ofstream out(temp_file);
    if (!out) {
        ParseResult result;
        result.success = false;
        result.error_message = "Failed to create temp file";
        return result;
    }
    
    out << content;
    out.close();
    
    // Parse temp file
    auto result = parse_file(temp_file);
    
    // Delete temp file
    std::remove(temp_file.c_str());
    
    return result;
}

} // namespace linguaarray

