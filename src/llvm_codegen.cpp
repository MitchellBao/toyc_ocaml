#include "llvm_codegen.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toyc {
namespace {

struct FunctionInfo {
    Type returnType = Type::Void;
    std::vector<std::string> params;
};

struct SymbolRef {
    std::string ptr;
};

struct LoopTarget {
    std::string breakLabel;
    std::string continueLabel;
};

std::string typeName(Type type)
{
    return type == Type::Int ? "i32" : "void";
}

std::string quotePath(const std::filesystem::path& path)
{
    std::string text = path.string();
    std::string quoted = "\"";
    for (const char ch : text) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += '"';
    return quoted;
}

std::filesystem::path findClang()
{
    if (const char* env = std::getenv("TOYC_CLANG")) {
        if (*env != '\0') {
            return std::filesystem::path(env);
        }
    }

#ifdef _WIN32
    const std::filesystem::path bundled = "C:/Program Files/LLVM/bin/clang.exe";
    if (std::filesystem::exists(bundled)) {
        return bundled;
    }
    return "clang.exe";
#else
    return "clang";
#endif
}

std::filesystem::path tempPath(const std::string& suffix)
{
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "toyc_llvm_" << ticks << '_' << reinterpret_cast<std::uintptr_t>(&name) << suffix;
    return std::filesystem::temp_directory_path() / name.str();
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to read file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to write file: " + path.string());
    }
    out << text;
}

class TempFiles {
public:
    TempFiles()
        : ir(tempPath(".ll")), assembly(tempPath(".s")), errors(tempPath(".err"))
    {
    }

    ~TempFiles()
    {
        std::error_code ignored;
        std::filesystem::remove(ir, ignored);
        std::filesystem::remove(assembly, ignored);
        std::filesystem::remove(errors, ignored);
    }

    std::filesystem::path ir;
    std::filesystem::path assembly;
    std::filesystem::path errors;
};

class IRGenerator {
public:
    explicit IRGenerator(bool optimize)
        : optimize_(optimize)
    {
    }

    std::string generate(const Program& program)
    {
        collectFunctions(program);

        out_ << "target triple = \"riscv32-unknown-elf\"\n\n";

        pushScope();
        emitGlobals(program);
        if (!globalScope_.empty()) {
            out_ << '\n';
        }

        for (const auto& item : program.items) {
            if (const auto* topFunc = dynamic_cast<const TopFunc*>(item.get())) {
                emitFunction(*topFunc->func);
                out_ << '\n';
            }
        }
        popScope();

        return out_.str();
    }

private:
    void collectFunctions(const Program& program)
    {
        for (const auto& item : program.items) {
            if (const auto* topFunc = dynamic_cast<const TopFunc*>(item.get())) {
                FunctionInfo info;
                info.returnType = topFunc->func->returnType;
                for (const auto& param : topFunc->func->params) {
                    info.params.push_back(param.name);
                }
                functions_.emplace(topFunc->func->name, std::move(info));
            }
        }
    }

    void emitGlobals(const Program& program)
    {
        for (const auto& item : program.items) {
            const auto* topDecl = dynamic_cast<const TopDecl*>(item.get());
            if (topDecl == nullptr) {
                continue;
            }

            const Decl& decl = *topDecl->decl;
            const std::int32_t init = evalConst(*decl.init);
            const char* linkage = decl.isConst ? "constant" : "global";
            out_ << '@' << decl.name << " = " << linkage << " i32 " << init << ", align 4\n";
            globalScope_.emplace(decl.name, SymbolRef{"@" + decl.name});
            globalInitValues_.emplace(decl.name, init);
        }
        scopes_.back().insert(globalScope_.begin(), globalScope_.end());
    }

    void emitFunction(const FuncDef& func)
    {
        currentFunctionReturnType_ = func.returnType;
        entryAllocs_.str("");
        entryAllocs_.clear();
        body_.str("");
        body_.clear();
        tempIndex_ = 0;
        labelIndex_ = 0;
        terminated_ = false;

        pushScope();

        entryAllocs_ << "entry:\n";
        currentLabel_ = "entry";
        for (std::size_t i = 0; i < func.params.size(); ++i) {
            const auto& param = func.params[i];
            const std::string ptr = createAlloca();
            scopes_.back().emplace(param.name, SymbolRef{ptr});
            body_ << "  store i32 %p" << i << ", ptr " << ptr << ", align 4\n";
        }

        emitBlock(*func.body, false);
        if (!terminated_) {
            if (func.returnType == Type::Void) {
                emit("ret void");
            } else {
                emit("ret i32 0");
            }
        }

        out_ << "define " << typeName(func.returnType) << " @" << func.name << '(';
        for (std::size_t i = 0; i < func.params.size(); ++i) {
            if (i != 0) {
                out_ << ", ";
            }
            out_ << "i32 %p" << i;
        }
        out_ << ") {\n";
        out_ << entryAllocs_.str();
        out_ << body_.str();
        out_ << "}\n";

        popScope();
    }

    void emitBlock(const BlockStmt& block, bool createsScope)
    {
        if (createsScope) {
            pushScope();
        }

        for (const auto& stmt : block.statements) {
            if (terminated_) {
                startBlock(newLabel("unreachable"));
            }
            emitStmt(*stmt);
        }

        if (createsScope) {
            popScope();
        }
    }

    void emitStmt(const Stmt& stmt)
    {
        if (dynamic_cast<const EmptyStmt*>(&stmt) != nullptr) {
            return;
        }
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
            emitExprForSideEffect(*exprStmt->expr);
            return;
        }
        if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
            const std::string value = emitExpr(*assign->value);
            emit("store i32 " + value + ", ptr " + lookup(assign->name).ptr + ", align 4");
            return;
        }
        if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
            emitDecl(*declStmt->decl);
            return;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
            emitBlock(*block, true);
            return;
        }
        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            emitIf(*ifStmt);
            return;
        }
        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            emitWhile(*whileStmt);
            return;
        }
        if (dynamic_cast<const BreakStmt*>(&stmt) != nullptr) {
            if (loopTargets_.empty()) {
                throw std::runtime_error("internal error: break without loop target");
            }
            emit("br label %" + loopTargets_.back().breakLabel);
            terminated_ = true;
            return;
        }
        if (dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
            if (loopTargets_.empty()) {
                throw std::runtime_error("internal error: continue without loop target");
            }
            emit("br label %" + loopTargets_.back().continueLabel);
            terminated_ = true;
            return;
        }
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            if (ret->value == nullptr) {
                emit("ret void");
            } else {
                emit("ret i32 " + emitExpr(*ret->value));
            }
            terminated_ = true;
            return;
        }
        throw std::runtime_error("internal error: unknown statement");
    }

    void emitDecl(const Decl& decl)
    {
        const std::string ptr = createAlloca();
        scopes_.back().emplace(decl.name, SymbolRef{ptr});
        const std::string init = emitExpr(*decl.init);
        emit("store i32 " + init + ", ptr " + ptr + ", align 4");
    }

    void emitIf(const IfStmt& stmt)
    {
        const std::string thenLabel = newLabel("if.then");
        const std::string elseLabel = newLabel("if.else");
        const std::string endLabel = newLabel("if.end");
        const std::string cond = emitBool(*stmt.cond);

        emit("br i1 " + cond + ", label %" + thenLabel + ", label %" + (stmt.elseBranch ? elseLabel : endLabel));
        terminated_ = true;

        startBlock(thenLabel);
        emitStmt(*stmt.thenBranch);
        if (!terminated_) {
            emit("br label %" + endLabel);
            terminated_ = true;
        }

        if (stmt.elseBranch) {
            startBlock(elseLabel);
            emitStmt(*stmt.elseBranch);
            if (!terminated_) {
                emit("br label %" + endLabel);
                terminated_ = true;
            }
        }

        startBlock(endLabel);
    }

    void emitWhile(const WhileStmt& stmt)
    {
        const std::string condLabel = newLabel("while.cond");
        const std::string bodyLabel = newLabel("while.body");
        const std::string endLabel = newLabel("while.end");

        emit("br label %" + condLabel);
        terminated_ = true;

        startBlock(condLabel);
        const std::string cond = emitBool(*stmt.cond);
        emit("br i1 " + cond + ", label %" + bodyLabel + ", label %" + endLabel);
        terminated_ = true;

        startBlock(bodyLabel);
        loopTargets_.push_back(LoopTarget{endLabel, condLabel});
        emitStmt(*stmt.body);
        loopTargets_.pop_back();
        if (!terminated_) {
            emit("br label %" + condLabel);
            terminated_ = true;
        }

        startBlock(endLabel);
    }

    void emitExprForSideEffect(const Expr& expr)
    {
        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            emitCall(*call);
            return;
        }
        (void)emitExpr(expr);
    }

    std::string emitExpr(const Expr& expr)
    {
        if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
            return std::to_string(intExpr->value);
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            const std::string tmp = newTemp();
            emit(tmp + " = load i32, ptr " + lookup(name->name).ptr + ", align 4");
            return tmp;
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            return emitCall(*call);
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            const std::string operand = emitExpr(*unary->operand);
            switch (unary->op) {
            case UnaryOp::Plus:
                return operand;
            case UnaryOp::Minus: {
                const std::string tmp = newTemp();
                emit(tmp + " = sub i32 0, " + operand);
                return tmp;
            }
            case UnaryOp::Not: {
                const std::string cmp = newTemp();
                emit(cmp + " = icmp eq i32 " + operand + ", 0");
                const std::string tmp = newTemp();
                emit(tmp + " = zext i1 " + cmp + " to i32");
                return tmp;
            }
            }
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            if (binary->op == BinaryOp::LogicalAnd || binary->op == BinaryOp::LogicalOr) {
                const std::string value = emitShortCircuit(*binary);
                const std::string tmp = newTemp();
                emit(tmp + " = zext i1 " + value + " to i32");
                return tmp;
            }

            const std::string lhs = emitExpr(*binary->lhs);
            const std::string rhs = emitExpr(*binary->rhs);
            const std::string tmp = newTemp();
            switch (binary->op) {
            case BinaryOp::Equal:
                emit(tmp + " = icmp eq i32 " + lhs + ", " + rhs);
                return zextI1(tmp);
            case BinaryOp::NotEqual:
                emit(tmp + " = icmp ne i32 " + lhs + ", " + rhs);
                return zextI1(tmp);
            case BinaryOp::Less:
                emit(tmp + " = icmp slt i32 " + lhs + ", " + rhs);
                return zextI1(tmp);
            case BinaryOp::LessEqual:
                emit(tmp + " = icmp sle i32 " + lhs + ", " + rhs);
                return zextI1(tmp);
            case BinaryOp::Greater:
                emit(tmp + " = icmp sgt i32 " + lhs + ", " + rhs);
                return zextI1(tmp);
            case BinaryOp::GreaterEqual:
                emit(tmp + " = icmp sge i32 " + lhs + ", " + rhs);
                return zextI1(tmp);
            case BinaryOp::Add:
                emit(tmp + " = add i32 " + lhs + ", " + rhs);
                return tmp;
            case BinaryOp::Sub:
                emit(tmp + " = sub i32 " + lhs + ", " + rhs);
                return tmp;
            case BinaryOp::Mul:
                emit(tmp + " = mul i32 " + lhs + ", " + rhs);
                return tmp;
            case BinaryOp::Div:
                emit(tmp + " = sdiv i32 " + lhs + ", " + rhs);
                return tmp;
            case BinaryOp::Mod:
                emit(tmp + " = srem i32 " + lhs + ", " + rhs);
                return tmp;
            case BinaryOp::LogicalAnd:
            case BinaryOp::LogicalOr:
                break;
            }
        }
        throw std::runtime_error("internal error: unknown expression");
    }

    std::string emitCall(const CallExpr& call)
    {
        const auto found = functions_.find(call.callee);
        if (found == functions_.end()) {
            throw std::runtime_error("internal error: unknown function: " + call.callee);
        }

        std::vector<std::string> args;
        args.reserve(call.args.size());
        for (const auto& arg : call.args) {
            args.push_back(emitExpr(*arg));
        }

        std::ostringstream line;
        std::string result;
        if (found->second.returnType == Type::Int) {
            result = newTemp();
            line << result << " = ";
        }
        line << "call " << typeName(found->second.returnType) << " @" << call.callee << '(';
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i != 0) {
                line << ", ";
            }
            line << "i32 " << args[i];
        }
        line << ')';
        emit(line.str());
        return result;
    }

    std::string emitBool(const Expr& expr)
    {
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            if (binary->op == BinaryOp::LogicalAnd || binary->op == BinaryOp::LogicalOr) {
                return emitShortCircuit(*binary);
            }
        }
        return emitBoolValue(emitExpr(expr));
    }

    std::string emitBoolValue(const std::string& value)
    {
        const std::string tmp = newTemp();
        emit(tmp + " = icmp ne i32 " + value + ", 0");
        return tmp;
    }

    std::string emitShortCircuit(const BinaryExpr& expr)
    {
        const bool isAnd = expr.op == BinaryOp::LogicalAnd;
        const std::string rhsLabel = newLabel(isAnd ? "land.rhs" : "lor.rhs");
        const std::string shortLabel = newLabel(isAnd ? "land.false" : "lor.true");
        const std::string endLabel = newLabel(isAnd ? "land.end" : "lor.end");

        const std::string lhs = emitBool(*expr.lhs);
        emit("br i1 " + lhs + ", label %" + (isAnd ? rhsLabel : shortLabel) + ", label %" + (isAnd ? shortLabel : rhsLabel));
        terminated_ = true;

        startBlock(rhsLabel);
        const std::string rhs = emitBool(*expr.rhs);
        const std::string rhsIncoming = currentLabel_;
        emit("br label %" + endLabel);
        terminated_ = true;

        startBlock(shortLabel);
        const std::string shortIncoming = currentLabel_;
        emit("br label %" + endLabel);
        terminated_ = true;

        startBlock(endLabel);
        const std::string phi = newTemp();
        body_ << "  " << phi << " = phi i1 [ " << (isAnd ? "false" : "true") << ", %" << shortIncoming
              << " ], [ " << rhs << ", %" << rhsIncoming << " ]\n";
        return phi;
    }

    std::string zextI1(const std::string& value)
    {
        const std::string tmp = newTemp();
        emit(tmp + " = zext i1 " + value + " to i32");
        return tmp;
    }

    SymbolRef lookup(const std::string& name) const
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return found->second;
            }
        }
        throw std::runtime_error("internal error: unknown symbol: " + name);
    }

    std::int32_t evalConst(const Expr& expr) const
    {
        if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
            return intExpr->value;
        }
        if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
            const auto found = globalInitValues_.find(name->name);
            if (found != globalInitValues_.end()) {
                return found->second;
            }
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
            const std::int32_t value = evalConst(*unary->operand);
            switch (unary->op) {
            case UnaryOp::Plus:
                return value;
            case UnaryOp::Minus:
                return -value;
            case UnaryOp::Not:
                return value == 0 ? 1 : 0;
            }
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
            const std::int32_t lhs = evalConst(*binary->lhs);
            const std::int32_t rhs = evalConst(*binary->rhs);
            switch (binary->op) {
            case BinaryOp::LogicalOr:
                return (lhs != 0 || rhs != 0) ? 1 : 0;
            case BinaryOp::LogicalAnd:
                return (lhs != 0 && rhs != 0) ? 1 : 0;
            case BinaryOp::Equal:
                return lhs == rhs ? 1 : 0;
            case BinaryOp::NotEqual:
                return lhs != rhs ? 1 : 0;
            case BinaryOp::Less:
                return lhs < rhs ? 1 : 0;
            case BinaryOp::LessEqual:
                return lhs <= rhs ? 1 : 0;
            case BinaryOp::Greater:
                return lhs > rhs ? 1 : 0;
            case BinaryOp::GreaterEqual:
                return lhs >= rhs ? 1 : 0;
            case BinaryOp::Add:
                return lhs + rhs;
            case BinaryOp::Sub:
                return lhs - rhs;
            case BinaryOp::Mul:
                return lhs * rhs;
            case BinaryOp::Div:
                return lhs / rhs;
            case BinaryOp::Mod:
                return lhs % rhs;
            }
        }
        throw std::runtime_error("internal error: non-constant global initializer");
    }

    void pushScope()
    {
        scopes_.emplace_back();
    }

    void popScope()
    {
        scopes_.pop_back();
    }

    std::string createAlloca()
    {
        const std::string ptr = "%v" + std::to_string(allocaIndex_++);
        entryAllocs_ << "  " << ptr << " = alloca i32, align 4\n";
        return ptr;
    }

    std::string newTemp()
    {
        return "%t" + std::to_string(tempIndex_++);
    }

    std::string newLabel(const std::string& prefix)
    {
        return prefix + "." + std::to_string(labelIndex_++);
    }

    void startBlock(const std::string& label)
    {
        body_ << label << ":\n";
        currentLabel_ = label;
        terminated_ = false;
    }

    void emit(const std::string& text)
    {
        body_ << "  " << text << '\n';
    }

    bool optimize_ = false;
    std::ostringstream out_;
    std::ostringstream entryAllocs_;
    std::ostringstream body_;
    std::unordered_map<std::string, FunctionInfo> functions_;
    std::unordered_map<std::string, SymbolRef> globalScope_;
    std::unordered_map<std::string, std::int32_t> globalInitValues_;
    std::vector<std::unordered_map<std::string, SymbolRef>> scopes_;
    std::vector<LoopTarget> loopTargets_;
    Type currentFunctionReturnType_ = Type::Void;
    int allocaIndex_ = 0;
    int tempIndex_ = 0;
    int labelIndex_ = 0;
    bool terminated_ = false;
    std::string currentLabel_;
};

std::string compileIRToAssembly(const std::string& ir, bool optimize)
{
    TempFiles temp;
    writeFile(temp.ir, ir);

    const std::filesystem::path clang = findClang();
    std::ostringstream command;
#ifdef _WIN32
    command << "cmd /C \"\""
            << clang.string()
            << "\" --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -x ir -S "
            << (optimize ? "-O2" : "-O0")
            << " -o " << quotePath(temp.assembly)
            << ' ' << quotePath(temp.ir)
            << " 2> " << quotePath(temp.errors)
            << '"';
#else
    command << quotePath(clang)
            << " --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -x ir -S "
            << (optimize ? "-O2" : "-O0")
            << " -o " << quotePath(temp.assembly)
            << ' ' << quotePath(temp.ir)
            << " 2> " << quotePath(temp.errors);
#endif

    const int status = std::system(command.str().c_str());
    if (status != 0) {
        std::string errors;
        if (std::filesystem::exists(temp.errors)) {
            errors = readFile(temp.errors);
        }
        throw std::runtime_error("LLVM clang failed while generating RISC-V assembly\n" + errors);
    }

    return readFile(temp.assembly);
}

} // namespace

LLVMRiscVCodeGenerator::LLVMRiscVCodeGenerator(LLVMCodegenOptions options)
    : options_(options)
{
}

void LLVMRiscVCodeGenerator::generate(const Program& program, std::ostream& out)
{
    IRGenerator irgen(options_.optimize);
    const std::string ir = irgen.generate(program);
    out << compileIRToAssembly(ir, options_.optimize);
}

} // namespace toyc
