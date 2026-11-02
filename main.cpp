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
              << "  " << progName << " <file.cro> -o -nomain <output_bin>  # Compile & link raw via ld\n"
              << "  " << progName << " <file.cro> -o -l <output_bin> a b c # Compile & link raw with libraries via ld\n";
}


int main(int argc, char* argv[]) {
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    std::string inputFile = argv[1];
    std::string flag = argv[2];
    
    bool rawLdMode = false;
    bool linkWithLibs = false;
    std::string outputFilename;
    std::vector<std::string> libraries;

    if (flag == "-s") {
        outputFilename = argv[3];
    } else if (flag == "-o") {
        std::string subFlag = argv[3];
        if (subFlag == "-nomain") {
            if (argc < 5) {
                std::cerr << "Error: Missing output filename after -nomain\n";
                return 1;
            }
            rawLdMode = true;
            outputFilename = argv[4];
        } 
        else if (subFlag == "-l") {
            // Format: ./cro main.cro -o -l <output_bin> lib1 lib2 ...
            if (argc < 6) {
                std::cerr << "Error: Missing output filename or libraries after -l\n";
                return 1;
            }
            rawLdMode = true;
            linkWithLibs = true;
            outputFilename = argv[4];
            
            // Ambil argumen sisa sebagai nama library (-la, -lb, dst.)
            for (int i = 5; i < argc; ++i) {
                libraries.push_back(argv[i]);
            }
        } 
        else {
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
    
    if (!semantic.analyze(module)) {
        for (const auto& msg : semantic.diagnostics()) {
            std::cerr << msg.location.file << ":"
                      << msg.location.line << ":"
                      << msg.location.column << ": "
                      << msg.message << '\n';
        }
        return 0;
    }

    std::string assembly = cru::codegen::codegenArch(std::move(module));

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

    // Simpan sementara ke file assembly (.s)
    std::string tempAssemblyFile = "temp_output_cru.s";
    {
        std::ofstream tempFile(tempAssemblyFile);
        tempFile << assembly;
    }

    std::string compileCommand;
    std::string tempObjFile = "temp_output_cru.o";

    if (linkWithLibs) {
        // Gabungkan library menjadi format flag ld (misal: -la -lb -lc)
        std::string extraFiles = "";
        for (const auto& lib : libraries) {
            extraFiles += " " + lib;
        }

        // Jalankan assembler (as) lalu linker (ld) dengan menyertakan library eksternal
        compileCommand = "as " + tempAssemblyFile + " -o " + tempObjFile + 
                         " && ld " + tempObjFile + extraFiles + " -o " + outputFilename + 
                         " && rm " + tempObjFile;
    } 
    else if (rawLdMode) {
        compileCommand = "as " + tempAssemblyFile + " -o " + tempObjFile + 
                         " && ld " + tempObjFile + " -o " + outputFilename + 
                         " && rm " + tempObjFile;
    } 
    else {
        // Catatan: Pada kode asli Anda bagian else menggunakan 'as' (mungkin maksudnya 'gcc' untuk mode standar)
        compileCommand = "as " + tempAssemblyFile + " -o " + outputFilename;
    }

    // Eksekusi perintah sistem
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
