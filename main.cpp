#include "lexer.cpp"
#include "ast.hpp"
#include "parser.hpp"
#include "semantic.hpp"
#include "semantic_types.hpp"
#include "semantic_instructions.cpp"
#include "semantic.cpp"

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
  Parser parser(lexer.tokenize());
  SemanticAnalyzer semantic;
  if(semantic.analyze(parser.parseModule())) {
    for(auto& msg : semantic.diagnsotics()) {
        std::cout << msg.message << "\n";
    }
  }
}
