// Test: Can we eval C++ code in WASM clang-repl?
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
#include <llvm/Support/TargetSelect.h>
#include <iostream>

int main() {
    std::cout << "=== WASM Clang-REPL Test ===" << std::endl;

    // Initialize LLVM
    std::cout << "Initializing LLVM..." << std::endl;
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();

    // Create interpreter
    std::cout << "Creating interpreter..." << std::endl;
    std::vector<const char*> args = {"clang-repl", "-std=c++17"};

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
        std::cerr << "Failed to create interpreter: " << llvm::toString(Interp.takeError()) << std::endl;
        return 1;
    }

    std::cout << "✓ Interpreter created!" << std::endl;
    std::cout << std::endl;

    // Test 1: Simple variable declaration
    std::cout << "Test 1: int x = 42;" << std::endl;
    auto R1 = (*Interp)->ParseAndExecute("int x = 42;");
    if (R1) {
        std::cerr << "✗ FAILED: " << llvm::toString(std::move(R1)) << std::endl;
        return 1;
    }
    std::cout << "✓ PASSED" << std::endl;
    std::cout << std::endl;

    // Test 2: Function definition
    std::cout << "Test 2: Define function add(a, b)" << std::endl;
    auto R2 = (*Interp)->ParseAndExecute(
        "int add(int a, int b) { return a + b; }"
    );
    if (R2) {
        std::cerr << "✗ FAILED: " << llvm::toString(std::move(R2)) << std::endl;
        return 1;
    }
    std::cout << "✓ PASSED" << std::endl;
    std::cout << std::endl;

    // Test 3: Call the function
    std::cout << "Test 3: Call add(10, 20)" << std::endl;
    auto R3 = (*Interp)->ParseAndExecute("add(10, 20);");
    if (R3) {
        std::cerr << "✗ FAILED: " << llvm::toString(std::move(R3)) << std::endl;
        return 1;
    }
    std::cout << "✓ PASSED" << std::endl;
    std::cout << std::endl;

    // Test 4: Expression evaluation
    std::cout << "Test 4: Expression x + 100" << std::endl;
    auto R4 = (*Interp)->ParseAndExecute("x + 100;");
    if (R4) {
        std::cerr << "✗ FAILED: " << llvm::toString(std::move(R4)) << std::endl;
        return 1;
    }
    std::cout << "✓ PASSED" << std::endl;
    std::cout << std::endl;

    std::cout << "==========================" << std::endl;
    std::cout << "ALL TESTS PASSED! 🎉" << std::endl;
    std::cout << "Clang-REPL works in WASM!" << std::endl;

    return 0;
}
