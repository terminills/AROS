#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>

// This checks bitcode serialization and parsing, not machine-code execution.
int main() {
    for (unsigned iteration = 0; iteration != 100; ++iteration) {
        llvm::LLVMContext sourceContext;
        llvm::Module source("native-bitcode", sourceContext);
        source.setSourceFileName("native-bitcode.cpp");
        auto *type = llvm::Type::getInt64Ty(sourceContext);
        new llvm::GlobalVariable(source, type, true,
                                 llvm::GlobalValue::ExternalLinkage,
                                 llvm::ConstantInt::get(type, iteration), "value");
        llvm::SmallVector<char, 0> bytes;
        llvm::raw_svector_ostream output(bytes);
        llvm::WriteBitcodeToFile(source, output);
        llvm::LLVMContext targetContext;
        llvm::StringRef data(bytes.data(), bytes.size());
        auto parsed = llvm::parseBitcodeFile(
            llvm::MemoryBufferRef(data, "roundtrip"), targetContext);
        if (!parsed) {
            llvm::logAllUnhandledErrors(parsed.takeError(), llvm::errs());
            return 1;
        }
        if (llvm::verifyModule(**parsed, &llvm::errs()))
            return 2;
        auto *global = (*parsed)->getNamedGlobal("value");
        auto *value = global && global->hasInitializer()
                          ? llvm::dyn_cast<llvm::ConstantInt>(global->getInitializer())
                          : nullptr;
        if (!value || value->getZExtValue() != iteration || !global->isConstant() ||
            (*parsed)->getSourceFileName() != "native-bitcode.cpp")
            return 3;

        // Damage the signature; parsing must return a consumable Error.
        bytes[0] = 0;
        llvm::LLVMContext invalidContext;
        auto invalid = llvm::parseBitcodeFile(
            llvm::MemoryBufferRef(llvm::StringRef(bytes.data(), bytes.size()),
                                  "invalid-signature"), invalidContext);
        if (invalid)
            return 4;
        llvm::consumeError(invalid.takeError());
    }
    std::puts("NATIVE-LLVM-BITCODE-PASS roundtrips=100 rejected_invalid=100");
    return 0;
}
