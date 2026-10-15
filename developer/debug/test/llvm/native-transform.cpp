#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>
#include <cstdio>

int main() {
    for (unsigned iteration = 0; iteration != 100; ++iteration) {
        llvm::LLVMContext context;
        llvm::SMDiagnostic diagnostic;
        auto original = llvm::parseAssemblyString(
            "define i32 @answer() { entry: %a = add i32 40, 2 "
            "%b = mul i32 %a, 1 ret i32 %b }", diagnostic, context);
        if (!original || llvm::verifyModule(*original, &llvm::errs()))
            return 1;
        auto copy = llvm::CloneModule(*original);
        auto *function = copy->getFunction("answer");
        auto &block = function->getEntryBlock();
        if (block.size() != 3 || !llvm::SimplifyInstructionsInBlock(&block))
            return 2;
        auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
        auto *constant = ret ? llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue()) : nullptr;
        if (block.size() != 1 || !constant || constant->getZExtValue() != 42 ||
            llvm::verifyModule(*copy, &llvm::errs()))
            return 3;
        if (original->getFunction("answer")->getEntryBlock().size() != 3)
            return 4;
        if (llvm::SimplifyInstructionsInBlock(&block))
            return 5;
    }
    std::puts("NATIVE-LLVM-TRANSFORM-PASS clones=100 simplified=100 originals_preserved=100");
    return 0;
}
