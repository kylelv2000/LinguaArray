#include "engine.h"
#include "quantifier_helper.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace linguaarray {

// Helper function: extract case name from file path
std::string extract_case_name(const std::string& file_path) {
    std::filesystem::path path(file_path);
    return path.stem().string(); // Get filename without extension
}

LinguaArrayEngine::LinguaArrayEngine(const Config& config) 
    : config_(config) {
    
    // Use Python parser adapter (based on z3py, supports all SMT-LIB logics)
    parser_ = std::make_unique<PythonParserAdapter>("python_bridge/smt_parser.py");
    
    // Create solver, different solvers need special arguments
    std::string solver_args;
    if (config_.solver.binary.find("cvc5") != std::string::npos) {
        solver_args = "--mbqi --finite-model-find --enum-inst --arrays-exp --fp-exp";
    } else if (config_.solver.binary.find("z3") != std::string::npos) {
        solver_args = "-smt2";
    }
    solver_ = std::make_unique<SMTSolver>(config_.solver.binary, solver_args, config_.solver.timeout);
    
    // Only create LLM client when API key exists
    if (!config_.llm.api_key.empty()) {
        llm_client_ = std::make_unique<LLMClient>(config_);
    }
}

LinguaArrayResult LinguaArrayEngine::solve_file(const std::string& file_path) {
    auto start = std::chrono::high_resolution_clock::now();
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "Starting to solve file: " << file_path << std::endl;
        std::cout << "Using solver: " << config_.solver.binary << std::endl;
    }
    
    // Step 1: Parse file
    auto parse_result = parser_->parse_file(file_path);
    
    if (!parse_result.success) {
        LinguaArrayResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "File parsing failed: " + parse_result.error_message;
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        return result;
    }
    
    // Step 1.5: Simplify SMT file
    auto simplify_result = parser_->simplify_smt(parse_result.content);
    if (simplify_result.success) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Simplified: " << simplify_result.variable_mapping.size() << " variables renamed, "
                      << simplify_result.constant_substitutions.size() << " constants inlined" << std::endl;
        }
        parse_result.content = simplify_result.simplified_content;
    }
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "Found " << parse_result.uninterpreted_functions.size() << " uninterpreted functions, "
                  << parse_result.array_variables.size() << " array variables" << std::endl;
    }
    
    // Step 2: Check if LLM enhancement needed
    if ((!parse_result.uninterpreted_functions.empty() || !parse_result.array_variables.empty()) && llm_client_) {
        return solve_with_llm(parse_result, file_path);
    } else {
        // Use original solver directly
        return solve_direct(parse_result.content, file_path);
    }
}

bool LinguaArrayEngine::generate_llm_json(const std::string& file_path, const std::string& output_base) {
    if (!llm_client_) {
        std::cerr << "Error: LLM API key not set" << std::endl;
        return false;
    }
    
    auto parse_result = parser_->parse_file(file_path);
    if (!parse_result.success) {
        std::cerr << "File parsing failed: " << parse_result.error_message << std::endl;
        return false;
    }
    
    // Simplify content
    auto simplify_result = parser_->simplify_smt(parse_result.content);
    if (simplify_result.success) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Simplified: " << simplify_result.variable_mapping.size() << " variables renamed, "
                      << simplify_result.constant_substitutions.size() << " constants inlined" << std::endl;
        }
        parse_result.content = simplify_result.simplified_content;
    }
    
    // Re-parse with simplified content
    parse_result = parser_->parse_content(parse_result.content);
    if (!parse_result.success) {
        std::cerr << "Simplified content parsing failed: " << parse_result.error_message << std::endl;
        return false;
    }
    
    // Check content size
    constexpr size_t MAX_CONTENT_SIZE = 50 * 1024;
    if (parse_result.content.size() > MAX_CONTENT_SIZE) {
        std::cerr << "File too large (" << parse_result.content.size() / 1024 << "KB), skipping LLM" << std::endl;
        return false;
    }
    
    // Generate filenames: {base}_sat.json, {base}_unsat.json, {base}_simplified.smt2
    std::string base = output_base;
    if (base.size() > 5 && base.substr(base.size() - 5) == ".json") {
        base = base.substr(0, base.size() - 5);
    }
    std::string sat_json = base + "_sat.json";
    std::string unsat_json = base + "_unsat.json";
    std::string simplified_smt = base + "_simplified.smt2";
    
    // Save simplified SMT file
    {
        std::ofstream out(simplified_smt);
        if (out) {
            out << parse_result.content;
            out.close();
            if (config_.log_level >= LogLevel::DEBUG) {
                std::cout << "Simplified file saved: " << simplified_smt << std::endl;
            }
        }
    }
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "Generating SAT and UNSAT LLM JSON in parallel..." << std::endl;
    }
    
    // Generate SAT and UNSAT JSON in parallel
    std::atomic<bool> sat_success(false);
    std::atomic<bool> unsat_success(false);
    
    // SAT thread
    std::thread sat_thread([&, file_path]() {
        sat_success.store(llm_client_->generate_constraints_json(
            parse_result.content,
            parse_result.uninterpreted_functions,
            parse_result.array_variables,
            sat_json,
            file_path  // Pass original file path for hints matching
        ));
    });
    
    // UNSAT thread: parse quantifiers and generate instantiation JSON
    std::thread unsat_thread([&, file_path]() {
        if (!QuantifierHelper::check_python_available()) {
            return;
        }
        
        std::string parse_result_json = QuantifierHelper::parse_quantifiers(simplified_smt);
        if (parse_result_json.empty()) {
            return;
        }
        
        try {
            json parse_data = json::parse(parse_result_json);
            if (!parse_data.value("has_quantifiers", false)) {
                // No quantifiers, create empty unsat json
                std::ofstream out(unsat_json);
                out << "{\"has_quantifiers\": false}";
                out.close();
                unsat_success.store(true);
                return;
            }
            
            std::string llm_prompt = parse_data.value("llm_prompt", "");
            if (llm_prompt.empty()) {
                return;
            }
            
            unsat_success.store(llm_client_->call_llm_with_prompt(llm_prompt, unsat_json, file_path));
        } catch (...) {
            return;
        }
    });
    
    sat_thread.join();
    unsat_thread.join();
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "SAT JSON: " << (sat_success.load() ? "Success" : "Failed") << std::endl;
        std::cout << "UNSAT JSON: " << (unsat_success.load() ? "Success" : "No quantifiers/Failed") << std::endl;
    }
    
    return sat_success.load();  // Mainly check if SAT succeeded
}

LinguaArrayResult LinguaArrayEngine::solve_with_json(const std::string& file_path, const std::string& json_file) {
    auto start = std::chrono::high_resolution_clock::now();
    
    // Parse json path, support new format ({base}_sat.json) and old format ({name}.json)
    std::string sat_json = json_file;
    std::string simplified_file;
    
    // Check if new format
    std::string base = json_file;
    if (base.size() > 5 && base.substr(base.size() - 5) == ".json") {
        base = base.substr(0, base.size() - 5);
    }
    if (base.size() > 4 && base.substr(base.size() - 4) == "_sat") {
        base = base.substr(0, base.size() - 4);
        sat_json = base + "_sat.json";
        simplified_file = base + "_simplified.smt2";
    } else {
        // Old format
        simplified_file = base + "_simplified.smt2";
    }
    
    // Use simplified file or original file
    std::string actual_file = file_path;
    if (std::filesystem::exists(simplified_file)) {
        actual_file = simplified_file;
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Using simplified file: " << simplified_file << std::endl;
        }
    } else {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Using original file: " << file_path << std::endl;
        }
    }
    
    auto parse_result = parser_->parse_file(actual_file);
    if (!parse_result.success) {
        LinguaArrayResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "File parsing failed: " + parse_result.error_message;
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        return result;
    }
    
    // If using original file, need to simplify
    if (actual_file == file_path) {
        auto simplify_result = parser_->simplify_smt(parse_result.content);
        if (simplify_result.success) {
            if (config_.log_level >= LogLevel::DEBUG) {
                std::cout << "Simplified: " << simplify_result.variable_mapping.size() << " variables renamed, "
                          << simplify_result.constant_substitutions.size() << " constants inlined" << std::endl;
            }
            parse_result.content = simplify_result.simplified_content;
        }
    }
    
    auto llm_response = LLMClient::read_llm_response_from_json(sat_json);
    if (llm_response.constraints.empty() && llm_response.functions.empty()) {
        // JSON content invalid
        if (config_.no_fallback) {
            LinguaArrayResult result;
            result.result = SolverResult::UNKNOWN;
            result.error_message = "JSON invalid and fallback disabled";
            auto end = std::chrono::high_resolution_clock::now();
            result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            return result;
        }
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "JSON invalid (no constraints/UF), falling back to baseline" << std::endl;
        }
        return solve_direct(parse_result.content, file_path, json_file);
    }
    
    // Collect LLM provided constraints
    std::vector<std::string> constraints;
    for (const auto& cons : llm_response.constraints) {
        constraints.push_back(cons.constraint);
    }
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "LLM satisfiability prediction: " << llm_response.satisfiability << std::endl;
        if (!llm_response.functions.empty()) {
            std::cout << "LLM provided " << llm_response.functions.size() << " UF definitions" << std::endl;
        }
    }
    
    std::string case_name = extract_case_name(file_path);
    
    // First handle UF definitions (if any)
    std::string content_with_uf = parse_result.content;
    ReplaceResult replace_result{false, "", "", {}};  // Initialize to default
    
    if (!llm_response.functions.empty()) {
        std::vector<std::pair<std::string, std::string>> function_defs;
        int skipped_low_confidence = 0;
        
        // Only apply high confidence UF definitions (>= 0.5)
        for (const auto& func : llm_response.functions) {
            if (func.confidence >= 0.5) {
                function_defs.emplace_back(func.function_name, func.definition);
            } else {
                skipped_low_confidence++;
                if (config_.log_level >= LogLevel::DEBUG) {
                    std::cout << "Skipping low confidence UF definition: " << func.function_name 
                              << " (confidence=" << func.confidence << ")" << std::endl;
                }
            }
        }
        
        if (skipped_low_confidence > 0 && config_.log_level >= LogLevel::INFO) {
            std::cout << "Skipped " << skipped_low_confidence << " low confidence UF definitions" << std::endl;
        }
        
        if (!function_defs.empty()) {
            replace_result = parser_->replace_function_definitions(content_with_uf, function_defs);
            if (replace_result.success) {
                content_with_uf = replace_result.new_content;
                if (config_.log_level >= LogLevel::INFO) {
                    std::cout << "Successfully replaced " << function_defs.size() << " UF definitions" << std::endl;
                }
            } else if (config_.log_level >= LogLevel::WARN) {
                std::cerr << "Warning: UF definition replacement failed: " << replace_result.error_message << std::endl;
            }
        }
    }
    
    // Then insert constraints into original formula
    std::string content_with_constraints = content_with_uf;
    for (size_t i = 0; i < constraints.size(); ++i) {
        size_t check_sat_pos = content_with_constraints.rfind("(check-sat)");
        if (check_sat_pos != std::string::npos) {
            content_with_constraints.insert(check_sat_pos, "(assert " + constraints[i] + ")\n");
        }
    }
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "Inserted " << constraints.size() << " LLM constraints, starting solve (timeout " 
                  << config_.llm.intermediate_timeout << "s)" << std::endl;
    }
    
    auto solve_result = solver_->solve_content(
        content_with_constraints,
        config_.output_dir,
        config_.save_intermediate,
        case_name + "_with_constraints",
        config_.llm.intermediate_timeout
    );
    
    LinguaArrayResult result;
    result.llm_used = true;
    result.functions_instantiated = static_cast<int>(constraints.size() + llm_response.functions.size());
    
    // If SAT after inserting constraints, original problem must be SAT
    if (solve_result.result == SolverResult::SAT) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "SAT after inserting constraints, confirming original problem is SAT" << std::endl;
        }
        result.result = SolverResult::SAT;
        result.error_message = solve_result.error_message;
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        return result;
    }
    
    // If UNSAT after inserting constraints, need to verify UF and constraint completeness
    if (solve_result.result == SolverResult::UNSAT && !config_.no_fallback) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "UNSAT after inserting constraints, starting completeness verification" << std::endl;
        }
        
        // Step 1: Generate negation constraint for UF definitions (only for actually applied high confidence UFs)
        if (replace_result.success && !replace_result.instantiated_functions.empty()) {
            if (config_.log_level >= LogLevel::INFO) {
                std::cout << "Generating negation constraint for " << replace_result.instantiated_functions.size() 
                          << " applied UFs" << std::endl;
            }
            
            // Get instantiated function info from replace_result
            std::string uf_negation_constraint = parser_->generate_negation_constraint(
                replace_result.instantiated_functions
            );
            
            if (!uf_negation_constraint.empty()) {
                // Add UF negation constraint to original content (without UF definitions)
                std::string content_with_uf_negation = parser_->add_negation_constraint_to_content(
                    parse_result.content,
                    uf_negation_constraint
                );
                
                if (config_.log_level >= LogLevel::INFO) {
                    std::cout << "Verifying UF negation constraint (timeout " << config_.llm.intermediate_timeout << "s)" << std::endl;
                }
                
                auto uf_negation_result = solver_->solve_content(
                    content_with_uf_negation,
                    config_.output_dir,
                    config_.save_intermediate,
                    case_name + "_uf_negation_check",
                    config_.llm.intermediate_timeout
                );
                
                if (uf_negation_result.result == SolverResult::UNSAT) {
                    if (config_.log_level >= LogLevel::INFO) {
                        std::cout << "UF negation constraint returned UNSAT, confirming UF definition uniqueness" << std::endl;
                    }
                    // UF definition unique, can continue verifying constraints
                } else if (uf_negation_result.result == SolverResult::SAT) {
                    if (config_.log_level >= LogLevel::INFO) {
                        std::cout << "UF negation constraint returned SAT, other UF definitions exist that make original problem SAT" << std::endl;
                    }
                    result.result = SolverResult::SAT;
                    result.error_message = uf_negation_result.error_message;
                    auto end = std::chrono::high_resolution_clock::now();
                    result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                    return result;
                } else {
                    // TIMEOUT or ERROR, cannot determine
                    if (config_.no_fallback) {
                        result.result = SolverResult::UNKNOWN;
                        auto end = std::chrono::high_resolution_clock::now();
                        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                        return result;
                    }
                    if (config_.log_level >= LogLevel::INFO) {
                        std::cout << "UF negation constraint verification undetermined, falling back to original solver" << std::endl;
                    }
                    auto fallback_result = solve_direct(parse_result.content, file_path, json_file);
                    result.result = fallback_result.result;
                    result.error_message = fallback_result.error_message;
                    result.llm_used = false;
                    auto end = std::chrono::high_resolution_clock::now();
                    result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                    return result;
                }
            }
        }
        
        // Step 2: Verify constraints by negating one at a time
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Starting constraint negation verification" << std::endl;
        }
        
        bool all_negations_unsat = true;
        for (size_t i = 0; i < constraints.size(); ++i) {
            // Negation verification must include UF definitions!
            std::string content_with_negated = content_with_uf;
            size_t check_sat_pos = content_with_negated.rfind("(check-sat)");
            if (check_sat_pos != std::string::npos) {
                content_with_negated.insert(check_sat_pos, "(assert (not " + constraints[i] + "))\n");
            }
            
            if (config_.log_level >= LogLevel::DEBUG) {
                std::cout << "Verifying negated constraint " << (i+1) << "/" << constraints.size() << std::endl;
            }
            
            auto negation_result = solver_->solve_content(
                content_with_negated,
                config_.output_dir,
                config_.save_intermediate,
                case_name + "_negation_" + std::to_string(i),
                config_.llm.intermediate_timeout
            );
            
            if (negation_result.result == SolverResult::SAT) {
                if (config_.log_level >= LogLevel::INFO) {
                    std::cout << "Negated constraint " << (i+1) << " returned SAT, original problem is SAT" << std::endl;
                }
                result.result = SolverResult::SAT;
                result.error_message = negation_result.error_message;
                auto end = std::chrono::high_resolution_clock::now();
                result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                return result;
            }
            
            if (negation_result.result != SolverResult::UNSAT) {
                all_negations_unsat = false;
                if (config_.log_level >= LogLevel::DEBUG) {
                    std::cout << "Negated constraint " << (i+1) << " undetermined, continuing" << std::endl;
                }
            }
        }
        
        if (all_negations_unsat) {
            if (config_.log_level >= LogLevel::INFO) {
                std::cout << "All negated constraints are UNSAT, confirming original problem is UNSAT" << std::endl;
            }
            result.result = SolverResult::UNSAT;
            result.error_message = solve_result.error_message;
            auto end = std::chrono::high_resolution_clock::now();
            result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            return result;
        }
        
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Negation verification not fully determined, falling back to original solver" << std::endl;
        }
    } else {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Returned " << solver_result_to_string(solve_result.result) << " after inserting constraints, falling back to original solver" << std::endl;
        }
    }
    
    // SAT path undetermined, try UNSAT path
    std::string unsat_json = base + "_unsat.json";
    if (std::filesystem::exists(unsat_json)) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "SAT path undetermined, trying UNSAT path..." << std::endl;
        }
        
        // Read UNSAT JSON
        std::ifstream f(unsat_json);
        if (f.is_open()) {
            std::stringstream buffer;
            buffer << f.rdbuf();
            std::string llm_response = buffer.str();
            f.close();
            
            // Check if has quantifier instantiation
            try {
                json data = json::parse(llm_response);
                if (data.value("has_quantifiers", true) && data.contains("instantiations")) {
                    // Execute quantifier instantiation - use base prefix for consistency
                    std::string instantiated_file = base + "_instantiated.smt2";
                    std::string inst_result = QuantifierHelper::instantiate_quantifiers(
                        actual_file, llm_response, instantiated_file
                    );
                    
                    if (!inst_result.empty() && std::filesystem::exists(instantiated_file)) {
                        if (config_.log_level >= LogLevel::INFO) {
                            std::cout << "[UNSAT] Instantiation complete, starting solve" << std::endl;
                        }
                        
                        auto inst_parse = parser_->parse_file(instantiated_file);
                        if (inst_parse.success) {
                            auto unsat_solve = solver_->solve_content(
                                inst_parse.content,
                                config_.output_dir,
                                config_.save_intermediate,
                                case_name + "_unsat_path",
                                config_.llm.intermediate_timeout
                            );
                            
                            if (unsat_solve.result == SolverResult::UNSAT) {
                                if (config_.log_level >= LogLevel::INFO) {
                                    std::cout << "[UNSAT] UNSAT after instantiation, confirming original problem is UNSAT" << std::endl;
                                }
                                result.result = SolverResult::UNSAT;
                                result.unsat_accelerated = true;
                                result.llm_used = true;
                                auto end = std::chrono::high_resolution_clock::now();
                                result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                                return result;
                            }
                        }
                    }
                }
            } catch (...) {
                // JSON parsing failed, continue
            }
        }
    }
    
    // Fall back to direct solve
    if (config_.no_fallback) {
        result.result = SolverResult::UNKNOWN;
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        return result;
    }
    auto fallback_result = solve_direct(parse_result.content, file_path, json_file);
    result.result = fallback_result.result;
    result.error_message = fallback_result.error_message;
    result.llm_used = false;
    auto end = std::chrono::high_resolution_clock::now();
    result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    return result;
}

LinguaArrayResult LinguaArrayEngine::solve_with_llm(const SMTParseResult& parse_result, const std::string& file_path) {
    auto start = std::chrono::high_resolution_clock::now();
    
    // File too large, fall back to traditional method directly (threshold 50KB)
    constexpr size_t MAX_CONTENT_SIZE = 50 * 1024;
    if (parse_result.content.size() > MAX_CONTENT_SIZE) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "File too large (" << parse_result.content.size() / 1024 << "KB), skipping LLM, falling back to traditional solver" << std::endl;
        }
        return solve_direct(parse_result.content, file_path);
    }
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "Starting LLM enhanced solve flow" << std::endl;
    }
    
    // Generate output_base path (under output_dir, using case_name as prefix)
    std::string case_name = extract_case_name(file_path);
    std::filesystem::create_directories(config_.output_dir);
    std::string output_base = (std::filesystem::path(config_.output_dir) / case_name).string();
    
    // Generate temp JSON file
    std::string temp_json;
    if (config_.save_intermediate) {
        // When saving intermediate files, use meaningful naming
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        temp_json = output_base + "_llm_functions_" + std::to_string(timestamp) + ".json";
    } else {
        // When not saving intermediate files, use generic temp filename
        temp_json = output_base + "_linguaarray_functions.json";
    }
    
    // Save simplified SMT file for UNSAT acceleration
    std::string simplified_smt = output_base + "_for_unsat.smt2";
    std::ofstream smt_out(simplified_smt);
    if (smt_out) {
        smt_out << parse_result.content;
        smt_out.close();
    }
    
    // Check if can use UNSAT acceleration (requires Python and quantifiers)
    bool can_use_unsat_accel = QuantifierHelper::check_python_available();
    
    // Start two threads in parallel
    std::atomic<bool> finished(false);
    std::atomic<bool> unsat_found(false);
    std::mutex result_mutex;
    LinguaArrayResult unsat_result;
    
    std::thread unsat_thread;
    std::atomic<bool> sat_llm_success(false);
    
    // UNSAT thread
    if (can_use_unsat_accel && llm_client_) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Starting parallel UNSAT acceleration thread" << std::endl;
        }
        unsat_thread = std::thread(
            &LinguaArrayEngine::unsat_acceleration_thread,
            this,
            simplified_smt,
            case_name,
            std::ref(finished),
            std::ref(unsat_found),
            std::ref(result_mutex),
            std::ref(unsat_result)
        );
    }
    
    // SAT LLM thread (parallel)
    std::thread sat_thread([&, file_path]() {
        sat_llm_success.store(llm_client_->generate_constraints_json(
            parse_result.content,
            parse_result.uninterpreted_functions,
            parse_result.array_variables,
            temp_json,
            file_path  // Pass original file path for hints matching
        ));
    });
    
    // Wait for SAT LLM to complete
    sat_thread.join();
    bool llm_success = sat_llm_success.load();
    
    
    if (!llm_success) {
        finished.store(true);
        if (unsat_thread.joinable()) {
            unsat_thread.join();
        }
        
        // If UNSAT thread found result, return it
        if (unsat_found.load()) {
            if (config_.log_level >= LogLevel::INFO) {
                std::cout << "SAT path failed, but UNSAT acceleration succeeded" << std::endl;
            }
            return unsat_result;
        }
        
        LinguaArrayResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "LLM constraint generation failed";
        result.llm_used = false;
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (!config_.save_intermediate) {
            std::filesystem::remove(temp_json);
            std::filesystem::remove(simplified_smt);
        }
        return result;
    }
    
    // Check if UNSAT thread already found answer
    if (unsat_found.load()) {
        finished.store(true);
        if (unsat_thread.joinable()) {
            unsat_thread.join();
        }
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "UNSAT acceleration already found answer, returning early" << std::endl;
        }
        if (!config_.save_intermediate) {
            std::filesystem::remove(temp_json);
            std::filesystem::remove(simplified_smt);
        }
        return unsat_result;
    }
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "LLM constraint generation complete, starting solve" << std::endl;
    }
    
    auto result = solve_with_json(file_path, temp_json);
    
    // Mark finished, wait for UNSAT thread
    finished.store(true);
    if (unsat_thread.joinable()) {
        unsat_thread.join();
    }
    
    // If SAT path no determined result, but UNSAT found, use UNSAT result
    if (result.result == SolverResult::UNKNOWN && unsat_found.load()) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "SAT path undetermined, using UNSAT acceleration result" << std::endl;
        }
        result = unsat_result;
    }
    
    // Update total runtime (including LLM generation time)
    auto end = std::chrono::high_resolution_clock::now();
    result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Decide whether to keep JSON file based on save_intermediate config
    if (!config_.save_intermediate) {
        std::filesystem::remove(temp_json);
        std::filesystem::remove(simplified_smt);
    } else {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "Intermediate JSON file saved: " << temp_json << std::endl;
        }
    }
    
    return result;
}

LinguaArrayResult LinguaArrayEngine::solve_direct(const std::string& smt_content, const std::string& file_path, const std::string& json_file) {
    auto start = std::chrono::high_resolution_clock::now();
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "Using original SMT solver (timeout " << config_.solver.timeout << "s)" << std::endl;
    }
    
    // Use original content directly for solving
    std::string content_for_solving = smt_content;
    
    std::string case_suffix = file_path.empty() ? "direct" : extract_case_name(file_path) + "_direct";
    
    auto solve_result = solver_->solve_content(
        content_for_solving,
        config_.output_dir,
        config_.save_intermediate,
        case_suffix
    );
    
    LinguaArrayResult result;
    result.result = solve_result.result;
    result.llm_used = false;
    result.functions_instantiated = 0;
    result.error_message = solve_result.error_message;
    
    auto end = std::chrono::high_resolution_clock::now();
    result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    return result;
}

bool LinguaArrayEngine::generate_unsat_instantiation_json(
    const std::string& file_path, 
    const std::string& output_json) {
    
    if (!llm_client_) {
        std::cerr << "Error: LLM API key not set" << std::endl;
        return false;
    }
    
    auto parse_result = parser_->parse_file(file_path);
    if (!parse_result.success) {
        std::cerr << "File parsing failed: " << parse_result.error_message << std::endl;
        return false;
    }
    
    // Simplify content
    auto simplify_result = parser_->simplify_smt(parse_result.content);
    if (simplify_result.success) {
        parse_result.content = simplify_result.simplified_content;
    }
    
    // Save simplified temp file
    std::string temp_smt = output_json + ".temp.smt2";
    std::ofstream temp_file(temp_smt);
    if (!temp_file.is_open()) {
        std::cerr << "Unable to create temp SMT file" << std::endl;
        return false;
    }
    temp_file << parse_result.content;
    temp_file.close();
    
    // Parse quantifiers, get LLM prompt
    std::string parse_json = QuantifierHelper::parse_quantifiers(temp_smt);
    std::filesystem::remove(temp_smt);
    
    if (parse_json.empty()) {
        std::cerr << "Quantifier parsing failed" << std::endl;
        return false;
    }
    
    // Extract llm_prompt
    try {
        json data = json::parse(parse_json);
        if (!data.value("has_quantifiers", false)) {
            std::cerr << "No quantifiers to instantiate" << std::endl;
            return false;
        }
        std::string llm_prompt = data.value("llm_prompt", "");
        if (llm_prompt.empty()) {
            std::cerr << "Unable to generate LLM prompt" << std::endl;
            return false;
        }
        
        // Check prompt size
        constexpr size_t MAX_PROMPT_SIZE = 50 * 1024;
        if (llm_prompt.size() > MAX_PROMPT_SIZE) {
            std::cerr << "Prompt too large (" << llm_prompt.size() / 1024 << "KB), skipping LLM" << std::endl;
            return false;
        }
        
        // Call LLM
        return llm_client_->call_llm_with_prompt(llm_prompt, output_json, file_path);
    } catch (...) {
        std::cerr << "Quantifier JSON parsing failed" << std::endl;
        return false;
    }
}

LinguaArrayResult LinguaArrayEngine::solve_with_unsat_acceleration(const std::string& file_path) {
    auto start = std::chrono::high_resolution_clock::now();
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "[UNSAT] Starting quantifier instantiation acceleration: " << file_path << std::endl;
    }
    
    // Derive output_base from file path (remove suffix)
    std::string output_base = file_path;
    // Remove _for_unsat.smt2 suffix to get base path
    if (output_base.size() > 13 && output_base.substr(output_base.size() - 13) == "_for_unsat.smt2") {
        output_base = output_base.substr(0, output_base.size() - 13);
    } else if (output_base.size() > 5 && output_base.substr(output_base.size() - 5) == ".smt2") {
        output_base = output_base.substr(0, output_base.size() - 5);
    }
    
    // Ensure output directory exists
    auto output_dir_path = std::filesystem::path(output_base).parent_path();
    if (!output_dir_path.empty()) {
        std::filesystem::create_directories(output_dir_path);
    }
    
    // File is already simplified, use directly
    std::string simplified_file = file_path;
    
    // Parse quantifiers, get LLM prompt
    if (config_.log_level >= LogLevel::DEBUG) {
        std::cout << "[UNSAT] Parsing quantifiers..." << std::endl;
    }
    std::string parse_result_json = QuantifierHelper::parse_quantifiers(simplified_file);
    if (parse_result_json.empty()) {
        LinguaArrayResult result;
        result.result = SolverResult::UNKNOWN;
        result.error_message = "Quantifier parsing failed";
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (!config_.save_intermediate) {
            std::filesystem::remove(simplified_file);
        }
        return result;
    }
    
    // Save quantifier parse result - use output_base
    std::string quantifier_json_file = output_base + "_unsat_quantifiers.json";
    {
        std::ofstream qf(quantifier_json_file);
        if (qf) {
            qf << parse_result_json;
            qf.close();
        }
    }
    
    // Parse JSON to get llm_prompt
    std::string llm_prompt;
    bool has_quantifiers = false;
    try {
        json parse_data = json::parse(parse_result_json);
        has_quantifiers = parse_data.value("has_quantifiers", false);
        llm_prompt = parse_data.value("llm_prompt", "");
    } catch (...) {
        LinguaArrayResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "Quantifier JSON parsing failed";
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        return result;
    }
    
    if (!has_quantifiers || llm_prompt.empty()) {
        if (config_.log_level >= LogLevel::DEBUG) {
            std::cout << "[UNSAT] No quantifiers to instantiate" << std::endl;
        }
        LinguaArrayResult result;
        result.result = SolverResult::UNKNOWN;
        result.error_message = "No quantifiers to instantiate";
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (!config_.save_intermediate) {
            std::filesystem::remove(simplified_file);
            std::filesystem::remove(quantifier_json_file);
        }
        return result;
    }
    
    // Check prompt size, skip LLM if too large
    constexpr size_t MAX_PROMPT_SIZE = 50 * 1024;
    if (llm_prompt.size() > MAX_PROMPT_SIZE) {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "[UNSAT] Prompt too large (" << llm_prompt.size() / 1024 << "KB), skipping LLM" << std::endl;
        }
        LinguaArrayResult result;
        result.result = SolverResult::UNKNOWN;
        result.error_message = "Prompt too large, skipping LLM";
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        return result;
    }
    
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "[UNSAT] Found quantifiers, calling LLM to generate instantiation suggestions..." << std::endl;
    }
    
    // Call LLM - use output_base
    std::string llm_response_file = output_base + "_unsat_llm_response.json";
    
    if (!llm_client_->call_llm_with_prompt(llm_prompt, llm_response_file, file_path)) {
        LinguaArrayResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "LLM call failed";
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (!config_.save_intermediate) {
            std::filesystem::remove(simplified_file);
            std::filesystem::remove(quantifier_json_file);
        }
        return result;
    }
    
    if (config_.log_level >= LogLevel::DEBUG) {
        std::cout << "[UNSAT] LLM response saved: " << llm_response_file << std::endl;
    }
    
    // Read LLM response
    std::ifstream inst_file(llm_response_file);
    if (!inst_file.is_open()) {
        LinguaArrayResult result;
        result.result = SolverResult::ERROR;
        result.error_message = "Unable to read LLM response";
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        return result;
    }
    
    std::stringstream buffer;
    buffer << inst_file.rdbuf();
    std::string llm_response = buffer.str();
    inst_file.close();
    
    // Execute instantiation
    if (config_.log_level >= LogLevel::INFO) {
        std::cout << "[UNSAT] Executing quantifier instantiation..." << std::endl;
    }
    
    try {
        // Use output_base
        std::string instantiated_file = output_base + "_instantiated.smt2";
        
        std::string output_file = QuantifierHelper::instantiate_quantifiers(
            simplified_file, llm_response, instantiated_file
        );
        
        if (output_file.empty() || !std::filesystem::exists(output_file)) {
            LinguaArrayResult result;
            result.result = SolverResult::ERROR;
            result.error_message = "Quantifier instantiation failed";
            auto end = std::chrono::high_resolution_clock::now();
            result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            return result;
        }
        
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "[UNSAT] Instantiation complete: " << output_file << std::endl;
            std::cout << "[UNSAT] Starting to solve instantiated formula..." << std::endl;
        }
        
        // Solve instantiated file
        auto inst_parse_result = parser_->parse_file(output_file);
        if (!inst_parse_result.success) {
            LinguaArrayResult result;
            result.result = SolverResult::ERROR;
            result.error_message = "Instantiated file parsing failed";
            auto end = std::chrono::high_resolution_clock::now();
            result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            return result;
        }
        
        // Solver temp files still use output_dir (these are solver internal temp files)
        std::string case_name = std::filesystem::path(output_base).filename().string();
        auto solve_result = solver_->solve_content(
            inst_parse_result.content,
            config_.output_dir,
            config_.save_intermediate,
            case_name + "_instantiated_solver",
            config_.llm.intermediate_timeout
        );
        
        LinguaArrayResult result;
        result.result = solve_result.result;
        result.error_message = solve_result.error_message;
        result.unsat_accelerated = true;
        result.llm_used = true;
        
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        if (result.result == SolverResult::UNSAT) {
            if (config_.log_level >= LogLevel::INFO) {
                std::cout << "[UNSAT] UNSAT after instantiation, confirming original problem is UNSAT" << std::endl;
            }
        } else if (config_.log_level >= LogLevel::DEBUG) {
            std::cout << "[UNSAT] Result after instantiation: " << solver_result_to_string(result.result) << std::endl;
        }
        
        // Clean up intermediate files
        if (!config_.save_intermediate) {
            std::filesystem::remove(simplified_file);
            std::filesystem::remove(quantifier_json_file);
            std::filesystem::remove(llm_response_file);
            std::filesystem::remove(output_file);
        } else if (config_.log_level >= LogLevel::INFO) {
            std::cout << "[UNSAT] Intermediate files saved in: " << output_dir_path.string() << std::endl;
        }
        
        return result;
        
    } catch (const std::exception& e) {
        LinguaArrayResult result;
        result.result = SolverResult::ERROR;
        result.error_message = std::string("Quantifier instantiation failed: ") + e.what();
        auto end = std::chrono::high_resolution_clock::now();
        result.runtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        return result;
    }
}

void LinguaArrayEngine::unsat_acceleration_thread(
    const std::string& smt_file,
    const std::string& case_name,
    std::atomic<bool>& finished,
    std::atomic<bool>& unsat_found,
    std::mutex& result_mutex,
    LinguaArrayResult& unsat_result) {
    
    try {
        if (config_.log_level >= LogLevel::INFO) {
            std::cout << "[UNSAT Thread] Starting quantifier instantiation acceleration" << std::endl;
        }
        
        auto result = solve_with_unsat_acceleration(smt_file);
        
        if (result.result == SolverResult::UNSAT && !finished.load()) {
            std::lock_guard<std::mutex> lock(result_mutex);
            if (!finished.load()) {
                unsat_result = result;
                unsat_found.store(true);
                finished.store(true);
                
                if (config_.log_level >= LogLevel::INFO) {
                    std::cout << "[UNSAT Thread] Found UNSAT proof!" << std::endl;
                }
            }
        }
        
    } catch (const std::exception& e) {
        if (config_.log_level >= LogLevel::WARN) {
            std::cerr << "[UNSAT Thread] Exception: " << e.what() << std::endl;
        }
    }
}

} // namespace linguaarray 
