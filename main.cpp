#include <fstream>
#include "lexer.cpp"
#include "ast.hpp"
#include "parser.hpp"
#include "semantic.hpp"
#include "semantic_types.hpp"
#include "semantic_instructions.cpp"
#include "semantic.cpp"
#include "codegen_arch64.cpp"

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

void printUsage(const char* progName) {
    std::cerr << "Usage:\n"
              << "  " << progName << " <file.cro> -o <output_bin>           # Compile & link via gcc\n"
              << "  " << progName << " <file.cro> -s <output.s>           # Save assembly file only\n"
              << "  " << progName << " <file.cro> -o -nomain <output_bin>  # Compile & link raw via ld\n";
}


int main(int argc, char* argv[]) {
  if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    std::string inputFile = argv[1];
    std::string flag = argv[2];
    
    bool rawLdMode = false;
    std::string outputFilename;

    if (flag == "-s") {
        outputFilename = argv[3];
    } else if (flag == "-o") {
        if (std::string(argv[3]) == "-nomain") {
            if (argc < 5) {
                std::cerr << "Error: Missing output filename after -nomain\n";
                return 1;
            }
            rawLdMode = true;
            outputFilename = argv[4];
        } else {
            outputFilename = argv[3];
        }
    } else {
        std::cerr << "Error: Unknown flag '" << flag << "'\n";
        printUsage(argv[0]);
        return 1;
    }

    // 1. Baca file source .cro
    std::string source;
    try {
        source = readFile(inputFile);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
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

    if (flag == "-s") {
        std::ofstream outFile(outputFilename);
        if (!outFile) {
            std::cerr << "Error: Cannot write to assembly file " << outputFilename << '\n';
            return 1;
        }
        outFile << assembly;
        std::cout << "Successfully generated assembly: " << outputFilename << '\n';
        return 0;
    }

    // Untuk opsi -o, kita simpan sementara ke file assembly penampung (misal: temp_out.s)
    std::string tempAssemblyFile = "temp_output_cru.s";
    {
        std::ofstream tempFile(tempAssemblyFile);
        tempFile << assembly;
    }

    std::string compileCommand;
    if (rawLdMode) {
        // Menggunakan assembler (as) lalu linker murni (ld) tanpa runtime startup gcc
        std::string tempObjFile = "temp_output_cru.o";
        compileCommand = "as " + tempAssemblyFile + " -o " + tempObjFile + 
                         " && ld " + tempObjFile + " -o " + outputFilename + 
                         " && rm " + tempObjFile;
    } else {
        // Menggunakan gcc untuk otomatis menangani crt0, libc, dll.
        compileCommand = "gcc " + tempAssemblyFile + " -o " + outputFilename;
    }

    // Eksekusi perintah sistem (as/ld/gcc)
    int result = std::system(compileCommand.c_str());
    
    // Hapus file assembly sementara
    std::remove(tempAssemblyFile.c_str());

    if (result != 0) {
        std::cerr << "Error: Compilation/Linking failed.\n";
        return 1;
    }

    std::cout << "Successfully compiled to executable: " << outputFilename << '\n';
    return 0;

}
