#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar.h>
#include <cstdio>

int main() {
    for (unsigned iteration = 0; iteration != 100; ++iteration) {
        llvm::LLVMContext context;
        llvm::SMDiagnostic diagnostic;
        auto module = llvm::parseAssemblyString(
            "define i32 @answer() { entry: "
            "%slot = alloca i32 store i32 42, ptr %slot "
            "%value = load i32, ptr %slot ret i32 %value }",
            diagnostic, context);
        if (!module || llvm::verifyModule(*module, &llvm::errs()))
            return 1;
        auto *function = module->getFunction("answer");
        if (function->getEntryBlock().size() != 4)
            return 2;
        llvm::legacy::PassManager passes;
        passes.add(llvm::createSROAPass());
        if (!passes.run(*module) || llvm::verifyModule(*module, &llvm::errs()))
            return 3;
        auto &block = function->getEntryBlock();
        auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
        auto *value = ret ? llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue()) : nullptr;
        if (block.size() != 1 || !value || value->getZExtValue() != 42)
            return 4;
    }
    std::puts("NATIVE-LLVM-SCALAR-PASS sroa=100 constant_returns=100 verified=100");
    return 0;
}
