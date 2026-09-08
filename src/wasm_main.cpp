#include <emscripten.h>
#include <iostream>
#include <sstream>
#include <string>
#include <memory>

#include "lexer.hpp"
#include "parser.hpp"
#include "interpreter.hpp"
#include "typechecker.hpp"

extern "C" {

EMSCRIPTEN_KEEPALIVE
const char* nusa_eval(const char* source) {
    static std::string output_buffer;
    output_buffer.clear();

    if (!source || source[0] == '\0') {
        return "";
    }

    std::stringstream captured_stdout;

    try {
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        std::unique_ptr<Program> program = parser.parse();

        TypeChecker checker;
        checker.check(*program);

        Interpreter interpreter(".");
        interpreter.setOutputStream(&captured_stdout);
        interpreter.run(*program);

        output_buffer = captured_stdout.str();
        return output_buffer.c_str();

    } catch (const std::exception& e) {
        output_buffer = std::string("ERROR: ") + e.what();
        return output_buffer.c_str();
    } catch (...) {
        output_buffer = "ERROR: Unknown exception occurred";
        return output_buffer.c_str();
    }
}

}
