#include "python_parser_adapter.h"
#include <cstdlib>
#include <sstream>
#include <array>
#include <memory>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <random>
#include <chrono>
#include <unistd.h>
#include "nlohmann/json.hpp"

namespace {
// Generate unique temp filename (multi-process safe)
std::string generate_unique_temp_name(const std::string& prefix) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd() ^ std::chrono::high_resolution_clock::now().time_since_epoch().count() ^ getpid());
    std::uniform_int_distribution<uint64_t> dis;
    return "/tmp/" + prefix + "_" + std::to_string(dis(gen)) + "_" + std::to_string(getpid()) + ".smt2";
}
}

namespace linguaarray {

using json = nlohmann::json;

PythonParserAdapter::PythonParserAdapter(const std::string& python_script_path) 
    : python_script_path_(python_script_path) {
}

std::string PythonParserAdapter::exec_python_command(const std::string& cmd) {
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

SMTParseResult PythonParserAdapter::parse_file(const std::string& filepath) {
    SMTParseResult result;
    
    try {
        // Call Python parser
        std::string cmd = "python3 " + python_script_path_ + " parse \"" + filepath + "\"";
        std::string output = exec_python_command(cmd);
        
        // Parse JSON
        auto j = json::parse(output);
        
        result.success = j.value("success", false);
        
        if (!result.success) {
            result.error_message = j.value("error_message", "Unknown error");
            return result;
        }
        
        // Extract logic
        if (j.contains("logic") && !j["logic"].is_null()) {
            result.logic = j["logic"].get<std::string>();
        }
        
        // Extract UF
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
                
                if (uf.contains("usage_contexts")) {
                    for (const auto& ctx : uf["usage_contexts"]) {
                        func.usage_contexts.push_back(ctx.get<std::string>());
                    }
                }
                
                result.uninterpreted_functions.push_back(func);
            }
        }
        
        // Extract Array
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
                
                if (arr.contains("usage_contexts")) {
                    for (const auto& ctx : arr["usage_contexts"]) {
                        array_var.usage_contexts.push_back(ctx.get<std::string>());
                    }
                }
                
                result.array_variables.push_back(array_var);
            }
        }
        
        // Extract custom_types
        if (j.contains("custom_types")) {
            for (const auto& t : j["custom_types"]) {
                result.custom_types.insert(t.get<std::string>());
            }
        }
        
        // Extract content
        result.content = j.value("content", "");
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Python parsing failed: ") + e.what();
    }
    
    return result;
}

SMTParseResult PythonParserAdapter::parse_content(const std::string& content) {
    // Create temp file (use unique filename to avoid multi-process conflicts)
    std::string temp_file = generate_unique_temp_name("smt_parse");
    
    std::ofstream out(temp_file);
    if (!out) {
        SMTParseResult result;
        result.success = false;
        result.error_message = "Unable to create temp file";
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

ReplaceResult PythonParserAdapter::replace_function_definitions(
    const std::string& original_content,
    const std::vector<std::pair<std::string, std::string>>& function_definitions) {
    
    ReplaceResult result;
    
    try {
        // Create temp file to store original content
        std::string temp_content_file = generate_unique_temp_name("smt_replace_content");
        std::ofstream out(temp_content_file);
        if (!out) {
            result.success = false;
            result.error_message = "Unable to create temp file";
            return result;
        }
        out << original_content;
        out.close();
        
        // Create temp file to store function definitions (JSON format)
        std::string temp_defs_file = generate_unique_temp_name("smt_replace_defs");
        temp_defs_file = temp_defs_file.substr(0, temp_defs_file.size()-5) + ".json";
        json defs_json = json::array();
        for (const auto& [name, def] : function_definitions) {
            defs_json.push_back({{"name", name}, {"definition", def}});
        }
        
        std::ofstream defs_out(temp_defs_file);
        if (!defs_out) {
            std::remove(temp_content_file.c_str());
            result.success = false;
            result.error_message = "Unable to create definition file";
            return result;
        }
        defs_out << defs_json.dump();
        defs_out.close();
        
        // Call Python script
        std::string cmd = "python3 -c \"import sys; sys.path.insert(0, '" + 
                          std::filesystem::path(python_script_path_).parent_path().string() + 
                          "'); from smt_parser import SMTParser; import json; " +
                          "parser = SMTParser(); " +
                          "with open('" + temp_content_file + "') as f: content = f.read(); " +
                          "with open('" + temp_defs_file + "') as f: defs = json.load(f); " +
                          "defs_list = [(d['name'], d['definition']) for d in defs]; " +
                          "result = parser.replace_function_definitions(content, defs_list); " +
                          "print(json.dumps(result, ensure_ascii=False))\"";
        
        std::string output = exec_python_command(cmd);
        auto j = json::parse(output);
        
        result.success = j.value("success", false);
        result.new_content = j.value("new_content", original_content);
        result.error_message = j.value("error_message", "");
        
        // Parse instantiated function info
        if (j.contains("instantiated_functions")) {
            for (const auto& func : j["instantiated_functions"]) {
                InstantiatedFunction inst_func;
                inst_func.name = func.value("name", "");
                
                if (func.contains("param_names")) {
                    for (const auto& p : func["param_names"]) {
                        inst_func.param_names.push_back(p.get<std::string>());
                    }
                }
                
                if (func.contains("param_types")) {
                    for (const auto& t : func["param_types"]) {
                        inst_func.param_types.push_back(t.get<std::string>());
                    }
                }
                
                inst_func.return_type = func.value("return_type", "");
                inst_func.body = func.value("body", "");
                inst_func.full_definition = func.value("full_definition", "");
                
                result.instantiated_functions.push_back(inst_func);
            }
        }
        
        // Clean up temp files
        std::remove(temp_content_file.c_str());
        std::remove(temp_defs_file.c_str());
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Python replacement failed: ") + e.what();
        result.new_content = original_content;
    }
    
    return result;
}

std::string PythonParserAdapter::generate_negation_constraint(
    const std::vector<InstantiatedFunction>& instantiated_functions) {
    
    try {
        // Convert instantiated function info to JSON
        json funcs_json = json::array();
        for (const auto& func : instantiated_functions) {
            funcs_json.push_back({
                {"name", func.name},
                {"param_names", func.param_names},
                {"param_types", func.param_types},
                {"return_type", func.return_type},
                {"body", func.body},
                {"full_definition", func.full_definition}
            });
        }
        
        // Create temp file
        std::string temp_file = generate_unique_temp_name("smt_negation");
        temp_file = temp_file.substr(0, temp_file.size()-5) + ".json";
        std::ofstream out(temp_file);
        if (!out) {
            return "";
        }
        out << funcs_json.dump();
        out.close();
        
        // Call Python script
        std::string cmd = "python3 -c \"import sys; sys.path.insert(0, '" + 
                          std::filesystem::path(python_script_path_).parent_path().string() + 
                          "'); from smt_parser import SMTParser; import json; " +
                          "parser = SMTParser(); " +
                          "with open('" + temp_file + "') as f: funcs = json.load(f); " +
                          "result = parser.generate_negation_constraint(funcs); " +
                          "print(result)\"";
        
        std::string output = exec_python_command(cmd);
        
        // Clean up temp file
        std::remove(temp_file.c_str());
        
        // Remove newline from output
        output.erase(std::remove(output.begin(), output.end(), '\n'), output.end());
        
        return output;
    } catch (const std::exception& e) {
        return "";
    }
}

std::string PythonParserAdapter::add_negation_constraint_to_content(
    const std::string& original_content,
    const std::string& negation_constraint) {
    
    try {
        // Create temp file
        std::string temp_file = generate_unique_temp_name("smt_add_constraint");
        std::ofstream out(temp_file);
        if (!out) {
            return original_content;
        }
        out << original_content;
        out.close();
        
        // Call Python script
        std::string cmd = "python3 -c \"import sys; sys.path.insert(0, '" + 
                          std::filesystem::path(python_script_path_).parent_path().string() + 
                          "'); from smt_parser import SMTParser; " +
                          "parser = SMTParser(); " +
                          "with open('" + temp_file + "') as f: content = f.read(); " +
                          "result = parser.add_negation_constraint_to_content(content, '" + 
                          negation_constraint + "'); " +
                          "print(result)\"";
        
        std::string output = exec_python_command(cmd);
        
        // Clean up temp file
        std::remove(temp_file.c_str());
        
        return output;
    } catch (const std::exception& e) {
        return original_content;
    }
}

SimplifyResult PythonParserAdapter::simplify_smt(const std::string& content) {
    SimplifyResult result;
    
    try {
        // Create temp file
        std::string temp_file = generate_unique_temp_name("smt_simplify");
        std::ofstream out(temp_file);
        if (!out) {
            result.success = false;
            result.error_message = "Unable to create temp file";
            result.simplified_content = content;
            return result;
        }
        out << content;
        out.close();
        
        // Call Python script
        std::string cmd = "python3 " + python_script_path_ + " simplify \"" + temp_file + "\"";
        std::string output = exec_python_command(cmd);
        
        // Parse JSON
        auto j = json::parse(output);
        
        result.success = j.value("success", false);
        result.simplified_content = j.value("simplified_content", content);
        result.error_message = j.value("error_message", "");
        
        // Parse variable mapping
        if (j.contains("variable_mapping") && j["variable_mapping"].is_object()) {
            for (auto& [key, val] : j["variable_mapping"].items()) {
                result.variable_mapping[key] = val.get<std::string>();
            }
        }
        
        // Parse constant substitutions
        if (j.contains("constant_substitutions") && j["constant_substitutions"].is_object()) {
            for (auto& [key, val] : j["constant_substitutions"].items()) {
                result.constant_substitutions[key] = val.get<std::string>();
            }
        }
        
        // Clean up temp file
        std::remove(temp_file.c_str());
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Python simplification failed: ") + e.what();
        result.simplified_content = content;
    }
    
    return result;
}

} // namespace linguaarray

