// Minimal test to verify Clang Interpreter works in WASM
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <llvm/Support/TargetSelect.h>
#include <iostream>

int main() {
    // Initialize LLVM targets for WebAssembly
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();

    std::cout << "Creating Clang Interpreter..." << std::endl;

    // Create interpreter arguments
    std::vector<const char*> args = {"clang-repl"};

    // Use the LLVM 22 API
    clang::IncrementalCompilerBuilder Builder;
    Builder.SetCompilerArgs(args);
    Builder.SetTargetTriple("wasm32-unknown-emscripten");

    auto CI = Builder.CreateCpp();
    if (!CI) {
        std::cerr << "Failed to create compiler instance" << std::endl;
        llvm::consumeError(CI.takeError());
        return 1;
    }

    auto Interp = clang::Interpreter::create(std::move(*CI));
    if (!Interp) {
        std::cerr << "Failed to create interpreter: " << toString(Interp.takeError()) << std::endl;
        return 1;
    }

    std::cout << "Interpreter created successfully!" << std::endl;

    // Try to execute a simple expression
    auto R = (*Interp)->ParseAndExecute("int x = 42;");
    if (R) {
        std::cerr << "Failed to execute: " << toString(std::move(R)) << std::endl;
        return 1;
    }

    std::cout << "Executed 'int x = 42;' successfully!" << std::endl;

    // Execute another expression
    R = (*Interp)->ParseAndExecute("x + 10");
    if (R) {
        std::cerr << "Failed to execute: " << toString(std::move(R)) << std::endl;
        return 1;
    }

    std::cout << "Clang Interpreter WASM test passed!" << std::endl;
    return 0;
}
