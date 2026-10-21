#pragma once
#include <fstream>
#include "lexer.cpp"
#include "ast.hpp"
#include "parser.hpp"
#include "semantic.hpp"
#include "semantic_types.hpp"
#include "semantic_instructions.cpp"
#include "semantic.cpp"
#include "codegen_arch64.hpp"
#include "codegen_arch64.cpp"
#include "codegen_arch64_globals.cpp"
#include "codegen_arch64_instructions.cpp"
#include "codegen_arch64_frame.cpp"
#include "codegen_arch64_control.cpp"
#include "codegen_arch64_abi.cpp"
#include "codegen_arch64_integrate.cpp"

namespace fs = std::filesystem;

std::string readFile(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int main() {
  std::string source = readFile("main.cro");
  Lexer lexer(source, "main.cro");
  cru::Parser parser(lexer.tokenize());
  cru::sem::SemanticAnalyzer semantic;
  auto module = parser.parseModule();
  if(!semantic.analyze(module)) {
    for (const auto& msg : semantic.diagnostics()) {
    std::cerr << msg.location.file << ":"
              << msg.location.line << ":"
              << msg.location.column << ": "
              << msg.message << '\n';
    }
    return 0;
  }
  auto result = cru::codegen::codegenArch64(
        module,
        "program.s"
    );

}
