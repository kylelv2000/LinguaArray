#pragma once

#include <string>
#include <vector>
#include <optional>
#include <set>
#include <map>

namespace linguaarray {

/**
 * Uninterpreted function information
 */
struct UninterpretedFunction {
    std::string name;
    std::vector<std::string> parameters;
    std::string return_type;
    std::vector<std::string> usage_contexts;
};

/**
 * Array variable information
 */
struct ArrayVariable {
    std::string name;
    std::vector<std::string> index_types;
    std::string element_type;
    std::vector<std::string> usage_contexts;
};

/**
 * SMT parsing result
 */
struct SMTParseResult {
    bool success = false;
    std::optional<std::string> logic;
    std::vector<UninterpretedFunction> uninterpreted_functions;
    std::vector<ArrayVariable> array_variables;
    std::set<std::string> custom_types;  // Custom types declared by declare-sort
    std::string content;
    std::string error_message;
};

/**
 * Instantiated function information
 */
struct InstantiatedFunction {
    std::string name;
    std::vector<std::string> param_names;
    std::vector<std::string> param_types;
    std::string return_type;
    std::string body;
    std::string full_definition;  // Complete define-fun definition
};

/**
 * Function replacement result
 */
struct ReplaceResult {
    bool success;
    std::string new_content;
    std::string error_message;
    std::vector<InstantiatedFunction> instantiated_functions;
};

/**
 * SMT simplification result
 */
struct SimplifyResult {
    bool success = false;
    std::string simplified_content;
    std::string error_message;
    std::map<std::string, std::string> variable_mapping;  // Original var name -> new var name
    std::map<std::string, std::string> constant_substitutions;  // Var name -> constant value
};

} // namespace linguaarray

