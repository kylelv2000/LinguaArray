#include "quantifier_helper.h"
#include "quantifier_instantiation_adapter.h"
#include <memory>

namespace linguaarray {

// Use singleton pattern to manage adapter
static QuantifierInstantiationAdapter& get_adapter() {
    static QuantifierInstantiationAdapter adapter("python_bridge/quantifier_instantiation.py");
    return adapter;
}

std::string QuantifierHelper::parse_quantifiers(const std::string& smt_file) {
    return get_adapter().parse_quantifiers(smt_file);
}

std::string QuantifierHelper::instantiate_quantifiers(
    const std::string& smt_file,
    const std::string& llm_response,
    const std::string& output_file) {
    
    return get_adapter().instantiate_quantifiers(smt_file, llm_response, output_file);
}

bool QuantifierHelper::check_python_available() {
    return QuantifierInstantiationAdapter::check_python_available();
}

} // namespace linguaarray

