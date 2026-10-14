#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>
#include <string>

// Exercise the native-built IR library, including a negative verifier case.
// This checks IR construction and validation; it does not execute generated IR.
int main() {
    for (unsigned iteration = 0; iteration != 100; ++iteration) {
        llvm::LLVMContext context;
        llvm::Module module("native-aros-ir", context);
        llvm::IRBuilder<> builder(context);
        auto *i64 = builder.getInt64Ty();
        auto *type = llvm::FunctionType::get(i64, {i64, i64}, false);
        auto *function = llvm::Function::Create(
            type, llvm::Function::ExternalLinkage, "native_add", module);
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
        auto *sum = builder.CreateAdd(function->getArg(0), function->getArg(1));
        builder.CreateRet(sum);
        if (llvm::verifyModule(module, &llvm::errs()))
            return 1;

        auto *invalid = llvm::Function::Create(
            llvm::FunctionType::get(i64, false),
            llvm::Function::ExternalLinkage, "invalid_return", module);
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", invalid));
        builder.CreateRet(builder.getInt32(7));
        std::string diagnostic;
        llvm::raw_string_ostream errors(diagnostic);
        if (!llvm::verifyModule(module, &errors) || diagnostic.empty())
            return 2;
        builder.ClearInsertionPoint();
        invalid->eraseFromParent();
        if (llvm::verifyModule(module, &llvm::errs()))
            return 3;

        std::string ir;
        llvm::raw_string_ostream output(ir);
        module.print(output, nullptr);
        if (ir.find("define i64 @native_add") == std::string::npos ||
            ir.find("add i64") == std::string::npos ||
            ir.find("invalid_return") != std::string::npos)
            return 4;
    }
    std::puts("NATIVE-LLVM-IR-PASS modules=100 rejected_invalid=100");
    return 0;
}
