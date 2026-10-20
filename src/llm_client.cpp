#include "llm_client.h"
#include "smt_types.h"
#include "config.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <regex>

using json = nlohmann::json;

namespace linguaarray {

// curl global initialization (executed only once)
static std::once_flag curl_init_flag;
static void init_curl_global() {
    curl_global_init(CURL_GLOBAL_ALL);
}

// curl callback function
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

LLMClient::LLMClient(const Config& config) 
    : api_key_(config.llm.api_key),
      api_base_(config.llm.api_base),
      model_(config.llm.model),
      temperature_(config.llm.temperature),
      max_tokens_(config.llm.max_tokens),
      timeout_(config.llm.timeout),
      max_retries_(config.llm.max_retries),
      enable_streaming_(config.llm.enable_streaming),
      organization_id_(config.llm.organization_id),
      system_prompt_(config.llm.system_prompt),
      function_prompt_(config.llm.function_prompt),
      optimization_prompt_(config.llm.optimization_prompt),
      is_local_vllm_(config.llm.is_local_vllm),
      force_json_mode_(config.llm.force_json_mode),
      hints_file_(config.llm.hints_file) {
    // Ensure curl global initialization only executed once
    std::call_once(curl_init_flag, init_curl_global);
}

bool LLMClient::generate_constraints_json(
    const std::string& smt_content,
    const std::vector<UninterpretedFunction>& functions,
    const std::vector<ArrayVariable>& arrays,
    const std::string& output_file,
    const std::string& original_smt_file) {
    
    if (api_key_.empty()) {
        std::cerr << "Error: OpenAI API key not set" << std::endl;
        return false;
    }
    
    std::string prompt = build_prompt(smt_content, functions, arrays, original_smt_file);
    std::string response;
    
    // Implement retry mechanism
    for (int attempt = 0; attempt < max_retries_; ++attempt) {
        response = call_openai_api(prompt);
        if (!response.empty()) {
            break;
        }
        
        if (attempt < max_retries_ - 1) {
            std::cerr << "LLM API call failed, retrying (" << (attempt + 1) << "/" << max_retries_ << ")..." << std::endl;
        }
    }
    
    if (response.empty()) {
        std::cerr << "LLM API call failed: max retries reached " << max_retries_ << std::endl;
        return false;
    }
    
    // Save response to file
    std::ofstream file(output_file);
    if (!file.is_open()) {
        std::cerr << "Unable to create output file: " << output_file << std::endl;
        return false;
    }
    
    file << response;
    file.close();
    
    if (g_config && g_config->log_level >= LogLevel::INFO) {
        std::cout << "LLM response saved to: " << output_file << std::endl;
    }
    return true;
}

std::string LLMClient::build_hints_prompt(const std::string& smt_file, bool is_sat_path) const {
    if (hints_file_.empty() || smt_file.empty()) {
        return "";
    }
    
    std::ifstream hints_stream(hints_file_);
    if (!hints_stream.is_open()) {
        return "";
    }
    
    try {
        json hints_data = json::parse(hints_stream);

        // Try multiple possible keys: relative path, absolute path
        std::vector<std::string> possible_keys = {smt_file};

        // If relative path, try converting to absolute path
        if (smt_file.find('/') != 0) {  // Not absolute path
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd)) != nullptr) {
                std::string abs_path = std::string(cwd) + "/" + smt_file;
                possible_keys.push_back(abs_path);
            }
        }

        std::string found_key = "";
        for (const auto& key : possible_keys) {
            if (hints_data.contains(key)) {
                found_key = key;
                break;
            }
        }

        if (found_key.empty()) {
            return "";
        }

        auto& h = hints_data[found_key];
        std::stringstream ss;
        bool has_content = false;
        
        ss << "\n## Previous Attempts\n";
        ss << "The following were tried before but did not lead to a solution. Please think again or further about whether there is a problem or whether you should use others.\n";
        ss << "They may not be wrong - perhaps they need simplification, more precision, or additional constraints.\n\n";

        if (is_sat_path) {
            // SAT branch: only show Previous constraints
            if (h.contains("constraints") && h["constraints"].is_array() && !h["constraints"].empty()) {
                ss << "Previous constraints:\n";
                for (const auto& c : h["constraints"]) {
                    if (c.is_string()) {
                        ss << "  - `" << c.get<std::string>() << "`\n";
                    }
                }
                has_content = true;
            }
        } else {
            // UNSAT branch: only show Previous instantiations
            if (h.contains("instantiations") && h["instantiations"].is_array() && !h["instantiations"].empty()) {
                ss << "Previous instantiations:\n";
                for (const auto& inst : h["instantiations"]) {
                    if (inst.is_string()) {
                        ss << "  - `" << inst.get<std::string>() << "`\n";
                    }
                }
                has_content = true;
            }
        }
        
        if (has_content) {
            ss << "\nConsider: refining these, adding more constraints, or trying different approaches.\n";
        }
        return has_content ? ss.str() : "";
    } catch (...) {
        return "";
    }
}

std::string LLMClient::build_prompt(
    const std::string& smt_content,
    const std::vector<UninterpretedFunction>& functions,
    const std::vector<ArrayVariable>& arrays,
    const std::string& original_smt_file) {
    
    std::stringstream prompt;
    
    if (!function_prompt_.empty()) {
        prompt << function_prompt_ << "\n\n";
    } else {
        prompt << "# SMT Satisfiability Analysis Task\n\n";
        prompt << "## Objective\n";
        prompt << "Analyze the SMT problem below and provide 3-5 key constraints to accelerate solving.\n\n";
        
        prompt << "## Your Task\n";
        prompt << "1. **Deeply analyze and reason about the problem**:\n";
        prompt << "   - Carefully examine all assertions and their logical relationships\n";
        prompt << "   - Identify which constraints are most restrictive\n";
        prompt << "   - Reason about what values or relationships MUST hold for satisfiability\n";
        prompt << "   - Try to derive a concrete satisfying assignment if possible\n\n";
        prompt << "2. **Determine satisfiability**: Based on your reasoning, predict sat/unsat/unknown\n\n";
        prompt << "3. **Provide 3-5 CONCRETE constraints** that directly help solve the problem:\n";
        prompt << "   - **If SAT**: Provide SPECIFIC variable/array assignments that satisfy the formula\n";
        prompt << "     * Instantiate variables with concrete values: `(= x 10)`, `(= (f 3) 7)`\n";
        prompt << "     * Instantiate array elements at specific indices: `(= (select arr 0) 5)`, `(= (select (select h 2) 4) 10)`\n";
        prompt << "     * State necessary disequalities: `(not (= E F))`, `(distinct x y z)`\n";
        prompt << "     * Derive implied equalities/inequalities that MUST hold\n";
        prompt << "     * DO NOT provide abstract lemmas like `(< a b)` - give concrete values!\n";
        prompt << "   - **If UNSAT**: Identify specific contradictory facts\n\n";
        prompt << "4. **Be CONCRETE and MINIMAL** - instantiate actual values to simplify the problem\n\n";
    }
    
    prompt << "## SMT Problem\n```smt2\n" << smt_content << "\n```\n\n";
    
    prompt << "## Required Output Format\n";
    prompt << "Return a JSON object with the following structure:\n\n";
    prompt << "```json\n";
    prompt << "{\n";
    prompt << "  \"success\": true,\n";
    prompt << "  \"satisfiability\": \"sat\" | \"unsat\" | \"unknown\",\n";
    prompt << "  \"chain_of_thought\": \"(200-400 tokens max, do NOT overthink). Detailed reasoning: analyze key constraints, identify bottlenecks, explain why sat/unsat, describe the model or contradiction...\",\n";
    
    // If there are UFs, add functions field description
    if (!functions.empty()) {
        prompt << "  \"functions\": [\n";
        prompt << "    {\n";
        prompt << "      \"function_name\": \"f\",\n";
        prompt << "      \"parameters\": [\"Int\"],\n";
        prompt << "      \"return_type\": \"Int\",\n";
        prompt << "      \"definition\": \"(define-fun f ((x Int)) Int (+ x 1))\",\n";
        prompt << "      \"confidence\": 0.85\n";
        prompt << "    }\n";
        prompt << "  ],\n";
    }
    
    prompt << "  \"constraints\": [\n";
    prompt << "    {\n";
    prompt << "      \"name\": \"Variable x concrete value\",\n";
    prompt << "      \"constraint\": \"(= x 10)\",\n";
    prompt << "      \"type\": \"assignment\",\n";
    prompt << "      \"confidence\": 0.9\n";
    prompt << "    },\n";
    prompt << "    {\n";
    prompt << "      \"name\": \"Array element at index 0\",\n";
    prompt << "      \"constraint\": \"(= (select arr 0) 5)\",\n";
    prompt << "      \"type\": \"assignment\",\n";
    prompt << "      \"confidence\": 0.8\n";
    prompt << "    },\n";
    prompt << "    {\n";
    prompt << "      \"name\": \"Variables must differ\",\n";
    prompt << "      \"constraint\": \"(not (= x y))\",\n";
    prompt << "      \"type\": \"disequality\",\n";
    prompt << "      \"confidence\": 0.75\n";
    prompt << "    }\n";
    prompt << "  ]\n";
    prompt << "}\n";
    prompt << "```\n\n";
    
    prompt << "## Reasoning Strategy\n";
    prompt << "Before providing constraints, perform deep analysis:\n";
    prompt << "1. **Understand the structure**: What are the key variables, functions, and arrays?\n";
    prompt << "2. **Identify bottlenecks**: Which assertions are most restrictive?\n";
    prompt << "3. **Trace dependencies**: How do different constraints interact?\n";
    prompt << "4. **Derive implications**: What relationships MUST be true for satisfiability?\n";
    prompt << "5. **Construct a model**: Try to find concrete values that satisfy all constraints\n";
    prompt << "6. **Instantiate strategically**: Pick specific values/indices that simplify the problem\n\n";
    prompt << "Your goal: Provide constraints that directly lead to a satisfying assignment or prove unsatisfiability.\n\n";
    
    prompt << "## Constraint Format Requirements\n";
    prompt << "The `constraint` field must contain a **pure SMT-LIB boolean expression** that can be directly used in `(assert ...)`. \n\n";
    prompt << "**Correct examples (CONCRETE assignments preferred for SAT):**\n";
    prompt << "- `\"(= x 10)\"` - ✓ concrete variable value\n";
    prompt << "- `\"(= (f 5) 42)\"` - ✓ concrete function value at specific index\n";
    prompt << "- `\"(= (select arr 0) 10)\"` - ✓ concrete array element\n";
    prompt << "- `\"(= (select (select h 2) 4) 7)\"` - ✓ concrete nested array element\n";
    prompt << "- `\"(not (= E F))\"` - ✓ concrete disequality between terms\n";
    prompt << "- `\"(and (= x 5) (= y 3))\"` - ✓ multiple concrete assignments\n\n";
    prompt << "**AVOID these (too abstract for SAT cases):**\n";
    prompt << "- `\"(< a b)\"` - ⚠️ Abstract, prefer concrete values: `(= a 3)` and `(= b 10)`\n";
    prompt << "- `\"(> (f x) 0)\"` - ⚠️ Abstract property, prefer: `(= (f 3) 5)`\n\n";
    prompt << "**Incorrect syntax (NEVER use):**\n";
    prompt << "- `\"(assert (= x 10))\"` - ❌ Do not include `assert`\n";
    prompt << "- `\"(define-fun ...)\"` - ❌ Do not use `define-fun`\n";
    prompt << "- `\"x = 10\"` - ❌ Not SMT-LIB syntax\n\n";
    
    prompt << "## Important Guidelines\n";
    
    if (!functions.empty()) {
        prompt << "- **For Uninterpreted Functions (UF)**: Provide concrete `define-fun` definitions in the `functions` array\n";
        prompt << "  * Each function definition must be complete and valid SMT-LIB syntax\n";
        prompt << "  * Avoid `define-fun-rec` unless necessary (prefer simple non-recursive definitions)\n";
        prompt << "  * Function name and types must match the declared function\n";
        prompt << "  * For Real type, use `.0` suffix (e.g., `5.0`); for Int type, use plain integers\n";
        prompt << "  * **IMPORTANT**: Only provide definitions for simple UFs you can fully understand\n";
        prompt << "    - ✓ Simple types: `(declare-fun f (Int) Int)`, `(declare-fun g (Int Int) Bool)`\n";
        prompt << "    - ⚠️ Complex theories: `(declare-fun h ((Array Int Int)) Int)` - Skip if uncertain\n";
        prompt << "    - ⚠️ BitVectors/FP: `(declare-fun bv ((_ BitVec 32)) Bool)` - Skip if uncertain\n";
        prompt << "  * If you cannot determine a valid definition, OMIT it from the `functions` array (leave it to the solver)\n";
        prompt << "  * Set lower confidence (< 0.6) for definitions involving complex theories\n\n";
    }
    
    prompt << "- Return **exactly 3-5 constraints** (quality over quantity)\n";
    prompt << "- **For SAT predictions**: Provide CONCRETE model guesses (specific values)\n";
    prompt << "  * Directly assign values to variables: `(= x 10)`\n";
    prompt << "  * Specify array elements at concrete indices: `(= (select arr 0) 5)`\n";
    prompt << "  * State specific disequalities: `(not (= E F))`\n";
    prompt << "  * Derive necessary relationships from reasoning about the constraints\n";
    prompt << "  * DO NOT provide abstract lemmas like `(< a b)` - guess actual values!\n";
    prompt << "- **For UNSAT predictions**: Identify contradictory facts\n";
    prompt << "- Each constraint must be **syntactically valid** SMT-LIB syntax: `(operator arg1 arg2 ...)`\n";
    prompt << "- For Real type: use `.0` suffix (e.g., `5.0`); for Int type: plain integers (e.g., `5`)\n";
    prompt << "- **CRITICAL**: Only reference top-level declared variables (from `declare-const`, `declare-fun`)\n";
    prompt << "  * ❌ NEVER use bound variables from quantifiers (`forall`, `exists`)\n";
    prompt << "  * ❌ Example: If the problem has `(forall ((x Int)) ...)`, do NOT use `x` in your constraints\n";
    prompt << "  * ✓ Only use variables that are globally declared at the top level\n";
    prompt << "- Be MINIMAL and CONCRETE - the solver needs specific guidance, not general theories\n";
    
    // Add previous attempts at the end (if any) - SAT path only shows constraints
    std::string hints = build_hints_prompt(original_smt_file, true);
    if (!hints.empty()) {
        prompt << hints;
    }
    
    if(g_config && g_config->log_level >= LogLevel::DEBUG) {
        std::cout << "prompt: " << prompt.str() << std::endl;
    }
    return prompt.str();
}

std::string LLMClient::call_openai_api(const std::string& prompt) {
    CURL* curl;
    CURLcode res;
    std::string response;
    
    curl = curl_easy_init();
    if (!curl) {
        std::cerr << "curl initialization failed" << std::endl;
        return "";
    }
    
    // Build request JSON with config parameters
    std::string effective_system_prompt;
    if (!system_prompt_.empty()) {
        effective_system_prompt = system_prompt_;
    } else if (is_local_vllm_) {
        // Local deployed model: brief thinking, then output JSON
        effective_system_prompt = "You are an SMT expert. Brief thinking (200-400 tokens max, do NOT overthink), then output JSON. CRITICAL: All constraints must be valid SMT-LIB expressions (e.g., \"(= x 10)\").";
    } else {
        effective_system_prompt = "You are an expert in SMT (Satisfiability Modulo Theories) and formal verification. Your task is to deeply analyze SMT problems, reason about their structure and semantics, and provide concrete constraints that directly guide solvers to solutions. Think carefully about what values or relationships MUST hold for satisfiability.  Brief thinking (200-400 tokens max, do NOT overthink), then return ONLY the JSON object.";
    }
    
    json request_json = {
        {"model", model_},
        {"messages", {
            {{"role", "system"}, {"content", effective_system_prompt}},
            {{"role", "user"}, {"content", prompt}}
        }},
        {"temperature", temperature_},
        {"max_tokens", max_tokens_}
    };
    
    // Add optional parameters
    if (enable_streaming_) {
        request_json["stream"] = true;
    }
    
    // Force JSON format output (vLLM local deployment may not support)
    if (force_json_mode_ && !is_local_vllm_) {
        request_json["response_format"] = {{"type", "json_object"}};
    }
    
    std::string request_body = request_json.dump();
    
    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header.c_str());
    
    // Add organization ID header (if configured)
    std::string org_header;
    if (!organization_id_.empty()) {
        org_header = "OpenAI-Organization: " + organization_id_;
        headers = curl_slist_append(headers, org_header.c_str());
    }
    
    // Build complete API URL
    std::string api_url = api_base_;
    if (api_url.back() == '/') {
        api_url.pop_back();  // Remove trailing slash
    }
    api_url += "/chat/completions";
    
    curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_));
    
    // Multithread support: disable shared resources, ensure each request is independent
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // Required for multithread
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 0L);  // Disable DNS cache sharing
    
    // Enable verbose debug info (only in debug mode)
    if (g_config && g_config->log_level >= LogLevel::DEBUG) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }
    
    res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        std::cerr << "curl request failed: " << curl_easy_strerror(res) << std::endl;
        response.clear();
    } else {
        // Check HTTP response code
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        
        if (response_code != 200) {
            std::cerr << "HTTP error: " << response_code << std::endl;
            std::cerr << "Response content: " << response << std::endl;
            response.clear();
        } else {
            // Parse response, extract content
            try {
                json response_json = json::parse(response);
                if (response_json.contains("choices") && !response_json["choices"].empty()) {
                    std::string content = response_json["choices"][0]["message"]["content"];
                    
                    // For vLLM local deployment, need to extract JSON from non-standard format
                    if (is_local_vllm_) {
                        if (g_config && g_config->log_level >= LogLevel::DEBUG) {
                            std::cout << "vLLM raw response content: " << content << std::endl;
                        }
                        response = extract_json_from_response(content);
                        if (response.empty()) {
                            std::cerr << "Unable to extract valid JSON from vLLM response" << std::endl;
                            std::cerr << "Original content: " << content << std::endl;
                        }
                    } else {
                        response = content;
                    }
                } else {
                    std::cerr << "API response format error: missing choices field" << std::endl;
                    std::cerr << "Full response: " << response << std::endl;
                    response.clear();
                }
            } catch (const std::exception& e) {
                std::cerr << "JSON parsing failed: " << e.what() << std::endl;
                std::cerr << "Original response: " << response << std::endl;
                response.clear();
            }
        }
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return response;
}

LLMResponse LLMClient::read_llm_response_from_json(
    const std::string& json_file) {
    
    LLMResponse response;
    
    std::ifstream file(json_file);
    if (!file.is_open()) {
        std::cerr << "Unable to open JSON file: " << json_file << std::endl;
        return response;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    try {
        json data = json::parse(content);
        
        response.success = data.value("success", false);
        response.satisfiability = data.value("satisfiability", "unknown");
        response.chain_of_thought = data.value("chain_of_thought", "");
        
        // Read constraints
        if (data.contains("constraints")) {
            for (const auto& cons_data : data["constraints"]) {
                LLMConstraint cons;
                cons.name = cons_data.value("name", "");
                cons.constraint = cons_data.value("constraint", "");
                cons.type = cons_data.value("type", "");
                cons.confidence = cons_data.value("confidence", 0.5);
                
                response.constraints.push_back(cons);
            }
        }
        
        // Read UF definitions
        if (data.contains("functions")) {
            for (const auto& func_data : data["functions"]) {
                FunctionInstantiation func;
                func.function_name = func_data.value("function_name", "");
                func.parameters = func_data.value("parameters", std::vector<std::string>());
                func.return_type = func_data.value("return_type", "");
                func.definition = func_data.value("definition", "");
                func.confidence = func_data.value("confidence", 0.5);
                
                response.functions.push_back(func);
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "JSON parsing failed: " << e.what() << std::endl;
    }
    
    if(g_config && g_config->log_level >= LogLevel::INFO) {
        std::cout << "Read " << response.constraints.size() << " constraints from JSON";
        if (!response.functions.empty()) {
            std::cout << ", " << response.functions.size() << " UF definitions";
        }
        std::cout << ", satisfiability prediction: " << response.satisfiability << std::endl;
    }
    
    return response;
}

bool LLMClient::call_llm_with_prompt(
    const std::string& prompt,
    const std::string& output_file,
    const std::string& original_smt_file) {
    
    if (api_key_.empty()) {
        std::cerr << "Error: OpenAI API key not set" << std::endl;
        return false;
    }
    
    // If hints exist, add to prompt end - UNSAT path only shows instantiations
    std::string full_prompt = prompt;
    std::string hints = build_hints_prompt(original_smt_file, false);
    if (!hints.empty()) {
        full_prompt = prompt + hints;
    }
    
    std::string response;
    
    // Retry mechanism
    for (int attempt = 0; attempt < max_retries_; ++attempt) {
        response = call_openai_api(full_prompt);
        if (!response.empty()) {
            break;
        }
        if (attempt < max_retries_ - 1) {
            std::cerr << "API call failed, retrying (" << (attempt + 1) << "/" << max_retries_ << ")..." << std::endl;
        }
    }
    
    if (response.empty()) {
        std::cerr << "API call failed" << std::endl;
        return false;
    }
    
    // Save response
    std::ofstream file(output_file);
    if (!file.is_open()) {
        std::cerr << "Unable to create output file: " << output_file << std::endl;
        return false;
    }
    
    file << response;
    file.close();
    return true;
}

// ============ vLLM JSON extraction helper functions ============

std::string LLMClient::remove_think_tags(const std::string& content) {
    std::string result = content;
    
    // Remove <think>...</think> tags and their content
    std::regex think_regex("<think>[\\s\\S]*?</think>", std::regex::icase);
    result = std::regex_replace(result, think_regex, "");
    
    // Remove other possible thinking tag formats
    std::regex think_alt_regex("<thinking>[\\s\\S]*?</thinking>", std::regex::icase);
    result = std::regex_replace(result, think_alt_regex, "");
    
    // Remove leading/trailing whitespace
    size_t start = result.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\r\n");
    return result.substr(start, end - start + 1);
}

std::string LLMClient::extract_json_from_markdown(const std::string& content) {
    // Try matching ```json ... ``` format
    std::regex json_block_regex("```json\\s*\\n([\\s\\S]*?)\\n```");
    std::smatch match;
    if (std::regex_search(content, match, json_block_regex) && match.size() > 1) {
        return match[1].str();
    }
    
    // Try matching ``` ... ``` format (no language tag)
    std::regex code_block_regex("```\\s*\\n([\\s\\S]*?)\\n```");
    if (std::regex_search(content, match, code_block_regex) && match.size() > 1) {
        std::string code = match[1].str();
        // Check if it looks like JSON
        size_t brace_start = code.find('{');
        if (brace_start != std::string::npos) {
            return code;
        }
    }
    
    return "";
}

std::string LLMClient::try_fix_json(const std::string& content) {
    std::string result = content;
    
    // 1. Find first { and last }
    size_t start = result.find('{');
    size_t end = result.rfind('}');
    
    if (start == std::string::npos || end == std::string::npos || start > end) {
        return "";
    }
    
    result = result.substr(start, end - start + 1);
    
    // 2. Try parsing, if successful return
    if (json::accept(result)) {
        return result;
    }
    
    // 3. Try fixing common issues
    // 3.1 Remove trailing commas
    std::regex trailing_comma_regex(",\\s*([}\\]])");
    result = std::regex_replace(result, trailing_comma_regex, "$1");
    
    // 3.2 Fix single quotes to double quotes
    // This is more complex, for simplicity only handle key names
    std::regex single_quote_key_regex("'([^']+)'\\s*:");
    result = std::regex_replace(result, single_quote_key_regex, "\"$1\":");
    
    // 3.3 Try parsing again
    if (json::accept(result)) {
        return result;
    }
    
    // 4. Last attempt: progressively truncate end to find valid JSON
    for (size_t i = result.length(); i > start + 1; --i) {
        if (result[i-1] == '}') {
            std::string attempt = result.substr(0, i);
            if (json::accept(attempt)) {
                return attempt;
            }
        }
    }
    
    return "";
}

std::string LLMClient::extract_json_from_response(const std::string& response) {
    if (response.empty()) {
        return "";
    }
    
    // 1. First remove <think> tags
    std::string content = remove_think_tags(response);
    
    if (content.empty()) {
        // If empty after removal, maybe entire response is in think tags, try original content
        content = response;
    }
    
    if (g_config && g_config->log_level >= LogLevel::DEBUG) {
        std::cout << "After removing think tags: " << content << std::endl;
    }
    
    // 2. Try parsing directly as JSON
    if (json::accept(content)) {
        return content;
    }
    
    // 3. Try extracting from Markdown code block
    std::string markdown_json = extract_json_from_markdown(content);
    if (!markdown_json.empty()) {
        if (json::accept(markdown_json)) {
            return markdown_json;
        }
        
        std::string fixed = try_fix_json(markdown_json);
        if (!fixed.empty()) {
            return fixed;
        }
    }
    
    // 4. Try extracting JSON directly from content
    std::string fixed_json = try_fix_json(content);
    if (!fixed_json.empty()) {
        return fixed_json;
    }
    
    // 5. Last attempt: construct a basic JSON structure
    // Try to extract useful info from response
    if (g_config && g_config->log_level >= LogLevel::INFO) {
        std::cerr << "Warning: Unable to extract standard JSON, attempting to construct basic structure" << std::endl;
    }
    
    // Detect which type of response (SAT branch vs UNSAT branch)
    bool is_unsat_branch = (content.find("instantiation") != std::string::npos ||
                            content.find("witness_map") != std::string::npos ||
                            content.find("quantifier_id") != std::string::npos);
    
    if (is_unsat_branch) {
        // UNSAT branch: extract quantifier instantiation info
        return extract_unsat_json_fallback(content);
    } else {
        // SAT branch: extract constraint info
        return extract_sat_json_fallback(content);
    }
}

std::string LLMClient::extract_sat_json_fallback(const std::string& content) {
    // Try to extract satisfiability info
    std::string sat_result = "unknown";
    if (content.find("unsat") != std::string::npos || 
        content.find("UNSAT") != std::string::npos ||
        content.find("unsatisfiable") != std::string::npos) {
        sat_result = "unsat";
    } else if (content.find("\"sat\"") != std::string::npos || 
               content.find("'sat'") != std::string::npos ||
               content.find(": sat") != std::string::npos ||
               (content.find("satisfiable") != std::string::npos && 
                content.find("unsatisfiable") == std::string::npos)) {
        sat_result = "sat";
    }
    
    // Try to extract constraints
    std::vector<std::string> constraints;
    std::regex constraint_regex("\"constraint\"\\s*:\\s*\"([^\"]+)\"");
    std::sregex_iterator it(content.begin(), content.end(), constraint_regex);
    std::sregex_iterator end_it;
    while (it != end_it) {
        constraints.push_back((*it)[1].str());
        ++it;
    }
    
    // If not found, try other patterns: (= x 10) or (select arr 0) etc SMT expressions
    if (constraints.empty()) {
        std::regex smt_expr_regex("\\((?:=|select|store|not|and|or|<|>|<=|>=|\\+|\\-|\\*)\\s+[^)]+\\)");
        std::sregex_iterator smt_it(content.begin(), content.end(), smt_expr_regex);
        while (smt_it != end_it) {
            std::string expr = (*smt_it)[0].str();
            // Filter out too long or possibly code example expressions
            if (expr.length() < 200) {
                constraints.push_back(expr);
            }
            ++smt_it;
            if (constraints.size() >= 5) break; // Max 5
        }
    }
    
    // Try to extract chain_of_thought / reasoning
    std::string reasoning;
    std::regex cot_regex("\"(?:chain_of_thought|reasoning)\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch cot_match;
    if (std::regex_search(content, cot_match, cot_regex) && cot_match.size() > 1) {
        reasoning = cot_match[1].str();
    } else {
        reasoning = "Extracted from non-standard vLLM response";
    }
    
    // Try to extract UF definitions (if any)
    std::vector<std::string> function_defs;
    std::regex func_regex("\\(define-fun(?:-rec)?\\s+[^)]+\\)\\s+[^)]+\\)\\s+[^)]+\\s+[^)]+\\)");
    std::sregex_iterator func_it(content.begin(), content.end(), func_regex);
    std::sregex_iterator func_end;
    while (func_it != func_end) {
        function_defs.push_back((*func_it).str());
        ++func_it;
    }
    
    // Construct basic JSON
    json result_json = {
        {"success", !constraints.empty() || sat_result != "unknown"},
        {"satisfiability", sat_result},
        {"chain_of_thought", reasoning},
        {"constraints", json::array()}
    };
    
    for (const auto& cons : constraints) {
        result_json["constraints"].push_back({
            {"name", "Extracted constraint"},
            {"constraint", cons},
            {"type", "extracted"},
            {"confidence", 0.5}
        });
    }
    
    // Add UF definitions (if any)
    if (!function_defs.empty()) {
        result_json["functions"] = json::array();
        for (const auto& def : function_defs) {
            result_json["functions"].push_back({
                {"function_name", "extracted"},
                {"definition", def},
                {"confidence", 0.5}
            });
        }
    }
    
    return result_json.dump();
}

std::string LLMClient::extract_unsat_json_fallback(const std::string& content) {
    // Try to extract quantifier instantiation info
    json result_json = {
        {"success", false},
        {"reasoning", "Extracted from non-standard vLLM response"},
        {"instantiations", json::array()},
        {"unsat_prediction", "unknown"}
    };
    
    // Try to extract reasoning
    std::regex reasoning_regex("\"reasoning\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch reasoning_match;
    if (std::regex_search(content, reasoning_match, reasoning_regex) && reasoning_match.size() > 1) {
        result_json["reasoning"] = reasoning_match[1].str();
    }
    
    // Try to extract unsat_prediction
    if (content.find("\"high\"") != std::string::npos || 
        content.find("'high'") != std::string::npos) {
        result_json["unsat_prediction"] = "high";
    } else if (content.find("\"medium\"") != std::string::npos ||
               content.find("'medium'") != std::string::npos) {
        result_json["unsat_prediction"] = "medium";
    } else if (content.find("\"low\"") != std::string::npos ||
               content.find("'low'") != std::string::npos) {
        result_json["unsat_prediction"] = "low";
    }
    
    // Try to extract quantifier_id and witness_map
    // Pattern: "quantifier_id": 0, "witness_map": {"x": "0", ...}
    std::regex qid_regex("\"quantifier_id\"\\s*:\\s*(\\d+)");
    std::regex witness_regex("\"witness_map\"\\s*:\\s*\\{([^}]+)\\}");
    
    std::sregex_iterator qid_it(content.begin(), content.end(), qid_regex);
    std::sregex_iterator end_it;
    
    std::vector<int> qids;
    while (qid_it != end_it) {
        qids.push_back(std::stoi((*qid_it)[1].str()));
        ++qid_it;
    }
    
    // Extract all witness_map
    std::sregex_iterator wm_it(content.begin(), content.end(), witness_regex);
    std::vector<std::string> witness_maps;
    while (wm_it != end_it) {
        witness_maps.push_back((*wm_it)[1].str());
        ++wm_it;
    }
    
    // Try to pair quantifier_id and witness_map
    for (size_t i = 0; i < qids.size() && i < witness_maps.size(); ++i) {
        json inst = {
            {"quantifier_id", qids[i]},
            {"witness_map", json::object()},
            {"confidence", 0.5}
        };
        
        // Parse witness_map content
        // Format: "var1": "value1", "var2": "value2"
        std::string wm = witness_maps[i];
        std::regex kv_regex("\"([^\"]+)\"\\s*:\\s*\"([^\"]+)\"");
        std::sregex_iterator kv_it(wm.begin(), wm.end(), kv_regex);
        while (kv_it != end_it) {
            std::string key = (*kv_it)[1].str();
            std::string val = (*kv_it)[2].str();
            inst["witness_map"][key] = val;
            ++kv_it;
        }
        
        if (!inst["witness_map"].empty()) {
            result_json["instantiations"].push_back(inst);
        }
    }
    
    // Check if extraction successful
    result_json["success"] = !result_json["instantiations"].empty();
    
    return result_json.dump();
}

} // namespace linguaarray 
