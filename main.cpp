#include <fstream>
#include "lexer.cpp"
#include "ast.hpp"
#include "parser.hpp"
#include "semantic.hpp"
#include "semantic_types.hpp"
#include "semantic_instructions.cpp"
#include "semantic.cpp"

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

    std::string assembly =
        cru::codegen::codegenArch(std::move(module));

    std::cout << assembly;

}
