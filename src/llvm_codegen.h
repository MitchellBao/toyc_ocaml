#pragma once

#include "ast.h"

#include <iosfwd>

namespace toyc {

struct LLVMCodegenOptions {
    bool optimize = false;
};

class LLVMRiscVCodeGenerator {
public:
    explicit LLVMRiscVCodeGenerator(LLVMCodegenOptions options = {});

    void generate(const Program& program, std::ostream& out);

private:
    LLVMCodegenOptions options_;
};

} // namespace toyc
