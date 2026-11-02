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
              << "  " << progName << " <file1.cro> [file2.cro ...] -o <output_bin>        # Compile & link multiple .cro files\n"
              << "  " << progName << " <file.cro> -s <output.s>                       # Save assembly file only (single file)\n"
              << "  " << progName << " <file1.cro> [file2.cro ...] -o -nomain <output_bin> # Raw ld mode without entry adjustments\n";
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    // Cari posisi argumen flag (-o atau -s)
    int flagIndex = -1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "-s") {
            flagIndex = i;
            break;
        }
    }

    if (flagIndex == -1 || flagIndex == 1) {
        std::cerr << "Error: Invalid argument structure or missing output flag (-o / -s).\n";
        printUsage(argv[0]);
        return 1;
    }

    // Semua argumen sebelum flag dianggap sebagai file input (.cro)
    std::vector<std::string> inputFiles;
    for (int i = 1; i < flagIndex; ++i) {
        inputFiles.push_back(argv[i]);
    }

    std::string flag = argv[flagIndex];
    bool rawLdMode = false;
    std::string outputFilename;

    if (flag == "-s") {
        if (inputFiles.size() > 1) {
            std::cerr << "Error: Flag -s only supports a single input file.\n";
            return 1;
        }
        if (flagIndex + 1 >= argc) {
            std::cerr << "Error: Missing output assembly filename after -s\n";
            return 1;
        }
        outputFilename = argv[flagIndex + 1];
    } 
    else if (flag == "-o") {
        // Cek apakah ada sub-flag -nomain setelah -o
        if (flagIndex + 1 < argc && std::string(argv[flagIndex + 1]) == "-nomain") {
            rawLdMode = true;
            if (flagIndex + 2 >= argc) {
                std::cerr << "Error: Missing output filename after -nomain\n";
                return 1;
            }
            outputFilename = argv[flagIndex + 2];
        } else {
            if (flagIndex + 1 >= argc) {
                std::cerr << "Error: Missing output binary filename after -o\n";
                return 1;
            }
            outputFilename = argv[flagIndex + 1];
        }
    }

    std::vector<std::string> generatedObjFiles;

    // 2. Loop kompilasi setiap file input .cro
    for (std::size_t i = 0; i < inputFiles.size(); ++i) {
        const auto& inputFile = inputFiles[i];
        std::string source;
        try {
            source = readFile(inputFile);
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            return 1;
        }

        Lexer lexer(source, inputFile);
        cru::Parser parser(lexer.tokenize());
        cru::sem::SemanticAnalyzer semantic;
        auto module = parser.parseModule();
        
        if (!semantic.analyze(module)) {
            std::cerr << "Semantic analysis failed in " << inputFile << ":\n";
            for (const auto& msg : semantic.diagnostics()) {
                std::cerr << msg.location.file << ":"
                          << msg.location.line << ":"
                          << msg.location.column << ": "
                          << msg.message << '\n';
            }
            return 1;
        }

        std::string assembly = cru::codegen::codegenArch(std::move(module));

        // Jika mode -s (save assembly), cukup simpan ke file output dan selesai
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

        // Simpan sementara ke file assembly (.s) untuk masing-masing file
        std::string tempAsmFile = "temp_output_" + std::to_string(i) + ".s";
        std::string tempObjFile = "temp_output_" + std::to_string(i) + ".o";

        {
            std::ofstream tempFile(tempAsmFile);
            tempFile << assembly;
        }

        // Assembler: as file.s -o file.o
        std::string assembleCmd = "gcc " + tempAsmFile + " -o " + tempObjFile;
        int assembleResult = std::system(assembleCmd.c_str());
        
        // Hapus file assembly sementara
        std::remove(tempAsmFile.c_str());

        if (assembleResult != 0) {
            std::cerr << "Error: Assembly failed for " << inputFile << '\n';
            // Bersihkan file .o yang sudah terlanjur dibuat
            for (const auto& obj : generatedObjFiles) {
                std::remove(obj.c_str());
            }
            return 1;
        }

        generatedObjFiles.push_back(tempObjFile);
    }

    // 3. Tahap Linking menggunakan ld untuk menggabungkan semua file .o
    std::string linkCmd = "ld";
    for (const auto& obj : generatedObjFiles) {
        linkCmd += " " + obj;
    }
    linkCmd += " -o " + outputFilename;

    int linkResult = std::system(linkCmd.c_str());

    // Hapus file-file objek sementara setelah di-link
    for (const auto& obj : generatedObjFiles) {
        std::remove(obj.c_str());
    }

    if (linkResult != 0) {
        std::cerr << "Error: Linking failed via ld.\n";
        return 1;
    }

    std::cout << "Successfully compiled and linked multiple files into: " << outputFilename << '\n';
    return 0;
}
