#include <fstream>
#include <vector>
#include <filesystem>
#include <sstream>
#include <iostream>
#include "lexer.cpp"
#include "ast.hpp"
#include "parser.hpp"
#include "semantic.hpp"
#include "semantic_types.hpp"
#include "semantic_instructions.cpp"
#include "semantic.cpp"
#include "codegen_arch64.cpp"

namespace fs = std::filesystem;

// ANSI Color Codes untuk Print Usage & Status yang Colorful
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RED     "\033[31m"
#define COLOR_MAGENTA "\033[35m"

// Helper function untuk menggantikan ends_with (kompatibel C++17 ke bawah)
bool hasEnding(const std::string& fullString, const std::string& ending) {
    if (fullString.length() >= ending.length()) {
        return (0 == fullString.compare(fullString.length() - ending.length(), ending.length(), ending));
    }
    return false;
}

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
    std::cout << COLOR_BOLD << "USAGE:" << COLOR_RESET << "\n"
              << "  " << COLOR_GREEN << progName << COLOR_RESET << " <files...> " << COLOR_YELLOW << "-o" << COLOR_RESET << " <output>       " << COLOR_MAGENTA << "# Compile & Link (.cro/.o to Executable)" << COLOR_RESET << "\n"
              << "  " << COLOR_GREEN << progName << COLOR_RESET << " " << COLOR_YELLOW << "-cr" << COLOR_RESET << " <files...> " << COLOR_YELLOW << "-o" << COLOR_RESET << " <output.o>   " << COLOR_MAGENTA << "# Compile to Object File(s) only" << COLOR_RESET << "\n"
              << "  " << COLOR_GREEN << progName << COLOR_RESET << " <file.cro> " << COLOR_YELLOW << "-s" << COLOR_RESET << " <output.s>       " << COLOR_MAGENTA << "# Generate Assembly (.s) only" << COLOR_RESET << "\n\n"
              << COLOR_BOLD << "EXAMPLES:" << COLOR_RESET << "\n"
              << "  " << progName << " main.cro add.cro -o main\n"
              << "  " << progName << " main.o utils.o -o main\n"
              << "  " << progName << " -cr main.cro add.cro -o main.o\n"
              << "  " << progName << " main.cro -s main.s\n"
              << COLOR_RESET << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    bool compileOnly = false;
    bool assemblyOnly = false;
    int flagIndex = -1;

    // Cek apakah argumen pertama adalah flag khusus seperti -cr
    int startArg = 1;
    if (std::string(argv[1]) == "-cr") {
        compileOnly = true;
        startArg = 2;
    }

    // Cari posisi flag output (-o atau -s)
    for (int i = startArg; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "-s") {
            flagIndex = i;
            if (arg == "-s") assemblyOnly = true;
            break;
        }
    }

    if (flagIndex == -1 || flagIndex == startArg) {
        std::cerr << COLOR_RED << "Error: Missing input files or valid output flag (-o / -s).\n" << COLOR_RESET;
        printUsage(argv[0]);
        return 1;
    }

    // Kumpulkan semua file input di antara startArg dan flagIndex
    std::vector<std::string> inputFiles;
    for (int i = startArg; i < flagIndex; ++i) {
        inputFiles.push_back(argv[i]);
    }

    // Ambil nama file output sesudah flag
    if (flagIndex + 1 >= argc) {
        std::cerr << COLOR_RED << "Error: Missing output filename after flag.\n" << COLOR_RESET;
        return 1;
    }
    std::string outputFilename = argv[flagIndex + 1];

    // Validasi aturan logika flag -s (hanya untuk 1 file .cro)
    if (assemblyOnly) {
        if (inputFiles.size() > 1) {
            std::cerr << COLOR_RED << "Error: Flag -s only supports a single input file.\n" << COLOR_RESET;
            return 1;
        }
        if (!hasEnding(inputFiles[0], ".cro")) {
            std::cerr << COLOR_RED << "Error: Input file for -s must be a .cro file.\n" << COLOR_RESET;
            return 1;
        }
    }

    std::vector<std::string> objectFilesToLink;

    // Proses setiap file input
    for (std::size_t i = 0; i < inputFiles.size(); ++i) {
        const auto& file = inputFiles[i];

        if (hasEnding(file, ".o")) {
            // Jika filenya sudah berupa binary object (.o), langsung tampung untuk dilink
            objectFilesToLink.push_back(file);
            std::cout << COLOR_GREEN << "[PASS] " << COLOR_RESET << "Using precompiled object: " << file << "\n";
        } 
        else if (hasEnding(file, ".cro")) {
            std::cout << COLOR_YELLOW << "[BUILD] " << COLOR_RESET << "Processing CRU source: " << file << "\n";
            
            // 1. Baca source code .cro
            std::string source;
            try {
                source = readFile(file);
            } catch (const std::exception& e) {
                std::cerr << COLOR_RED << e.what() << COLOR_RESET << '\n';
                return 1;
            }

            // 2. Lexer, Parser, & Semantic Analysis
            Lexer lexer(source, file);
            cru::Parser parser(lexer.tokenize());
            cru::sem::SemanticAnalyzer semantic;
            auto module = parser.parseModule();
            
            if (!semantic.analyze(module)) {
                std::cerr << COLOR_RED << "Semantic analysis failed in " << file << ":" << COLOR_RESET << "\n";
                for (const auto& msg : semantic.diagnostics()) {
                    std::cerr << "  " << msg.location.file << ":"
                              << msg.location.line << ":"
                              << msg.location.column << ": "
                              << COLOR_RED << msg.message << COLOR_RESET << '\n';
                }
                return 1;
            }

            // 3. Codegen (Arch64 Assembly)
            std::string assembly = cru::codegen::codegenArch(std::move(module));

            // Jika mode -s (Save Assembly)
            if (assemblyOnly) {
                std::ofstream outFile(outputFilename);
                if (!outFile) {
                    std::cerr << COLOR_RED << "Error: Cannot write to assembly file " << outputFilename << COLOR_RESET << '\n';
                    return 1;
                }
                outFile << assembly;
                std::cout << COLOR_GREEN << "Successfully generated assembly: " << outputFilename << COLOR_RESET << '\n';
                return 0;
            }

            // 4. Buat file assembly sementara (.s) dan rakit ke (.o)
            std::string tempAsmFile = "temp_" + std::to_string(i) + ".s";
            std::string targetObjFile;

            if (compileOnly && inputFiles.size() == 1) {
                targetObjFile = outputFilename;
            } else {
                std::size_t dotIdx = file.rfind('.');
                std::string baseName = (dotIdx == std::string::npos) ? file : file.substr(0, dotIdx);
                targetObjFile = baseName + ".o";
            }

            {
                std::ofstream tempFile(tempAsmFile);
                tempFile << assembly;
            }

            std::string assembleCmd = "as " + tempAsmFile + " -o " + targetObjFile;
            int assembleResult = std::system(assembleCmd.c_str());
            std::remove(tempAsmFile.c_str());

            if (assembleResult != 0) {
                std::cerr << COLOR_RED << "Error: Assembly failed for " << file << COLOR_RESET << '\n';
                return 1;
            }

            std::cout << COLOR_GREEN << "  -> Compiled to " << targetObjFile << COLOR_RESET << "\n";
            objectFilesToLink.push_back(targetObjFile);
        } 
        else {
            std::cerr << COLOR_RED << "Error: Unknown file extension for '" << file << "'. Expected .cro or .o" << COLOR_RESET << "\n";
            return 1;
        }
    }

    // Jika flag -cr aktif, proses berhenti di sini karena hanya ingin membuat file objek (.o)
    if (compileOnly) {
        std::cout << COLOR_BOLD << COLOR_GREEN << "Successfully compiled all source files into object file(s)!" << COLOR_RESET << "\n";
        return 0;
    }

    // 5. Tahap Linking menggunakan ld untuk menggabungkan seluruh file .o menjadi Executable
    std::cout << COLOR_YELLOW << "[LINK] " << COLOR_RESET << "Linking objects into executable: " << outputFilename << "\n";
    std::string linkCmd = "ld";
    for (const auto& obj : objectFilesToLink) {
        linkCmd += " " + obj;
    }
    linkCmd += " -o " + outputFilename;

    int linkResult = std::system(linkCmd.c_str());

    if (linkResult != 0) {
        std::cerr << COLOR_RED << "Error: Linking failed via ld." << COLOR_RESET << '\n';
        return 1;
    }

    std::cout << COLOR_BOLD << COLOR_GREEN << "SUCCESS! Executable created: " << outputFilename << COLOR_RESET << "\n";
    return 0;
}
