#ifndef LINGUAARRAY_PYTHON_PARSER_WRAPPER_H
#define LINGUAARRAY_PYTHON_PARSER_WRAPPER_H

#include <string>
#include <vector>
#include <fstream>
#include "smt_parser.h"

namespace linguaarray {

/**
 * Python Parser Wrapper Class
 * Calls Python script (using pySMT) to parse SMT files for higher robustness
 */
class PythonParserWrapper {
public:
    /**
     * Constructor
     * @param python_script_path Path to the Python script
     */
    explicit PythonParserWrapper(const std::string& python_script_path);
    
    /**
     * Parse SMT file
     * @param filepath File path
     * @return Parsing result
     */
    ParseResult parse_file(const std::string& filepath);
    
    /**
     * Parse SMT content
     * @param content SMT content string
     * @return Parsing result
     */
    ParseResult parse_content(const std::string& content);

private:
    std::string python_script_path_;
};

} // namespace linguaarray

#endif // LINGUAARRAY_PYTHON_PARSER_WRAPPER_H

