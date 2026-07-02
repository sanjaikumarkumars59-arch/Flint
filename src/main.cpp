#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/IR/LegacyPassManager.h"

#include <vector>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <cassert>
#include <iostream>
#include <thread>
#include <mutex>
#include <functional>
#include <queue>
#include <future>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cctype>
#include <chrono>
#include <fstream>

// ============================================================================
// PROFILER — nanosecond-precision instrumentation
// ============================================================================

#ifdef FLINTC_PROFILE

class Timer {
    using Clock = std::chrono::high_resolution_clock;
    struct Record { const char* phase; Clock::time_point start; Clock::duration wall; };
    std::vector<Record> records;
    std::vector<size_t> stack;
    Clock::time_point programStart;
public:
    Timer() { programStart = Clock::now(); }
    void begin(const char* phase) {
        records.push_back({phase, Clock::now(), {}});
        stack.push_back(records.size() - 1);
    }
    void end() {
        if (stack.empty()) return;
        auto idx = stack.back(); stack.pop_back();
        records[idx].wall = Clock::now() - records[idx].start;
    }
    void report() {
        auto total = Clock::now() - programStart;
        std::ofstream out("profile_report.json");
        out << "{\n  \"total_ns\": " << total.count() << ",\n  \"phases\": [\n";
        bool first = true;
        for (auto& r : records) {
            if (!first) out << ",\n";
            first = false;
            out << "    {\"phase\":\"" << r.phase << "\",\"wall_ns\":" << r.wall.count() << "}";
        }
        out << "\n  ]\n}\n";
        out.close();
        std::cerr << "\n=== Profile Report ===\n";
        std::cerr << "Total: " << total.count() / 1000000 << " ms\n";
        for (auto& r : records)
            if (r.wall.count() > 0)
                std::cerr << "  " << r.phase << ": " << r.wall.count() / 1000 << " us (" << r.wall.count() << " ns)\n";
        std::cerr << "=====================\n";
    }
};

Timer g_timer;
#define PROFILE_BEGIN(phase) g_timer.begin(phase)
#define PROFILE_END()        g_timer.end()
#define PROFILE_REPORT()     g_timer.report()

#else
#define PROFILE_BEGIN(phase)
#define PROFILE_END()
#define PROFILE_REPORT()
#endif

// ============================================================================
// FAILURE REPORT — detailed nanosecond trace on error
// ============================================================================

struct PhaseRecorder {
    struct Record { const char* name; uint64_t start; uint64_t end; };
    std::vector<Record> records;
    void begin(const char* name) {
        records.push_back({name, nanos(), 0});
    }
    void end() {
        if (!records.empty()) records.back().end = nanos();
    }
    static uint64_t nanos() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }
    void report(const char* context) {
        uint64_t now = nanos();
        std::cerr << "\n=== FAILURE REPORT (" << context << ") ===\n";
        for (auto& r : records) {
            if (r.end == 0) r.end = now;
            uint64_t wall = r.end - r.start;
            std::cerr << "  " << r.name << ": " << wall << " ns (" << (wall / 1000) << " us, " << (wall / 1000000) << " ms)\n";
        }
        std::cerr << "========================================\n";
    }
};

// ============================================================================
// FNV-1A HASH
// ============================================================================

static std::once_flag targetInitFlag;

static void initTargets() {
    std::call_once(targetInitFlag, []() {
        llvm::InitializeAllTargetInfos();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
    });
}

static std::string sysRoot() {
    const char* pref = getenv("PREFIX");
    if (pref) return std::string(pref);
    return "/data/data/com.termux/files/usr";
}

// Reject paths containing characters that are dangerous even when passed
// directly to exec (control chars) — defensive validation.
static bool isSafePath(const std::string& p) {
    if (p.empty()) return false;
    for (unsigned char c : p) {
        if (c == '\0' || c == '\n' || c == '\r') return false;
    }
    return true;
}

// Split a flag string (e.g. from python3-config) on whitespace into argv tokens.
// This does NOT interpret shell metacharacters — tokens are passed literally
// to exec, so injection via `$()`, backticks, `;`, etc. is impossible.
static std::vector<std::string> splitFlags(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Run a program with an explicit argv array via fork + execvp — NO shell is
// involved, so none of the arguments can be interpreted as shell syntax.
// Returns the child's exit status (0 == success), or -1 on spawn failure.
static int runProcess(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child: replace image. execvp searches PATH for argv[0].
        execvp(argv[0], argv.data());
        _exit(127); // exec failed
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static bool spawnLinker(const std::string& objPath, const std::string& outputPath,
                        const std::string& extraFlags = "") {
    if (!isSafePath(objPath) || !isSafePath(outputPath)) {
        std::cerr << "linker: unsafe path rejected\n";
        return false;
    }

    // Build the argv array directly — no shell, no string interpolation.
    std::vector<std::string> args = {"clang", objPath, "runtime.o"};

    // Include pyruntime.o if it exists (Python support) — try current dir.
    struct stat st;
    if (stat("pyruntime.o", &st) == 0) {
        args.push_back("pyruntime.o");
        // Detect python3-config linker flags. The command here is a fixed
        // literal (no user input), so popen is safe; the *output* is tokenized
        // and passed literally to exec, never re-interpreted by a shell.
        std::string pyFlags;
        FILE* pipe = popen("python3-config --ldflags 2>/dev/null", "r");
        if (!pipe) { pipe = popen("python3-config --ldflags --embed 2>/dev/null", "r"); }
        if (pipe) {
            char buf[4096] = {};
            std::string acc;
            while (fgets(buf, sizeof(buf), pipe)) acc += buf;
            pclose(pipe);
            pyFlags = acc;
        }
        for (auto& tok : splitFlags(pyFlags)) args.push_back(tok);
    }

    // User-supplied --link flags: tokenized and passed literally (no shell).
    for (auto& tok : splitFlags(extraFlags)) args.push_back(tok);

    args.push_back("-o");
    args.push_back(outputPath);

    int ret = runProcess(args);
    if (ret != 0) {
        std::cerr << "linker failed (exit=" << ret << ")\n";
        return false;
    }
    return true;
}
static bool emitModuleOutput(llvm::Module* mod, const std::string& outputPath) {
    bool emitObject = outputPath.size() >= 2 &&
                      outputPath.substr(outputPath.size() - 2) == ".o";
    if (!emitObject) {
        std::string irBuf;
        llvm::raw_string_ostream rso(irBuf);
        mod->print(rso, nullptr);
        rso.flush();
        std::error_code ec;
        llvm::raw_fd_ostream outStream(outputPath, ec);
        if (ec) { std::cerr << "cannot open output file: " << ec.message() << "\n"; return false; }
        outStream << irBuf;
        outStream.close();
        return true;
    }

    initTargets();
    auto triple = mod->getTargetTriple();
    std::string error;
    auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) { std::cerr << "target error: " << error << "\n"; return false; }
    auto* tm = target->createTargetMachine(triple, "generic", "", {}, {});
    if (!tm) { std::cerr << "failed to create target machine\n"; return false; }

    llvm::legacy::PassManager pm;
    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec);
    if (ec) { std::cerr << "error: " << ec.message() << "\n"; delete tm; return false; }
    if (tm->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        std::cerr << "target cannot emit object files\n";
        delete tm;
        return false;
    }
    pm.run(*mod);
    dest.close();
    delete tm;
    return true;
}

static uint64_t fnv1a(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= (uint8_t)c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t fnv1a(llvm::StringRef s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= (uint8_t)c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// ============================================================================
// MODULE CACHE — content-addressed LLVM bitcode cache
// ============================================================================

class ModuleCache {
    std::string cacheDir;
public:
    ModuleCache() {
        const char* home = getenv("HOME");
        cacheDir = std::string(home ? home : ".") + "/.cache/flintc";
        mkdir(cacheDir.c_str(), 0755);
    }

    std::string bitcodePath(uint64_t hash) const {
        return cacheDir + "/" + std::to_string(hash) + ".bc";
    }

    std::string binaryPath(uint64_t hash) const {
        return cacheDir + "/" + std::to_string(hash) + ".bin";
    }

    bool has(uint64_t hash) const {
        struct stat st;
        return stat(bitcodePath(hash).c_str(), &st) == 0;
    }

    bool hasBinary(uint64_t hash) const {
        struct stat st;
        return stat(binaryPath(hash).c_str(), &st) == 0;
    }

    void save(llvm::Module* mod, uint64_t hash) {
        auto path = bitcodePath(hash);
        std::error_code ec;
        llvm::raw_fd_ostream out(path, ec);
        if (ec) { std::cerr << "cache write error: " << ec.message() << "\n"; return; }
        llvm::WriteBitcodeToFile(*mod, out);
        out.close();
    }

    void saveBinary(uint64_t hash, const std::string& binPath) {
        auto dest = binaryPath(hash);
        llvm::sys::fs::copy_file(binPath, dest);
    }

    bool loadBinary(uint64_t hash, const std::string& outputPath) {
        auto src = binaryPath(hash);
        std::error_code ec;
        llvm::sys::fs::copy_file(src, outputPath);
        return !ec;
    }

    std::unique_ptr<llvm::Module> load(uint64_t hash, llvm::LLVMContext& ctx) {
        auto path = bitcodePath(hash);
        auto buf = llvm::MemoryBuffer::getFile(path);
        if (!buf) return nullptr;
        auto src = llvm::parseBitcodeFile(buf.get()->getMemBufferRef(), ctx);
        if (!src) {
            llvm::handleAllErrors(src.takeError(), [](const llvm::ErrorInfoBase& e) {
                std::cerr << "cache read error: " << e.message() << "\n";
            });
            return nullptr;
        }
        return std::move(*src);
    }
};

// ============================================================================
// THREAD POOL — simple worker thread pool
// ============================================================================

class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop = false;
    size_t active = 0;
public:
    ThreadPool(size_t n) {
        for (size_t i = 0; i < n; i++)
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        cv.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                        ++active;
                    }
                    task();
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        --active;
                    }
                    cv.notify_all();
                }
            });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        cv.notify_all();
        for (auto& w : workers) w.join();
    }

    void enqueue(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.push(std::move(f));
        }
        cv.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return tasks.empty() && active == 0; });
    }
};

// ============================================================================
// ARENA ALLOCATOR & STRING POOL
// ============================================================================

class ArenaAllocator {
public:
    static constexpr size_t BLOCK_SIZE = 64 * 1024;

    ArenaAllocator() { addBlock(BLOCK_SIZE); }
    ~ArenaAllocator() { for (auto* b : blocks) std::free(b); }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    template<typename T, typename... Args>
    T* make(Args&&... args) {
        void* ptr = alloc(sizeof(T), alignof(T));
        return new (ptr) T(std::forward<Args>(args)...);
    }

    void reset() {
        for (size_t i = 1; i < blocks.size(); i++) std::free(blocks[i]);
        blocks.resize(1);
        cur = blocks[0];
        offset = 0;
    }

private:
    struct BlockHeader { size_t size; };

    static void* addBlock(size_t sz) {
        void* block = std::malloc(sz);
        if (!block) { std::fprintf(stderr, "ArenaAllocator: out of memory\n"); std::abort(); }
        static_cast<BlockHeader*>(block)->size = sz;
        return block;
    }

    void* alloc(size_t sz, size_t align) {
        size_t start = (offset + align - 1) & ~(align - 1);
        if (start + sz > capacity()) {
            size_t newSz = std::max(BLOCK_SIZE, sz + sizeof(BlockHeader) + align);
            blocks.push_back(addBlock(newSz));
            cur = blocks.back();
            offset = 0;
            start = 0;
        }
        void* ptr = static_cast<char*>(cur) + sizeof(BlockHeader) + start;
        offset = start + sz;
        return ptr;
    }

    size_t capacity() const {
        return static_cast<BlockHeader*>(cur)->size - sizeof(BlockHeader);
    }

    std::vector<void*> blocks;
    void* cur = nullptr;
    size_t offset = 0;
};

class StringPool {
public:
    uint32_t intern(const std::string& s) {
        auto it = table.find(s);
        if (it != table.end()) return it->second;
        uint32_t id = nextId++;
        table[s] = id;
        char* copy = arena.make<char>(s.size() + 1);
        std::memcpy(copy, s.c_str(), s.size() + 1);
        strings.push_back(copy);
        return id;
    }

    const char* get(uint32_t id) const {
        return (id < strings.size()) ? strings[id] : "";
    }

    void reset() {
        arena.reset();
        table.clear();
        strings.clear();
        nextId = 0;
    }

private:
    ArenaAllocator arena;
    std::unordered_map<std::string, uint32_t> table;
    std::vector<const char*> strings;
    uint32_t nextId = 0;
};

// ============================================================================
// TYPE
// ============================================================================

enum class TypeKind { I64, Str, Bool, Void, Array, Ref, Struct, Enum, TypeParam };

struct Type {
    TypeKind kind = TypeKind::Void;
    std::shared_ptr<Type> elemType;
    std::string structName;

    bool operator==(const Type& o) const {
        if (kind != o.kind) return false;
        if (kind == TypeKind::Array || kind == TypeKind::Ref) {
            if (!elemType || !o.elemType) return elemType == o.elemType;
            return *elemType == *o.elemType;
        }
        if (kind == TypeKind::Struct) return structName == o.structName;
        if (kind == TypeKind::Enum) return structName == o.structName;
        if (kind == TypeKind::TypeParam) return structName == o.structName;
        return true;
    }
    bool operator!=(const Type& o) const { return !(*this == o); }

    static Type i64()    { Type t; t.kind = TypeKind::I64; return t; }
    static Type str()    { Type t; t.kind = TypeKind::Str; return t; }
    static Type boolean(){ Type t; t.kind = TypeKind::Bool; return t; }
    static Type void_()  { Type t; t.kind = TypeKind::Void; return t; }
    static Type array(Type e)  { Type t; t.kind = TypeKind::Array; t.elemType = std::make_shared<Type>(e); return t; }
    static Type ref(Type e)    { Type t; t.kind = TypeKind::Ref;   t.elemType = std::make_shared<Type>(e); return t; }
    static Type struct_(const std::string& n) { Type t; t.kind = TypeKind::Struct; t.structName = n; return t; }
    static Type enum_(const std::string& n) { Type t; t.kind = TypeKind::Enum; t.structName = n; return t; }
    static Type typeParam(const std::string& n) { Type t; t.kind = TypeKind::TypeParam; t.structName = n; return t; }

    bool isCopyType() const {
        return kind == TypeKind::I64 || kind == TypeKind::Bool || kind == TypeKind::Ref || kind == TypeKind::Struct || kind == TypeKind::Enum;
    }
    bool isCompound() const { return kind == TypeKind::Array || kind == TypeKind::Ref; }
};

struct StructFieldDef {
    std::string name;
    Type type;
};

struct StructDef {
    std::string name;
    std::vector<StructFieldDef> fields;
};

struct EnumVariantDef {
    std::string name;
    std::vector<Type> payloadTypes;
};

struct EnumDef {
    std::string name;
    std::vector<EnumVariantDef> variants;
};

llvm::Type* llvmType(Type t, llvm::LLVMContext& ctx, llvm::Module* mod = nullptr) {
    switch (t.kind) {
        case TypeKind::I64:  return llvm::Type::getInt64Ty(ctx);
        case TypeKind::Bool: return llvm::Type::getInt64Ty(ctx);
        case TypeKind::Str:  return llvm::PointerType::get(ctx, 0);
        case TypeKind::Void: return llvm::Type::getVoidTy(ctx);
        case TypeKind::Ref:  return llvm::PointerType::get(ctx, 0);
        case TypeKind::Array: {
            llvm::Type* elemPtrTy = llvm::PointerType::get(ctx, 0);
            llvm::Type* lenTy = llvm::Type::getInt64Ty(ctx);
            return llvm::StructType::get(ctx, {elemPtrTy, lenTy});
        }
        case TypeKind::Struct:
        case TypeKind::Enum:
            return llvm::StructType::getTypeByName(ctx, t.structName);
        case TypeKind::TypeParam:
            return llvm::Type::getInt64Ty(ctx); // placeholder, substituted before use
    }
    return llvm::Type::getInt64Ty(ctx);
}

// ============================================================================
// TOKEN
// ============================================================================

enum class TokenType {
    END_OF_FILE, NUMBER_LITERAL, STRING_LITERAL, IDENTIFIER,
    ASSIGN, LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET, SEMICOLON, COMMA, COLON, ARROW,
    NEWLINE, UNKNOWN,
    KW_MUT, KW_FN, KW_IF, KW_ELSE, KW_ELIF, KW_WHILE, KW_RETURN, KW_BREAK, KW_I64, KW_STR, KW_BOOL, KW_EXTERN, KW_PYTHON, KW_STRUCT, KW_ENUM, KW_MATCH, KW_IMPORT,
    PLUS, MINUS, STAR, SLASH, ELLIPSIS, AMPERSAND, DOT,
    EQ_EQ, NE, LT, GT, LE, GE, FAT_ARROW, PIPE_PIPE
};

struct SourceLocation {
    int line = 1;
    int col = 1;
};

struct Token {
    TokenType type = TokenType::UNKNOWN;
    std::string lexeme;
    SourceLocation loc;
};

// ============================================================================
// LEXER
// ============================================================================

class Lexer {
public:
    explicit Lexer(llvm::StringRef source) : source(source), pos(0) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        tokens.reserve(source.size() / 5 + 16);
        loc = SourceLocation{1, 1};
        pos = 0;
        while (!isAtEnd()) {
            skipWhitespace();
            if (isAtEnd()) break;
            char c = advance();
            if (c == '\n') continue;
            switch (c) {
                case '(': tokens.push_back(makeToken(TokenType::LPAREN, "(")); break;
                case ')': tokens.push_back(makeToken(TokenType::RPAREN, ")")); break;
                case '{': tokens.push_back(makeToken(TokenType::LBRACE, "{")); break;
                case '}': tokens.push_back(makeToken(TokenType::RBRACE, "}")); break;
                case '[': tokens.push_back(makeToken(TokenType::LBRACKET, "[")); break;
                case ']': tokens.push_back(makeToken(TokenType::RBRACKET, "]")); break;
                case ';': tokens.push_back(makeToken(TokenType::SEMICOLON, ";")); break;
                case ',': tokens.push_back(makeToken(TokenType::COMMA, ",")); break;
                case ':': tokens.push_back(makeToken(TokenType::COLON, ":")); break;
                case '&': tokens.push_back(makeToken(TokenType::AMPERSAND, "&")); break;
                case '+': tokens.push_back(makeToken(TokenType::PLUS, "+")); break;
                case '-':
                    if (peek() == '>') { advance(); tokens.push_back(makeToken(TokenType::ARROW, "->")); }
                    else { tokens.push_back(makeToken(TokenType::MINUS, "-")); }
                    break;
                case '*': tokens.push_back(makeToken(TokenType::STAR, "*")); break;
                case '/': tokens.push_back(makeToken(TokenType::SLASH, "/")); break;
                case '=':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::EQ_EQ, "==")); }
                    else if (peek() == '>') { advance(); tokens.push_back(makeToken(TokenType::FAT_ARROW, "=>")); }
                    else { tokens.push_back(makeToken(TokenType::ASSIGN, "=")); }
                    break;
                case '!':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::NE, "!=")); }
                    else { unexpected(c); }
                    break;
                case '<':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::LE, "<=")); }
                    else { tokens.push_back(makeToken(TokenType::LT, "<")); }
                    break;
                case '>':
                    if (peek() == '=') { advance(); tokens.push_back(makeToken(TokenType::GE, ">=")); }
                    else { tokens.push_back(makeToken(TokenType::GT, ">")); }
                    break;
                case '.':
                    if (peek() == '.' && peek(1) == '.') { advance(); advance(); tokens.push_back(makeToken(TokenType::ELLIPSIS, "...")); }
                    else { tokens.push_back(makeToken(TokenType::DOT, ".")); }
                    break;
                case '|':
                    if (peek() == '|') { advance(); tokens.push_back(makeToken(TokenType::PIPE_PIPE, "||")); }
                    else { unexpected(c); }
                    break;
                case '"': tokens.push_back(readString()); break;
                case '#':
                    while (peek() != '\n' && peek() != '\0') advance();
                    if (peek() == '\n') advance();
                    break;
                default:
                    if (std::isdigit(c)) { pos--; loc.col--; tokens.push_back(readNumber()); }
                    else if (std::isalpha(c) || c == '_') { pos--; loc.col--; tokens.push_back(readIdentifier()); }
                    else { unexpected(c); }
                    break;
            }
        }
        tokens.push_back(makeToken(TokenType::END_OF_FILE, ""));
        return tokens;
    }

private:
    llvm::StringRef source;
    size_t pos;
    SourceLocation loc;

    char peek(size_t ahead = 0) const {
        size_t idx = pos + ahead;
        return idx < source.size() ? source[idx] : '\0';
    }

    char advance() {
        char c = source[pos++];
        if (c == '\n') { loc.line++; loc.col = 1; } else { loc.col++; }
        return c;
    }

    bool isAtEnd() const { return pos >= source.size(); }

    void skipWhitespace() {
        while (!isAtEnd()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') { advance(); }
            else if (c == '/' && peek(1) == '/') { while (!isAtEnd() && peek() != '\n') advance(); }
            else { break; }
        }
    }

    Token makeToken(TokenType type, const std::string& lexeme) {
        Token t; t.type = type; t.lexeme = lexeme; t.loc = loc; return t;
    }

    void unexpected(char c) {
        std::cerr << "lex error: unexpected character '" << c << "' at line " << loc.line << "\n";
    }

    Token readNumber() {
        size_t start = pos;
        while (std::isdigit(peek())) advance();
        return makeToken(TokenType::NUMBER_LITERAL, source.substr(start, pos - start).str());
    }

    Token readString() {
        std::string value;
        while (!isAtEnd() && peek() != '"') {
            if (peek() == '\\') { advance();
                switch (advance()) {
                    case 'n': value += '\n'; break;
                    case 't': value += '\t'; break;
                    case '\\': value += '\\'; break;
                    case '"': value += '"'; break;
                    default: value += '\\'; break;
                }
            } else { value += advance(); }
        }
        if (isAtEnd()) { std::cerr << "lex error: unterminated string\n"; }
        else { advance(); }
        return makeToken(TokenType::STRING_LITERAL, value);
    }

    Token readIdentifier() {
        size_t start = pos;
        while (std::isalnum(peek()) || peek() == '_') advance();
        std::string s = source.substr(start, pos - start).str();
        if (s == "mut")    return makeToken(TokenType::KW_MUT, s);
        if (s == "fn")     return makeToken(TokenType::KW_FN, s);
        if (s == "if")     return makeToken(TokenType::KW_IF, s);
        if (s == "elif")   return makeToken(TokenType::KW_ELIF, s);
        if (s == "else")   return makeToken(TokenType::KW_ELSE, s);
        if (s == "while")  return makeToken(TokenType::KW_WHILE, s);
        if (s == "break")  return makeToken(TokenType::KW_BREAK, s);
        if (s == "return") return makeToken(TokenType::KW_RETURN, s);
        if (s == "extern") return makeToken(TokenType::KW_EXTERN, s);
        if (s == "i64")    return makeToken(TokenType::KW_I64, s);
        if (s == "str")    return makeToken(TokenType::KW_STR, s);
        if (s == "bool")   return makeToken(TokenType::KW_BOOL, s);
        if (s == "python") return makeToken(TokenType::KW_PYTHON, s);
        if (s == "struct") return makeToken(TokenType::KW_STRUCT, s);
        if (s == "enum")   return makeToken(TokenType::KW_ENUM, s);
        if (s == "match")  return makeToken(TokenType::KW_MATCH, s);
        if (s == "import") return makeToken(TokenType::KW_IMPORT, s);
        return makeToken(TokenType::IDENTIFIER, s);
    }
};

// ============================================================================
// AST
// ============================================================================

enum class NodeKind {
    Number, String, Variable, Assign, Binary, Compare, Call, VarDecl,
    Return, If, While, Break, Block, PyBlock, Array, Index, Ref, Deref,
    StructLiteral, EnumConstruct, Match, FieldAccess, Function
};

struct ExprAST {
    SourceLocation loc;
    NodeKind kind;
    virtual ~ExprAST() = default;
    ExprAST() = default;
    explicit ExprAST(NodeKind k) : kind(k) {}
};

struct NumberExprAST : ExprAST {
    long long value;
    explicit NumberExprAST(long long v) : ExprAST(NodeKind::Number), value(v) {}
};

struct StringExprAST : ExprAST {
    std::string value;
    explicit StringExprAST(std::string v) : ExprAST(NodeKind::String), value(std::move(v)) {}
};

struct VariableExprAST : ExprAST {
    std::string name;
    explicit VariableExprAST(std::string n) : ExprAST(NodeKind::Variable), name(std::move(n)) {}
};

struct AssignExprAST : ExprAST {
    std::string varName;
    std::unique_ptr<ExprAST> rhs;
    AssignExprAST(std::string vn, std::unique_ptr<ExprAST> r)
        : ExprAST(NodeKind::Assign), varName(std::move(vn)), rhs(std::move(r)) {}
};

struct BinaryExprAST : ExprAST {
    char op;
    std::unique_ptr<ExprAST> lhs, rhs;
    BinaryExprAST(char o, std::unique_ptr<ExprAST> l, std::unique_ptr<ExprAST> r)
        : ExprAST(NodeKind::Binary), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct CompareExprAST : ExprAST {
    std::string op;
    std::unique_ptr<ExprAST> lhs, rhs;
    CompareExprAST(std::string o, std::unique_ptr<ExprAST> l, std::unique_ptr<ExprAST> r)
        : ExprAST(NodeKind::Compare), op(std::move(o)), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct CallExprAST : ExprAST {
    std::string callee;
    std::vector<Type> typeArgs;
    std::vector<std::unique_ptr<ExprAST>> args;
    CallExprAST(std::string c, std::vector<std::unique_ptr<ExprAST>> a)
        : ExprAST(NodeKind::Call), callee(std::move(c)), args(std::move(a)) {}
};

struct VarDeclAST : ExprAST {
    std::string varName;
    bool isMutable;
    Type varType;
    std::unique_ptr<ExprAST> init;
    VarDeclAST(std::string vn, bool mut, Type t, std::unique_ptr<ExprAST> i)
        : ExprAST(NodeKind::VarDecl), varName(std::move(vn)), isMutable(mut), varType(t), init(std::move(i)) {}
};

struct ReturnStmtAST : ExprAST {
    std::unique_ptr<ExprAST> value;
    explicit ReturnStmtAST(std::unique_ptr<ExprAST> v) : ExprAST(NodeKind::Return), value(std::move(v)) {}
};

struct IfStmtAST : ExprAST {
    std::unique_ptr<ExprAST> condition;
    std::unique_ptr<ExprAST> thenBlock;
    std::unique_ptr<ExprAST> elseBlock;
    IfStmtAST() : ExprAST(NodeKind::If) {}
};

struct WhileStmtAST : ExprAST {
    std::unique_ptr<ExprAST> condition;
    std::unique_ptr<ExprAST> body;
    WhileStmtAST() : ExprAST(NodeKind::While) {}
};

struct BreakStmtAST : ExprAST {
    BreakStmtAST() : ExprAST(NodeKind::Break) {}
};

struct BlockStmtAST : ExprAST {
    std::vector<std::unique_ptr<ExprAST>> stmts;
    BlockStmtAST() : ExprAST(NodeKind::Block) {}
};

struct PyBlockStmtAST : ExprAST {
    std::vector<std::string> codeStrings;
    PyBlockStmtAST() : ExprAST(NodeKind::PyBlock) {}
};

struct ArrayExprAST : ExprAST {
    std::vector<std::unique_ptr<ExprAST>> elements;
    ArrayExprAST() : ExprAST(NodeKind::Array) {}
};

struct IndexExprAST : ExprAST {
    std::unique_ptr<ExprAST> base;
    std::unique_ptr<ExprAST> index;
    IndexExprAST(std::unique_ptr<ExprAST> b, std::unique_ptr<ExprAST> i)
        : ExprAST(NodeKind::Index), base(std::move(b)), index(std::move(i)) {}
};

struct RefExprAST : ExprAST {
    std::unique_ptr<ExprAST> target;
    explicit RefExprAST(std::unique_ptr<ExprAST> t) : ExprAST(NodeKind::Ref), target(std::move(t)) {}
};

struct DerefExprAST : ExprAST {
    std::unique_ptr<ExprAST> target;
    explicit DerefExprAST(std::unique_ptr<ExprAST> t) : ExprAST(NodeKind::Deref), target(std::move(t)) {}
};

struct StructLiteralAST : ExprAST {
    std::string structName;
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> fields;
    StructLiteralAST() : ExprAST(NodeKind::StructLiteral) {}
};

struct EnumConstructAST : ExprAST {
    std::string enumName;
    std::string variantName;
    std::vector<std::unique_ptr<ExprAST>> args;
    EnumConstructAST() : ExprAST(NodeKind::EnumConstruct) {}
};

struct MatchArm {
    std::string enumName;
    std::string variantName;
    std::string bindName;
    std::unique_ptr<ExprAST> body;
};

struct MatchExprAST : ExprAST {
    std::unique_ptr<ExprAST> scrutinee;
    std::vector<MatchArm> arms;
    MatchExprAST() : ExprAST(NodeKind::Match) {}
};

struct FieldAccessAST : ExprAST {
    std::unique_ptr<ExprAST> base;
    std::string fieldName;
    FieldAccessAST(std::unique_ptr<ExprAST> b, std::string f)
        : ExprAST(NodeKind::FieldAccess), base(std::move(b)), fieldName(std::move(f)) {}
};

struct FunctionAST : ExprAST {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<std::pair<std::string, Type>> params;
    Type returnType;
    std::unique_ptr<ExprAST> body;
    bool isDeclaration = false;
    size_t bodyStart = 0;  // token index of '{' in token stream
    size_t bodyEnd = 0;    // token index past '}'
    FunctionAST() : ExprAST(NodeKind::Function) {}
};

struct ExternFn {
    std::string name;
    std::vector<Type> paramTypes;
    Type returnType;
    bool isVararg = false;
};

struct ProgramAST {
    std::vector<ExternFn> externs;
    std::vector<std::unique_ptr<FunctionAST>> functions;
    std::vector<StructDef> structs;
    std::vector<EnumDef> enums;
    std::vector<std::string> imports;
    std::vector<std::unique_ptr<VarDeclAST>> globals;
};

// ============================================================================
// SYMBOL TABLE
// ============================================================================

struct Symbol {
    Type type;
    llvm::AllocaInst* alloca = nullptr;
    llvm::GlobalVariable* global = nullptr;
    bool isMutable = false;
    bool moved = false;
    int borrowCount = 0;
};

class SymbolTable {
public:
    void enterScope() {
        scopes.emplace_back();
        borrowRecords.emplace_back();
    }

    void exitScope() {
        if (!borrowRecords.empty()) {
            for (auto& pair : borrowRecords.back()) {
                auto* sym = lookup(pair.first);
                if (sym) sym->borrowCount -= pair.second;
            }
            borrowRecords.pop_back();
        }
        if (!scopes.empty()) scopes.pop_back();
    }

    bool declare(const std::string& name, const Symbol& sym) {
        if (scopes.empty()) scopes.emplace_back();
        if (scopes.back().count(name)) return false;
        scopes.back()[name] = sym;
        return true;
    }

    Symbol* lookup(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    bool isDeclaredInCurrentScope(const std::string& name) {
        if (scopes.empty()) return false;
        return scopes.back().count(name) > 0;
    }

    void recordBorrow(const std::string& name) {
        if (!borrowRecords.empty()) {
            for (auto& pair : borrowRecords.back()) {
                if (pair.first == name) { pair.second++; return; }
            }
            borrowRecords.back().emplace_back(name, 1);
        }
    }

private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes;
    std::vector<std::vector<std::pair<std::string, int>>> borrowRecords;
};

// ============================================================================
// PARSER
// ============================================================================

class Codegen; // forward declaration for single-pass mode

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens(std::move(tokens)), pos(0) {}

    // Single-pass mode: parser emits IR directly instead of building AST
    void setCodegen(Codegen* c, bool mode = true) { cg = c; emitMode = mode; }
    void setSkipBodies(bool s) { skipBodies = s; }
    size_t getPos() const { return pos; }
    void setPos(size_t p) { pos = p; }
    bool isInEmitMode() const { return emitMode; }
    // Used by Codegen to re-parse function bodies from token ranges
    std::unique_ptr<ExprAST> reparseBlock() { return parseBlock(); }

    std::unique_ptr<ProgramAST> parseProgram() {
        auto prog = std::make_unique<ProgramAST>();
        while (!isAtEnd()) {
            if (check(TokenType::KW_EXTERN)) {
                parseExternBlock(*prog);
            } else if (check(TokenType::KW_FN)) {
                auto fn = parseFunction();
                if (fn) {
                    prog->functions.push_back(std::move(fn));
                    declaredVars.clear();
                }
            } else if (check(TokenType::KW_STRUCT)) {
                auto sd = parseStructDef();
                if (sd.name.empty()) { advance(); continue; }
                structRegistry[sd.name] = sd;
                prog->structs.push_back(std::move(sd));
            } else if (check(TokenType::KW_ENUM)) {
                auto ed = parseEnumDef();
                if (ed.name.empty()) { advance(); continue; }
                enumRegistry[ed.name] = ed;
                prog->enums.push_back(std::move(ed));
            } else if (check(TokenType::KW_IMPORT)) {
                advance(); // 'import'
                Token path = consume(TokenType::STRING_LITERAL, "expected module path string after 'import'");
                prog->imports.push_back(path.lexeme);
            } else if (check(TokenType::KW_MUT)) {
                advance(); // 'mut'
                auto decl = parseVarDecl(true);
                if (decl) { auto* vd = static_cast<VarDeclAST*>(decl.get()); globalVarNames.insert(vd->varName); prog->globals.push_back(std::unique_ptr<VarDeclAST>(static_cast<VarDeclAST*>(decl.release()))); }
            } else if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON) {
                auto decl = parseVarDecl(false);
                if (decl) { auto* vd = static_cast<VarDeclAST*>(decl.get()); globalVarNames.insert(vd->varName); prog->globals.push_back(std::unique_ptr<VarDeclAST>(static_cast<VarDeclAST*>(decl.release()))); }
            } else if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
                auto decl = parseVarDecl(false);
                if (decl) { auto* vd = static_cast<VarDeclAST*>(decl.get()); globalVarNames.insert(vd->varName); prog->globals.push_back(std::unique_ptr<VarDeclAST>(static_cast<VarDeclAST*>(decl.release()))); }
            } else {
                parseError("expected 'fn', 'extern', 'struct', 'enum', 'import', or global variable at top level");
                advance();
            }
        }
        return prog;
    }

    StructDef parseStructDef() {
        advance(); // 'struct'
        Token nameTok = consume(TokenType::IDENTIFIER, "expected struct name");
        StructDef sd;
        sd.name = nameTok.lexeme;
        consume(TokenType::LBRACE, "expected '{' after struct name");
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            Token fName = consume(TokenType::IDENTIFIER, "expected field name");
            consume(TokenType::COLON, "expected ':' after field name");
            Type fType = parseType();
            sd.fields.push_back({fName.lexeme, fType});
            if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
        }
        consume(TokenType::RBRACE, "expected '}' to close struct");
        return sd;
    }

    EnumDef parseEnumDef() {
        advance(); // 'enum'
        Token nameTok = consume(TokenType::IDENTIFIER, "expected enum name");
        EnumDef ed;
        ed.name = nameTok.lexeme;
        consume(TokenType::LBRACE, "expected '{' after enum name");
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            Token vName = consume(TokenType::IDENTIFIER, "expected variant name");
            EnumVariantDef variant;
            variant.name = vName.lexeme;
            if (match(TokenType::LPAREN)) {
                if (!check(TokenType::RPAREN)) {
                    do { variant.payloadTypes.push_back(parseType()); } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "expected ')' after variant types");
            }
            ed.variants.push_back(std::move(variant));
            if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
        }
        consume(TokenType::RBRACE, "expected '}' to close enum");
        return ed;
    }

    void parseExternBlock(ProgramAST& prog) {
        advance(); // 'extern'
        if (check(TokenType::STRING_LITERAL)) {
            std::string linkage = peek().lexeme;
            advance();
            if (linkage == "C") {
                consume(TokenType::LBRACE, "expected '{' after extern \"C\"");
                while (!check(TokenType::RBRACE) && !isAtEnd()) {
                    ExternFn ext;
                    consume(TokenType::KW_FN, "expected 'fn' in extern block");
                    Token nameTok = consume(TokenType::IDENTIFIER, "expected function name");
                    ext.name = nameTok.lexeme;
                    consume(TokenType::LPAREN, "expected '('");
                    if (!check(TokenType::RPAREN) && !check(TokenType::ELLIPSIS)) {
                        do {
                            if (check(TokenType::ELLIPSIS)) break;
                            Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
                            consume(TokenType::COLON, "expected ':'");
                            ext.paramTypes.push_back(parseType());
                        } while (match(TokenType::COMMA));
                    }
                    if (match(TokenType::ELLIPSIS)) ext.isVararg = true;
                    consume(TokenType::RPAREN, "expected ')'");
                    if (match(TokenType::ARROW)) ext.returnType = parseType();
                    else ext.returnType = Type::void_();
                    match(TokenType::SEMICOLON);
                    match(TokenType::NEWLINE);
                    prog.externs.push_back(ext);
                }
                consume(TokenType::RBRACE, "expected '}' to close extern block");
            }
        } else {
            parseError("expected '\"C\"' after extern");
        }
    }

    bool hadError() const { return error; }
    std::string errorMsg() const { return msg; }

private:
    std::vector<Token> tokens;
    size_t pos;
    bool error = false;
    std::string msg;
    std::unordered_set<std::string> declaredVars;
    std::unordered_set<std::string> globalVarNames;
    std::unordered_map<std::string, Type> varTypeMap;
    std::unordered_map<std::string, StructDef> structRegistry;
    std::unordered_map<std::string, EnumDef> enumRegistry;
    std::vector<std::string> parserTypeParams;
    Codegen* cg = nullptr;
    bool emitMode = false;
    bool skipBodies = false;

    const Token& peek(size_t a = 0) const {
        size_t i = pos + a; return i < tokens.size() ? tokens[i] : tokens.back();
    }
    const Token& advance() { if (!isAtEnd()) pos++; return previous(); }
    const Token& previous() const { return tokens[pos - 1]; }
    bool isAtEnd() const { return peek().type == TokenType::END_OF_FILE; }
    bool check(TokenType t) const { return !isAtEnd() && peek().type == t; }
    bool match(TokenType t) { if (check(t)) { advance(); return true; } return false; }

    void parseError(const std::string& message) {
        if (!error) {
            error = true;
            auto& t = peek();
            msg = "parse error at line " + std::to_string(t.loc.line)
                + ", col " + std::to_string(t.loc.col) + ": " + message;
        }
    }

    Token consume(TokenType t, const std::string& err) {
        if (check(t)) return advance();
        parseError(err);
        return tokens.empty() ? Token{} : tokens[0];
    }

    // ---- type ----
    Type parseType() {
        if (match(TokenType::KW_I64))  return Type::i64();
        if (match(TokenType::KW_STR))  return Type::str();
        if (match(TokenType::KW_BOOL)) return Type::boolean();
        if (match(TokenType::AMPERSAND)) {
            Type inner = parseType();
            return Type::ref(inner);
        }
        if (match(TokenType::LBRACKET)) {
            Type inner = parseType();
            consume(TokenType::RBRACKET, "expected ']' after array element type");
            return Type::array(inner);
        }
        if (match(TokenType::IDENTIFIER)) {
            std::string name = previous().lexeme;
            if (structRegistry.count(name)) return Type::struct_(name);
            if (enumRegistry.count(name)) return Type::enum_(name);
            for (auto& tp : parserTypeParams) if (tp == name) return Type::typeParam(name);
            parseError("unknown type '" + name + "'");
            return Type::i64();
        }
        parseError("expected type (i64, str, bool, [T], &T, struct name, or enum name)");
        return Type::i64();
    }

    // ---- function ----
    std::unique_ptr<FunctionAST> parseFunction() {
        advance(); // 'fn'
        Token nameTok = consume(TokenType::IDENTIFIER, "expected function name");
        std::vector<std::string> typeParams;
        if (match(TokenType::LT)) {
            do {
                Token tp = consume(TokenType::IDENTIFIER, "expected type parameter name");
                typeParams.push_back(tp.lexeme);
            } while (match(TokenType::COMMA));
            consume(TokenType::GT, "expected '>' after type parameters");
        }
        parserTypeParams = typeParams;
        consume(TokenType::LPAREN, "expected '(' after function name");

        std::vector<std::pair<std::string, Type>> params;
        if (!check(TokenType::RPAREN)) {
            do {
                Token pName = consume(TokenType::IDENTIFIER, "expected parameter name");
                consume(TokenType::COLON, "expected ':' after parameter name");
                Type pType = parseType();
                params.emplace_back(pName.lexeme, pType);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "expected ')' after parameters");

        Type retType = Type::void_();
        if (match(TokenType::ARROW)) {
            retType = parseType();
        }

        for (auto& p : params) {
            declaredVars.insert(p.first);
            varTypeMap[p.first] = p.second;
        }
        
        bool isDecl = !check(TokenType::LBRACE);

        auto fn = std::make_unique<FunctionAST>();
        fn->name = nameTok.lexeme;
        fn->typeParams = std::move(typeParams);
        fn->params = std::move(params);
        fn->returnType = retType;
        fn->isDeclaration = isDecl;
        fn->loc = nameTok.loc;

        if (!isDecl) {
            if (skipBodies) {
                fn->bodyStart = pos;
                skipBlock();
                fn->bodyEnd = pos;
            } else {
                fn->bodyStart = pos;
                fn->body = parseBlock();
                fn->bodyEnd = pos;
            }
        }
        parserTypeParams.clear();
        return fn;
    }

    // Fast-forward through a { ... } block without building AST
    void skipBlock() {
        if (!check(TokenType::LBRACE)) return;
        advance(); // '{'
        int depth = 1;
        while (depth > 0 && !isAtEnd()) {
            if (check(TokenType::LBRACE)) { depth++; advance(); }
            else if (check(TokenType::RBRACE)) { depth--; advance(); }
            else if (check(TokenType::STRING_LITERAL)) { advance(); }
            else { advance(); }
        }
    }

    // ---- block ----
    std::unique_ptr<ExprAST> parseBlock() {
        if (!check(TokenType::LBRACE)) {
            return nullptr;
        }
        advance(); // '{'
        auto block = std::make_unique<BlockStmtAST>();
        block->loc = previous().loc;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            if (check(TokenType::SEMICOLON)) { advance(); continue; }
            auto s = parseStatement();
            if (s) block->stmts.push_back(std::move(s));
        }
        consume(TokenType::RBRACE, "expected '}' to close block");
        return block;
    }

    // ---- statement ----
    std::unique_ptr<ExprAST> parseStatement() {
        if (check(TokenType::KW_RETURN)) return parseReturnStmt();
        if (check(TokenType::KW_IF))     return parseIfStmt();
        if (check(TokenType::KW_WHILE))  return parseWhileStmt();
        if (check(TokenType::KW_BREAK))  { advance(); return std::make_unique<BreakStmtAST>(); }
        if (check(TokenType::KW_PYTHON)) return parsePythonBlock();
        if (check(TokenType::LBRACE)) {
            advance();
            auto block = std::make_unique<BlockStmtAST>();
            block->loc = previous().loc;
            while (!check(TokenType::RBRACE) && !isAtEnd()) {
                if (check(TokenType::SEMICOLON)) { advance(); continue; }
                auto s = parseStatement();
                if (s) block->stmts.push_back(std::move(s));
            }
            consume(TokenType::RBRACE, "expected '}' to close block");
            return block;
        }
        if (check(TokenType::KW_MUT))    { advance(); return parseVarDecl(true); }

        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON) {
            return parseVarDecl(false);
        }

        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
            std::string name = peek().lexeme;
            if (declaredVars.count(name) || globalVarNames.count(name)) {
                return parseExprStmt();
            }
            return parseVarDecl(false);
        }

        return parseExprStmt();
    }

    std::unique_ptr<ExprAST> parseReturnStmt() {
        auto node = std::make_unique<ReturnStmtAST>(nullptr);
        node->loc = previous().loc;
        advance(); // 'return'
        if (!check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE) && !check(TokenType::RBRACE) && !isAtEnd()) {
            node->value = parseExpression();
        }
        match(TokenType::SEMICOLON);
        match(TokenType::NEWLINE);
        return node;
    }

    // Parse: cond { then } [elif ...] [else ...]
    // Returns: if cond { then } else { ... }
    std::unique_ptr<ExprAST> parseElifChain() {
        auto node = std::make_unique<IfStmtAST>();
        node->loc = previous().loc;
        auto cond = parseExpression();
        if (!cond) return nullptr;
        node->condition = std::move(cond);
        node->thenBlock = parseBlock();
        if (match(TokenType::KW_ELIF)) {
            auto wrap = std::make_unique<BlockStmtAST>();
            wrap->stmts.push_back(parseElifChain());
            node->elseBlock = std::move(wrap);
        } else if (match(TokenType::KW_ELSE)) {
            node->elseBlock = parseBlock();
        }
        return node;
    }

    std::unique_ptr<ExprAST> parseIfStmt() {
        auto node = std::make_unique<IfStmtAST>();
        node->loc = previous().loc;
        advance(); // 'if'
        auto cond = parseExpression();
        if (!cond) return nullptr;
        node->condition = std::move(cond);
        node->thenBlock = parseBlock();
        if (match(TokenType::KW_ELIF)) {
            auto wrap = std::make_unique<BlockStmtAST>();
            wrap->stmts.push_back(parseElifChain());
            node->elseBlock = std::move(wrap);
        } else if (match(TokenType::KW_ELSE)) {
            node->elseBlock = parseBlock();
        }
        return node;
    }

    std::unique_ptr<ExprAST> parseWhileStmt() {
        auto node = std::make_unique<WhileStmtAST>();
        node->loc = previous().loc;
        advance(); // 'while'
        auto cond = parseExpression();
        if (!cond) return nullptr;
        node->condition = std::move(cond);
        node->body = parseBlock();
        return node;
    }

    std::unique_ptr<ExprAST> parsePythonBlock() {
        advance(); // 'python'
        auto node = std::make_unique<PyBlockStmtAST>();
        node->loc = previous().loc;
        consume(TokenType::LBRACE, "expected '{' after 'python'");
        while (check(TokenType::STRING_LITERAL)) {
            node->codeStrings.push_back(peek().lexeme);
            advance();
        }
        consume(TokenType::RBRACE, "expected '}' to close python block");
        return node;
    }

    std::unique_ptr<ExprAST> parseVarDecl(bool isMutable) {
        Token nameTok = advance();
        if (declaredVars.count(nameTok.lexeme)) {
            parseError("variable '" + nameTok.lexeme + "' already declared");
        }
        declaredVars.insert(nameTok.lexeme);
        Type varType = Type::i64();
        bool hasTypeAnnotation = match(TokenType::COLON);
        if (hasTypeAnnotation) {
            varType = parseType();
        }
        consume(TokenType::ASSIGN, "expected '=' in variable declaration");
        auto init = parseExpression();
        if (!init) parseError("expected expression after '='");
        if (!hasTypeAnnotation) {
            switch (init->kind) {
                case NodeKind::String:
                    varType = Type::str();
                    break;
                case NodeKind::Array:
                    varType = Type::array(Type::i64());
                    break;
                case NodeKind::Variable: {
                    auto* varInit = static_cast<VariableExprAST*>(init.get());
                    auto it = varTypeMap.find(varInit->name);
                    if (it != varTypeMap.end()) varType = it->second;
                    break;
                }
                case NodeKind::Ref: {
                    auto* refInit = static_cast<RefExprAST*>(init.get());
                    if (refInit->target->kind == NodeKind::Variable) {
                        auto* refVar = static_cast<VariableExprAST*>(refInit->target.get());
                        auto it = varTypeMap.find(refVar->name);
                        if (it != varTypeMap.end()) varType = Type::ref(it->second);
                    }
                    break;
                }
                case NodeKind::StructLiteral: {
                    auto* structInit = static_cast<StructLiteralAST*>(init.get());
                    if (structRegistry.count(structInit->structName))
                        varType = Type::struct_(structInit->structName);
                    break;
                }
                case NodeKind::EnumConstruct: {
                    auto* enumInit = static_cast<EnumConstructAST*>(init.get());
                    if (enumRegistry.count(enumInit->enumName))
                        varType = Type::enum_(enumInit->enumName);
                    break;
                }
                default: break;
            }
        }
        varTypeMap[nameTok.lexeme] = varType;
        match(TokenType::SEMICOLON);
        match(TokenType::NEWLINE);

        auto decl = std::make_unique<VarDeclAST>(nameTok.lexeme, isMutable, varType, std::move(init));
        decl->loc = nameTok.loc;
        return decl;
    }

    std::unique_ptr<ExprAST> parseExprStmt() {
        auto expr = parseExpression();
        if (!expr) return nullptr;
        match(TokenType::SEMICOLON);
        match(TokenType::NEWLINE);
        return expr;
    }

    // ---- expression precedence ----
    int getPrec(TokenType t) {
        switch (t) {
            case TokenType::STAR:
            case TokenType::SLASH: return 60;
            case TokenType::PLUS:
            case TokenType::MINUS: return 50;
            case TokenType::EQ_EQ:
            case TokenType::NE:    return 40;
            case TokenType::LT:
            case TokenType::GT:
            case TokenType::LE:
            case TokenType::GE:    return 40;
            case TokenType::PIPE_PIPE: return 30;
            default: return -1;
        }
    }

    std::unique_ptr<ExprAST> parsePostfix(std::unique_ptr<ExprAST> lhs) {
        while (lhs) {
            if (check(TokenType::LBRACKET)) {
                advance(); // '['
                auto idx = parseExpression();
                consume(TokenType::RBRACKET, "expected ']' after index");
                auto idxNode = std::make_unique<IndexExprAST>(std::move(lhs), std::move(idx));
                idxNode->loc = previous().loc;
                lhs = std::move(idxNode);
            } else if (check(TokenType::DOT)) {
                advance(); // '.'
                Token fName = consume(TokenType::IDENTIFIER, "expected field name after '.'");
                auto fa = std::make_unique<FieldAccessAST>(std::move(lhs), fName.lexeme);
                fa->loc = fName.loc;
                lhs = std::move(fa);
            } else {
                break;
            }
        }
        return lhs;
    }

    std::unique_ptr<ExprAST> parseExpression() {
        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
            std::string name = peek().lexeme;
            if (!declaredVars.count(name)) {
                parseError("variable '" + name + "' not declared");
                return nullptr;
            }
            Token nameTok = advance();
            advance(); // '='
            auto rhs = parseExpression();
            auto assign = std::make_unique<AssignExprAST>(nameTok.lexeme, std::move(rhs));
            assign->loc = nameTok.loc;
            return assign;
        }
        auto lhs = parsePostfix(parsePrimary());
        if (!lhs) return nullptr;
        return parseBinaryOpRHS(0, std::move(lhs));
    }

    std::unique_ptr<ExprAST> parseBinaryOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs) {
        if (!lhs) return nullptr;
        while (true) {
            int prec = getPrec(peek().type);
            if (prec < exprPrec) return lhs;

            Token opTok = advance();
            auto rhs = parsePostfix(parsePrimary());
            if (!rhs) return lhs;

            int nextPrec = getPrec(peek().type);
            if (prec < nextPrec) {
                rhs = parseBinaryOpRHS(prec + 1, std::move(rhs));
                if (!rhs) return lhs;
            }

            TokenType tt = opTok.type;
            if (tt == TokenType::PLUS || tt == TokenType::MINUS ||
                tt == TokenType::STAR || tt == TokenType::SLASH ||
                tt == TokenType::PIPE_PIPE) {
                lhs = std::make_unique<BinaryExprAST>(opTok.lexeme[0], std::move(lhs), std::move(rhs));
            } else {
                lhs = std::make_unique<CompareExprAST>(opTok.lexeme, std::move(lhs), std::move(rhs));
            }
            lhs->loc = opTok.loc;
        }
    }

    std::unique_ptr<ExprAST> parsePrimary() {
        if (match(TokenType::NUMBER_LITERAL)) {
            auto n = std::make_unique<NumberExprAST>(std::stoll(previous().lexeme));
            n->loc = previous().loc; return n;
        }
        if (match(TokenType::STRING_LITERAL)) {
            auto s = std::make_unique<StringExprAST>(previous().lexeme);
            s->loc = previous().loc; return s;
        }
        if (match(TokenType::LBRACKET)) {
            std::vector<std::unique_ptr<ExprAST>> elems;
            if (!check(TokenType::RBRACKET)) {
                do {
                    auto e = parseExpression();
                    if (e) elems.push_back(std::move(e));
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RBRACKET, "expected ']' after array literal");
            auto arr = std::make_unique<ArrayExprAST>();
            arr->elements = std::move(elems);
            arr->loc = previous().loc;
            return arr;
        }
        if (match(TokenType::KW_MATCH)) {
            auto mn = std::make_unique<MatchExprAST>();
            mn->loc = previous().loc;
            mn->scrutinee = parseExpression();
            consume(TokenType::LBRACE, "expected '{' after match expression");
            while (!check(TokenType::RBRACE) && !isAtEnd()) {
                MatchArm arm;
                Token enumTk = consume(TokenType::IDENTIFIER, "expected enum name in match arm");
                consume(TokenType::DOT, "expected '.' after enum name");
                Token varTk = consume(TokenType::IDENTIFIER, "expected variant name");
                arm.enumName = enumTk.lexeme;
                arm.variantName = varTk.lexeme;
                if (match(TokenType::LPAREN)) {
                    Token bind = consume(TokenType::IDENTIFIER, "expected binding name");
                    arm.bindName = bind.lexeme;
                    consume(TokenType::RPAREN, "expected ')'");
                }
                consume(TokenType::FAT_ARROW, "expected '=>' after match arm pattern");
                if (check(TokenType::LBRACE)) {
                    arm.body = parseBlock();
                } else {
                    arm.body = parseStatement();
                }
                mn->arms.push_back(std::move(arm));
                if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
            }
            consume(TokenType::RBRACE, "expected '}' to close match");
            return mn;
        }
        if (match(TokenType::IDENTIFIER)) {
            std::string name = previous().lexeme;
            SourceLocation loc = previous().loc;
            // Enum construction: EnumName.Variant(args...)
            if (enumRegistry.count(name) && check(TokenType::DOT)) {
                advance(); // '.'
                Token varTok = consume(TokenType::IDENTIFIER, "expected variant name");
                auto ec = std::make_unique<EnumConstructAST>();
                ec->enumName = name;
                ec->variantName = varTok.lexeme;
                ec->loc = loc;
                if (match(TokenType::LPAREN)) {
                    if (!check(TokenType::RPAREN)) {
                        do {
                            ec->args.push_back(parseExpression());
                        } while (match(TokenType::COMMA));
                    }
                    consume(TokenType::RPAREN, "expected ')'");
                }
                return ec;
            }
            if (match(TokenType::LPAREN)) {
                std::vector<std::unique_ptr<ExprAST>> args;
                if (!check(TokenType::RPAREN)) {
                    do {
                        auto a = parseExpression();
                        if (a) args.push_back(std::move(a));
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "expected ')' after arguments");
                auto c = std::make_unique<CallExprAST>(name, std::move(args));
                c->loc = loc;
                return c;
            }
            if (structRegistry.count(name) && match(TokenType::LBRACE)) {
                auto sl = std::make_unique<StructLiteralAST>();
                sl->structName = name;
                sl->loc = loc;
                while (!check(TokenType::RBRACE) && !isAtEnd()) {
                    Token fName = consume(TokenType::IDENTIFIER, "expected field name");
                    consume(TokenType::COLON, "expected ':'");
                    auto fVal = parseExpression();
                    sl->fields.emplace_back(fName.lexeme, std::move(fVal));
                    if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
                }
                consume(TokenType::RBRACE, "expected '}' to close struct literal");
                return sl;
            }
            auto v = std::make_unique<VariableExprAST>(name);
            v->loc = loc;
            return v;
        }
        if (match(TokenType::LPAREN)) {
            auto e = parseExpression();
            consume(TokenType::RPAREN, "expected ')' after expression");
            return e;
        }
        if (match(TokenType::MINUS)) {
            auto rhs = parsePrimary();
            if (!rhs) return nullptr;
            auto zero = std::make_unique<NumberExprAST>(0);
            zero->loc = previous().loc;
            auto neg = std::make_unique<BinaryExprAST>('-', std::move(zero), std::move(rhs));
            neg->loc = previous().loc;
            return neg;
        }
        if (match(TokenType::AMPERSAND)) {
            auto target = parsePrimary();
            if (!target) return nullptr;
            auto ref = std::make_unique<RefExprAST>(std::move(target));
            ref->loc = previous().loc;
            return ref;
        }
        if (match(TokenType::STAR)) {
            auto target = parsePrimary();
            if (!target) return nullptr;
            auto deref = std::make_unique<DerefExprAST>(std::move(target));
            deref->loc = previous().loc;
            return deref;
        }
        return nullptr;
    }

    // ---- emit-mode (single-pass): parse + emit directly, no AST nodes ----
public:
    enum Prec : int {
        PREC_NONE = 0,
        PREC_OR = 10,
        PREC_COMPARE = 20,
        PREC_TERM = 30,
        PREC_FACTOR = 40,
        PREC_POSTFIX = 60,
        PREC_CALL = 70,
    };

    int tokPrec(TokenType t);
    llvm::Value* parseExpressionEmit(int minPrec = PREC_NONE);
    llvm::Value* parseNudEmit();
    llvm::Value* parseIdentEmit();
    llvm::Value* emitArithOpEmit(char op, llvm::Value* l, llvm::Value* r);
    llvm::Value* emitBinaryOpEmit(Token opTok, llvm::Value* l, llvm::Value* r);
    llvm::Value* emitIndexEmit(llvm::Value* base, llvm::Value* index);
    llvm::Value* emitFieldAccessEmit(llvm::Value* base, const std::string& field);
    llvm::Value* emitArrayLiteralEmit(const std::vector<llvm::Value*>& elems);
    llvm::Value* emitStructLiteralEmit(const std::string& name);
    llvm::Value* emitEnumConstructEmit(const std::string& enumName, const std::string& variantName);
    llvm::Value* emitCallEmit(const std::string& callee);
    llvm::Value* parseMatchEmit();
    void parseStatementEmit();
    void parseExpressionStmtEmit();
    void parseBlockEmit();
    void parseVarDeclEmit(bool isMutable);
    void parseReturnEmit();
    void parseBreakEmit();
    void parseIfEmit();
    void parseWhileEmit();
    void parsePythonBlockEmit();
    std::string generateGenericInstance(FunctionAST* genFn, const std::string& callee);
};

// ============================================================================
// CODEGEN
// ============================================================================

class Codegen {
public:
    Codegen() : ctx(std::make_unique<llvm::LLVMContext>()),
                mod(std::make_unique<llvm::Module>("flint_module", *ctx)),
                builder(std::make_unique<llvm::IRBuilder<>>(*ctx)),
                i64Ty(llvm::Type::getInt64Ty(*ctx)),
                i32Ty(llvm::Type::getInt32Ty(*ctx)),
                i8Ty(llvm::Type::getInt8Ty(*ctx)),
                voidTy(llvm::Type::getVoidTy(*ctx)),
                i8PtrTy(llvm::PointerType::get(*ctx, 0)) {
        mod->setTargetTriple(llvm::Triple("aarch64-unknown-linux-android24"));
        mod->setDataLayout("e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128");
    }

    void setParser(Parser* p) { parser = p; }
    bool usedPython() const { return hasPython; }

    bool generateDeclarations(ProgramAST& prog) {
        PROFILE_BEGIN("codegen_init");
        initBuiltins();
        PROFILE_END();

        PROFILE_BEGIN("codegen_declare_externs");
        declareExterns(prog.externs);
        PROFILE_END();

        PROFILE_BEGIN("codegen_declare_structs");
        declareStructs(prog.structs);
        PROFILE_END();

        PROFILE_BEGIN("codegen_declare_enums");
        declareEnums(prog.enums);
        PROFILE_END();

        programGlobals = &prog.globals;
        PROFILE_BEGIN("codegen_declare_globals");
        declareGlobals(prog);
        PROFILE_END();

        PROFILE_BEGIN("codegen_declare_functions");
        declareFunctions(prog);
        PROFILE_END();
        return true;
    }

    bool emitFunctionBodies(ProgramAST& prog) {
        PROFILE_BEGIN("codegen_emit_functions");
        for (auto& fn : prog.functions) {
            if (!emitFunction(fn.get())) return false;
        }
        PROFILE_END();
        return true;
    }

    bool generate(ProgramAST& prog, const std::string& outputPath) {
        if (!generateDeclarations(prog)) return false;
        if (!emitFunctionBodies(prog)) return false;

        PROFILE_BEGIN("codegen_output");
        bool ok = emitModuleOutput(mod.get(), outputPath);
        PROFILE_END();
        return ok;
    }

    void declareExterns(std::vector<ExternFn>& externs) {
        for (auto& ext : externs) {
            std::vector<llvm::Type*> paramTys;
            for (auto& pt : ext.paramTypes) paramTys.push_back(llvmType(pt, *ctx, mod.get()));
            llvm::Type* retTy = llvmType(ext.returnType, *ctx, mod.get());
            auto fTy = llvm::FunctionType::get(retTy, paramTys, ext.isVararg);
            auto f = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, ext.name, mod.get());
            functionMap[ext.name] = f;
        }
    }

    void declareStructs(std::vector<StructDef>& structs) {
        for (auto& sd : structs) {
            structRegistry[sd.name] = sd;
            auto* st = llvm::StructType::create(*ctx, sd.name);
            std::vector<llvm::Type*> fieldTys;
            for (auto& f : sd.fields) fieldTys.push_back(llvmType(f.type, *ctx, mod.get()));
            st->setBody(fieldTys);
        }
    }

    void declareEnums(std::vector<EnumDef>& enums) {
        for (auto& ed : enums) {
            enumRegistry[ed.name] = ed;
            // Compute max payload size across all variants
            size_t maxSize = 0;
            for (auto& v : ed.variants) {
                size_t sz = 0;
                for (auto& pt : v.payloadTypes) {
                    auto* lt = llvmType(pt, *ctx, mod.get());
                    sz += mod->getDataLayout().getTypeStoreSize(lt);
                }
                if (sz > maxSize) maxSize = sz;
            }
            if (maxSize < 1) maxSize = 1;
            // Round to 8-byte alignment
            maxSize = (maxSize + 7) & ~(size_t)7;
            auto* payloadTy = llvm::ArrayType::get(i8Ty, maxSize);
            auto* enumTy = llvm::StructType::create(*ctx, {i8Ty, payloadTy}, ed.name);
            (void)enumTy; // created and registered by name
        }
    }

    // Methods used by Parser in emit-mode (single-pass)
    llvm::AllocaInst* createEntryAlloca(llvm::Type* ty, const std::string& name) {
        llvm::IRBuilder<> tmp(&currentFunc->getEntryBlock(), currentFunc->getEntryBlock().begin());
        return tmp.CreateAlloca(ty, nullptr, name);
    }
    Type resolveType(Type t) {
        if (t.kind == TypeKind::TypeParam) {
            auto it = typeSubstMap.find(t.structName);
            if (it != typeSubstMap.end()) return it->second;
            return t;
        }
        if (t.isCompound() && t.elemType) {
            Type inner = resolveType(*t.elemType);
            if (t.kind == TypeKind::Array) return Type::array(inner);
            if (t.kind == TypeKind::Ref) return Type::ref(inner);
        }
        return t;
    }
    llvm::Type* resolvedLlvmType(Type t) {
        return llvmType(resolveType(t), *ctx, mod.get());
    }

public:
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> mod;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Cached LLVM types
    llvm::Type* i64Ty = nullptr;
    llvm::Type* i32Ty = nullptr;
    llvm::Type* i8Ty = nullptr;
    llvm::Type* voidTy = nullptr;
    llvm::Type* i8PtrTy = nullptr;
    SymbolTable symTable;
    llvm::Function* currentFunc = nullptr;
    std::vector<llvm::BasicBlock*> breakStack;

    std::unordered_map<std::string, llvm::Function*> functionMap;
    std::unordered_map<std::string, StructDef> structRegistry;
    std::unordered_map<std::string, EnumDef> enumRegistry;
    std::unordered_map<std::string, FunctionAST*> genericFunctions;
    bool hasPython = false;
    std::unordered_map<std::string, Type> typeSubstMap;
    std::unordered_map<std::string, llvm::Function*> genericCache;
    std::unordered_map<std::string, Symbol> globalSymTable;
    std::vector<std::unique_ptr<VarDeclAST>>* programGlobals = nullptr;
    Parser* parser = nullptr;

    void initBuiltins() {
        auto add = [&](const std::string& n, llvm::Type* r, std::vector<llvm::Type*> p, bool va) {
            auto f = llvm::Function::Create(llvm::FunctionType::get(r, p, va), llvm::Function::ExternalLinkage, n, mod.get());
            functionMap[n] = f;
        };
        add("flint_println_i64", voidTy, {i64Ty}, false);
        add("flint_println_str", voidTy, {i8PtrTy}, false);
        add("flint_panic", voidTy, {i8PtrTy}, false);
        add("flint_py_init", voidTy, {}, false);
        add("flint_py_run", voidTy, {i8PtrTy}, false);
        add("flint_py_fini", voidTy, {}, false);
        add("flint_py_eval_int", i64Ty, {i8PtrTy}, false);
        add("flint_bounds_check", voidTy, {i64Ty, i64Ty}, false);
    }

    void declareGlobals(ProgramAST& prog) {
        for (auto& g : prog.globals) {
            llvm::Type* lty = llvmType(g->varType, *ctx, mod.get());
            llvm::GlobalVariable* gv = new llvm::GlobalVariable(
                *mod, lty, false, llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(lty), g->varName);
            globalSymTable[g->varName] = {g->varType, nullptr, gv, g->isMutable, false, 0};
        }
    }

    void declareFunctions(ProgramAST& prog) {
        for (auto& fn : prog.functions) {
            if (functionMap.count(fn->name)) continue; // already declared (e.g. from --use-interface)
            if (!fn->typeParams.empty()) {
                genericFunctions[fn->name] = fn.get();
                continue;
            }
            std::vector<llvm::Type*> paramTys;
            for (auto& p : fn->params) paramTys.push_back(llvmType(p.second, *ctx, mod.get()));
            // Main function returns i32 and takes argc/argv for CLI access
            llvm::Type* retTy = (fn->name == "main") ? i32Ty : llvmType(fn->returnType, *ctx, mod.get());
            llvm::FunctionType* fTy;
            if (fn->name == "main") {
                fTy = llvm::FunctionType::get(i32Ty, {i64Ty, i8PtrTy}, false);
            } else {
                fTy = llvm::FunctionType::get(retTy, paramTys, false);
            }
            // Check if the module already has this function (from interface merge)
            llvm::Function* f = mod->getFunction(fn->name);
            if (!f) {
                f = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, fn->name, mod.get());
            }
            if (fn->name == "main") {
                auto ai = f->args().begin();
                ai->setName("flint_argc");
                (ai + 1)->setName("flint_argv");
            } else {
                size_t i = 0;
                for (auto& arg : f->args()) {
                    if (i < fn->params.size()) arg.setName(fn->params[i].first);
                    i++;
                }
            }
            functionMap[fn->name] = f;
        }
    }

    // After merging interface modules via llvm::Linker, sync LLVM declarations
    // into the C++ symbol tables so emitCall / emitVarDecl can find them.
    void syncInterfaceSymbols() {
        for (auto& f : mod->functions()) {
            if (f.isDeclaration() && functionMap.count(f.getName().str()) == 0) {
                functionMap[f.getName().str()] = &f;
            }
            if (f.getName() == "main") {
                functionMap["main"] = &f;
            }
        }
        // Sync globals
        for (auto& gv : mod->globals()) {
            if (globalSymTable.count(gv.getName().str()) == 0) {
                llvm::Type* ty = gv.getValueType();
                Type flintType = Type::i64();
                if (ty->isPointerTy()) flintType = Type::str();
                globalSymTable[gv.getName().str()] = {flintType, nullptr, &gv, !gv.isConstant(), false, 0};
            }
        }
    }

    bool emitFunction(FunctionAST* fn) {
        if (fn->isDeclaration) return true;
        if (!fn->typeParams.empty()) return true; // generic, specialized on demand
        auto fIt = functionMap.find(fn->name);
        if (fIt == functionMap.end()) return false;
        llvm::Function* f = fIt->second;
        currentFunc = f;

        auto entry = llvm::BasicBlock::Create(*ctx, "entry", f);
        builder->SetInsertPoint(entry);
        symTable.enterScope();

        // Store CLI args in globals for flint_arg_count() / flint_get_arg()
        if (fn->name == "main") {
            auto* argcGV = mod->getOrInsertGlobal("flint_g_argc", i64Ty);
            auto* argvGV = mod->getOrInsertGlobal("flint_g_argv", i8PtrTy);
            auto ai = f->args().begin();
            builder->CreateStore(&*ai, argcGV);
            builder->CreateStore(&*(ai + 1), argvGV);
            // Initialize global variables
            if (programGlobals) {
                for (auto& g : *programGlobals) {
                    auto git = globalSymTable.find(g->varName);
                    if (git != globalSymTable.end()) {
                        auto* init = emitExpr(g->init.get());
                        if (init) builder->CreateStore(init, git->second.global);
                    }
                }
            }
        }

        size_t i = 0;
        for (auto& arg : f->args()) {
            if (fn->name == "main" && i >= fn->params.size()) { i++; continue; }
            std::string pName = fn->params[i].first;
            Type pType = resolveType(fn->params[i].second);
            llvm::AllocaInst* alloca = createEntryAlloca(resolvedLlvmType(fn->params[i].second), pName);
            builder->CreateStore(&arg, alloca);
            symTable.declare(pName, {pType, alloca, nullptr, false, false, 0});
            i++;
        }

        if (fn->name == "main" && hasPython) {
            auto it = functionMap.find("flint_py_init");
            if (it != functionMap.end()) builder->CreateCall(it->second, {});
        }

        if (fn->body) {
            emitStmt(fn->body.get());
        } else if (fn->bodyStart > 0 && fn->bodyEnd > 0 && parser) {

            size_t savedPos = parser->getPos();
            parser->setPos(fn->bodyStart);
            parser->setCodegen(this, true);
            parser->parseBlockEmit();
            if (parser->hadError()) {
                llvm::errs() << "FLINT COMPILE ERROR: " << parser->errorMsg() << "\n";
                parser->setPos(savedPos);
                return false;
            }
            parser->setPos(fn->bodyEnd);
            parser->setPos(savedPos);
        }

        bool hasReturnType = fn->returnType.kind != TypeKind::Void;
        if (!builder->GetInsertBlock()->getTerminator()) {
            if (fn->name == "main" && hasPython) {
                auto it = functionMap.find("flint_py_fini");
                if (it != functionMap.end()) builder->CreateCall(it->second, {});
            }
            if (fn->name == "main") {
            builder->CreateRet(llvm::ConstantInt::get(i32Ty, 0));
            } else if (hasReturnType) {
                builder->CreateRet(llvm::ConstantInt::get(llvmType(fn->returnType, *ctx, mod.get()), 0));
            } else {
                builder->CreateRetVoid();
            }
        }

        symTable.exitScope();

    if (0 && llvm::verifyFunction(*f, &llvm::outs())) {
        std::cerr << "function verification failed: " << fn->name << "\n";
        return false;
    }
        return true;
    }

    void emitStmt(ExprAST* node) {
        switch (node->kind) {
            case NodeKind::VarDecl: emitVarDecl(static_cast<VarDeclAST*>(node)); return;
            case NodeKind::Return:  emitReturn(static_cast<ReturnStmtAST*>(node)); return;
            case NodeKind::Break:
                if (breakStack.empty()) { std::cerr << "break outside loop\n"; return; }
                builder->CreateBr(breakStack.back());
                return;
            case NodeKind::If:      emitIf(static_cast<IfStmtAST*>(node)); return;
            case NodeKind::While:   emitWhile(static_cast<WhileStmtAST*>(node)); return;
            case NodeKind::Block:   emitBlock(static_cast<BlockStmtAST*>(node)); return;
            case NodeKind::PyBlock: emitPythonBlock(static_cast<PyBlockStmtAST*>(node)); return;
            default: emitExpr(node); return;
        }
    }

    llvm::Value* emitExpr(ExprAST* node) {
        switch (node->kind) {
            case NodeKind::Number: {
                auto* n = static_cast<NumberExprAST*>(node);
                return llvm::ConstantInt::get(i64Ty, n->value);
            }
            case NodeKind::String: {
                auto* s = static_cast<StringExprAST*>(node);
                return builder->CreateGlobalString(s->value, "str");
            }
            case NodeKind::Variable: {
                auto* v = static_cast<VariableExprAST*>(node);
                auto* sym = symTable.lookup(v->name);
                if (!sym) {
                    auto git = globalSymTable.find(v->name);
                    if (git != globalSymTable.end()) {
                        auto* gv = git->second.global;
                        return builder->CreateLoad(llvmType(git->second.type, *ctx, mod.get()), gv, v->name.c_str());
                    }
                    std::cerr << "codegen: undefined var '" << v->name << "'\n"; return nullptr;
                }
                if (sym->moved) {
                    std::cerr << "codegen: use of moved variable '" << v->name << "'\n";
                    return nullptr;
                }
                if (sym->borrowCount > 0 && !sym->type.isCopyType()) {
                    std::cerr << "codegen: cannot move '" << v->name << "' while borrowed\n";
                    return nullptr;
                }
                llvm::Value* ptr = sym->alloca ? (llvm::Value*)sym->alloca : (llvm::Value*)sym->global;
                return builder->CreateLoad(llvmType(sym->type, *ctx, mod.get()), ptr, v->name.c_str());
            }
            case NodeKind::Assign: {
                auto* a = static_cast<AssignExprAST*>(node);
                auto* sym = symTable.lookup(a->varName);
                if (!sym) {
                    auto git = globalSymTable.find(a->varName);
                    if (git == globalSymTable.end()) {
                        std::cerr << "codegen: undefined var '" << a->varName << "'\n"; return nullptr;
                    }
                    if (!git->second.isMutable) {
                        std::cerr << "codegen: cannot assign to immutable global '" << a->varName << "'\n"; return nullptr;
                    }
                    auto* val = emitExpr(a->rhs.get());
                    if (!val) return nullptr;
                    return builder->CreateStore(val, git->second.global);
                }
                if (!sym->isMutable) { std::cerr << "codegen: cannot assign to immutable '" << a->varName << "'\n"; return nullptr; }
                if (sym->borrowCount > 0) {
                    std::cerr << "codegen: cannot assign to '" << a->varName << "' while borrowed\n";
                    return nullptr;
                }
                auto* val = emitExpr(a->rhs.get());
                if (!val) return nullptr;
                if (a->rhs->kind == NodeKind::Variable) {
                    auto* varRhs = static_cast<VariableExprAST*>(a->rhs.get());
                    auto* srcSym = symTable.lookup(varRhs->name);
                    if (srcSym && !srcSym->type.isCopyType()) {
                        if (srcSym->borrowCount > 0) {
                            std::cerr << "codegen: cannot move '" << varRhs->name << "' while borrowed\n";
                            return nullptr;
                        }
                        srcSym->moved = true;
                    }
                }
                auto* store = builder->CreateStore(val, sym->alloca);
                if (sym->moved) sym->moved = false;
                return store;
            }
            case NodeKind::Binary: {
                auto* b = static_cast<BinaryExprAST*>(node);
                auto* l = emitExpr(b->lhs.get());
                auto* r = emitExpr(b->rhs.get());
                if (!l || !r) return nullptr;
                if (b->op == '+' || b->op == '-' || b->op == '*') {
                    llvm::Intrinsic::ID iid;
                    switch (b->op) {
                        case '+': iid = llvm::Intrinsic::sadd_with_overflow; break;
                        case '-': iid = llvm::Intrinsic::ssub_with_overflow; break;
                        default:  iid = llvm::Intrinsic::smul_with_overflow; break;
                    }
                    auto* ovFn = llvm::Intrinsic::getOrInsertDeclaration(mod.get(), iid, {i64Ty});
                    auto* callRes = builder->CreateCall(ovFn, {l, r}, "arith_ov");
                    auto* val = builder->CreateExtractValue(callRes, {0}, "arith_val");
                    auto* ov = builder->CreateExtractValue(callRes, {1}, "arith_ovfl");
                    auto* okBB = llvm::BasicBlock::Create(*ctx, "arith_ok", currentFunc);
                    auto* panicBB = llvm::BasicBlock::Create(*ctx, "arith_panic", currentFunc);
                    builder->CreateCondBr(ov, panicBB, okBB);
                    builder->SetInsertPoint(panicBB);
                    auto* panicFn = functionMap["flint_panic"];
                    if (panicFn) {
                        auto* msg = builder->CreateGlobalString("integer overflow", "ovmsg");
                        builder->CreateCall(panicFn, {msg});
                    }
                    builder->CreateBr(okBB);
                    builder->SetInsertPoint(okBB);
                    return val;
                } else if (b->op == '/') {
                    return builder->CreateSDiv(l, r, "div");
                } else if (b->op == '|') {
                    auto* lBool = builder->CreateICmpNE(l, llvm::ConstantInt::get(i64Ty, 0), "lbool");
                    auto* rBool = builder->CreateICmpNE(r, llvm::ConstantInt::get(i64Ty, 0), "rbool");
                    auto* orVal = builder->CreateOr(lBool, rBool, "or");
                    return builder->CreateZExt(orVal, i64Ty, "or_ext");
                }
                return nullptr;
            }
            case NodeKind::Compare: {
                auto* c = static_cast<CompareExprAST*>(node);
                auto* l = emitExpr(c->lhs.get());
                auto* r = emitExpr(c->rhs.get());
                if (!l || !r) return nullptr;
                llvm::CmpInst::Predicate pred;
                if (c->op == "==") pred = llvm::CmpInst::ICMP_EQ;
                else if (c->op == "!=") pred = llvm::CmpInst::ICMP_NE;
                else if (c->op == "<")  pred = llvm::CmpInst::ICMP_SLT;
                else if (c->op == ">")  pred = llvm::CmpInst::ICMP_SGT;
                else if (c->op == "<=") pred = llvm::CmpInst::ICMP_SLE;
                else if (c->op == ">=") pred = llvm::CmpInst::ICMP_SGE;
                else return nullptr;
                auto* cmp = builder->CreateICmp(pred, l, r, "cmp");
                return builder->CreateZExt(cmp, i64Ty, "cmp_ext");
            }
            case NodeKind::Call:   return emitCall(static_cast<CallExprAST*>(node));
            case NodeKind::Array:  return emitArrayLiteral(static_cast<ArrayExprAST*>(node));
            case NodeKind::Index:  return emitIndexAccess(static_cast<IndexExprAST*>(node));
            case NodeKind::Ref:    return emitRef(static_cast<RefExprAST*>(node));
            case NodeKind::Deref:  return emitDeref(static_cast<DerefExprAST*>(node));
            case NodeKind::StructLiteral: return emitStructLiteral(static_cast<StructLiteralAST*>(node));
            case NodeKind::FieldAccess:   return emitFieldAccess(static_cast<FieldAccessAST*>(node));
            case NodeKind::EnumConstruct: return emitEnumConstruct(static_cast<EnumConstructAST*>(node));
            case NodeKind::Match:  return emitMatch(static_cast<MatchExprAST*>(node));
            default: return nullptr;
        }
    }

    void emitVarDecl(VarDeclAST* decl) {
        Type rt = resolveType(decl->varType);
        llvm::Type* lty = llvmType(rt, *ctx, mod.get());
        llvm::AllocaInst* alloca = createEntryAlloca(lty, decl->varName);
        symTable.declare(decl->varName, {decl->varType, alloca, nullptr, decl->isMutable, false, 0});
        // Move: emit init first, then mark source as moved
        auto* init = emitExpr(decl->init.get());
        if (decl->init->kind == NodeKind::Variable) {
            auto* varInit = static_cast<VariableExprAST*>(decl->init.get());
            auto* srcSym = symTable.lookup(varInit->name);
            if (srcSym && !srcSym->type.isCopyType()) {
                if (srcSym->borrowCount > 0) {
                    std::cerr << "codegen: cannot move '" << varInit->name << "' while borrowed\n";
                    return;
                }
                srcSym->moved = true;
            }
        }
        if (init) builder->CreateStore(init, alloca);
    }

    void emitReturn(ReturnStmtAST* ret) {
        if (currentFunc && currentFunc->getName() == "main" && hasPython) {
            auto it = functionMap.find("flint_py_fini");
            if (it != functionMap.end()) builder->CreateCall(it->second, {});
        }
        if (ret->value) {
            auto* v = emitExpr(ret->value.get());
            if (v) {
                if (currentFunc && currentFunc->getName() == "main") {
                    auto* tr = builder->CreateTrunc(v, i32Ty, "mainret");
                    builder->CreateRet(tr);
                } else {
                    builder->CreateRet(v);
                }
            }
        } else if (currentFunc && currentFunc->getName() == "main") {
            builder->CreateRet(llvm::ConstantInt::get(i32Ty, 0));
        } else {
            builder->CreateRetVoid();
        }
    }

    void emitIf(IfStmtAST* ifs) {
        auto* cond = emitExpr(ifs->condition.get());
        if (!cond) return;
        auto* condVal = builder->CreateICmpNE(cond, llvm::ConstantInt::get(i64Ty, 0), "ifcond");

        auto* thenBB = llvm::BasicBlock::Create(*ctx, "then", currentFunc);
        auto* elseBB = ifs->elseBlock ? llvm::BasicBlock::Create(*ctx, "else", currentFunc) : nullptr;
        auto* mergeBB = llvm::BasicBlock::Create(*ctx, "ifend", currentFunc);

        builder->CreateCondBr(condVal, thenBB, elseBB ? elseBB : mergeBB);

        builder->SetInsertPoint(thenBB);
        symTable.enterScope();
        emitStmt(ifs->thenBlock.get());
        symTable.exitScope();
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);

        if (elseBB) {
            builder->SetInsertPoint(elseBB);
            symTable.enterScope();
            emitStmt(ifs->elseBlock.get());
            symTable.exitScope();
            if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);
        }

        builder->SetInsertPoint(mergeBB);
    }

    void emitWhile(WhileStmtAST* ws) {
        auto* condBB = llvm::BasicBlock::Create(*ctx, "while_cond", currentFunc);
        auto* bodyBB = llvm::BasicBlock::Create(*ctx, "while_body", currentFunc);
        auto* endBB = llvm::BasicBlock::Create(*ctx, "while_end", currentFunc);

        builder->CreateBr(condBB);

        builder->SetInsertPoint(condBB);
        auto* cond = emitExpr(ws->condition.get());
        if (!cond) { builder->SetInsertPoint(endBB); return; }
        auto* condVal = builder->CreateICmpNE(cond, llvm::ConstantInt::get(i64Ty, 0), "whilecond");
        builder->CreateCondBr(condVal, bodyBB, endBB);

        builder->SetInsertPoint(bodyBB);
        symTable.enterScope();
        breakStack.push_back(endBB);
        emitStmt(ws->body.get());
        breakStack.pop_back();
        symTable.exitScope();
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(condBB);

        builder->SetInsertPoint(endBB);
    }

    void emitBlock(BlockStmtAST* blk) {
        symTable.enterScope();
        for (auto& s : blk->stmts) emitStmt(s.get());
        symTable.exitScope();
    }

    llvm::Value* emitArrayLiteral(ArrayExprAST* arr) {
        size_t count = arr->elements.size();

        // Allocate array data on stack
        llvm::ArrayType* arrDataType = llvm::ArrayType::get(i64Ty, count);
        llvm::AllocaInst* dataAlloca = createEntryAlloca(arrDataType, "arr_data");

        // Store each element
        for (size_t i = 0; i < count; i++) {
            llvm::Value* idxVal = llvm::ConstantInt::get(i64Ty, i);
            llvm::Value* elemPtr = builder->CreateGEP(arrDataType, dataAlloca, {llvm::ConstantInt::get(i64Ty, 0), idxVal}, "arr_elem");
            llvm::Value* val = emitExpr(arr->elements[i].get());
            if (!val) return nullptr;
            builder->CreateStore(val, elemPtr);
        }

        // Get pointer to first element as ptr
        llvm::Value* dataPtr = builder->CreateGEP(arrDataType, dataAlloca, {llvm::ConstantInt::get(i64Ty, 0), llvm::ConstantInt::get(i64Ty, 0)}, "arr_data_ptr");

        // Build struct { ptr, i64 } on stack, then load it
        llvm::Type* structTy = llvmType(Type::array(Type::i64()), *ctx, mod.get());
        llvm::AllocaInst* tempAlloca = createEntryAlloca(structTy, "arr_tmp");
        llvm::Value* ptrField = builder->CreateStructGEP(structTy, tempAlloca, 0);
        builder->CreateStore(dataPtr, ptrField);
        llvm::Value* lenField = builder->CreateStructGEP(structTy, tempAlloca, 1);
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, count), lenField);
        return builder->CreateLoad(structTy, tempAlloca, "arr_val");
    }

    llvm::Value* emitIndexAccess(IndexExprAST* idx) {
        llvm::Value* base = emitExpr(idx->base.get());
        llvm::Value* index = emitExpr(idx->index.get());
        if (!base || !index) return nullptr;

        llvm::Value* dataPtr = builder->CreateExtractValue(base, {0}, "arr_ptr");
        llvm::Value* len = builder->CreateExtractValue(base, {1}, "arr_len");

        // Bounds check
        auto* boundsFn = functionMap["flint_bounds_check"];
        if (boundsFn) builder->CreateCall(boundsFn, {index, len});

        llvm::Value* elemPtr = builder->CreateGEP(i64Ty, dataPtr, index, "arr_elem");
        return builder->CreateLoad(i64Ty, elemPtr, "arr_elem_val");
    }

    llvm::Value* emitRef(RefExprAST* ref) {
        if (ref->target->kind == NodeKind::Variable) {
            auto* var = static_cast<VariableExprAST*>(ref->target.get());
            auto* sym = symTable.lookup(var->name);
            if (!sym) { std::cerr << "codegen: undefined var '" << var->name << "'\n"; return nullptr; }
            if (sym->moved) {
                std::cerr << "codegen: cannot borrow moved variable '" << var->name << "'\n";
                return nullptr;
            }
            sym->borrowCount++;
            symTable.recordBorrow(var->name);
            return sym->alloca;
        }
        std::cerr << "codegen: can only reference variables\n";
        return nullptr;
    }

    llvm::Value* emitDeref(DerefExprAST* deref) {
        llvm::Value* ptr = emitExpr(deref->target.get());
        if (!ptr) return nullptr;
        return builder->CreateLoad(i64Ty, ptr, "deref");
    }

    llvm::Value* emitStructLiteral(StructLiteralAST* sl) {
        auto it = structRegistry.find(sl->structName);
        if (it == structRegistry.end()) {
            std::cerr << "codegen: unknown struct '" << sl->structName << "'\n";
            return nullptr;
        }
        auto& def = it->second;
        llvm::Type* st = llvmType(Type::struct_(sl->structName), *ctx, mod.get());
        if (!st) { std::cerr << "codegen: struct type not found '" << sl->structName << "'\n"; return nullptr; }
        // Build struct via undef + insertvalue for each field
        llvm::Value* s = llvm::UndefValue::get(st);
        for (unsigned i = 0; i < def.fields.size(); i++) {
            bool found = false;
            for (unsigned j = 0; j < sl->fields.size(); j++) {
                if (sl->fields[j].first == def.fields[i].name) {
                    auto* fv = emitExpr(sl->fields[j].second.get());
                    if (!fv) return nullptr;
                    s = builder->CreateInsertValue(s, fv, {i}, sl->structName + "." + def.fields[i].name);
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "codegen: missing field '" << def.fields[i].name << "' in struct literal\n";
                return nullptr;
            }
        }
        return s;
    }

    llvm::Value* emitFieldAccess(FieldAccessAST* fa) {
        llvm::Value* base = emitExpr(fa->base.get());
        if (!base) return nullptr;
        for (auto& [name, def] : structRegistry) {
            auto* st = llvm::StructType::getTypeByName(*ctx, name);
            if (!st) continue;
            if (base->getType() == st) {
                for (unsigned i = 0; i < def.fields.size(); i++) {
                    if (def.fields[i].name == fa->fieldName) {
                        return builder->CreateExtractValue(base, {i}, fa->fieldName);
                    }
                }
                std::cerr << "codegen: struct '" << name << "' has no field '" << fa->fieldName << "'\n";
                return nullptr;
            }
        }
        std::cerr << "codegen: cannot access field '" << fa->fieldName << "' on non-struct type\n";
        return nullptr;
    }

    llvm::Value* emitEnumConstruct(EnumConstructAST* ec) {
        auto it = enumRegistry.find(ec->enumName);
        if (it == enumRegistry.end()) { std::cerr << "codegen: unknown enum '" << ec->enumName << "'\n"; return nullptr; }
        auto& ed = it->second;
        llvm::Type* enumTy = llvmType(Type::enum_(ec->enumName), *ctx, mod.get());
        // Find variant index
        int tag = -1;
        for (size_t i = 0; i < ed.variants.size(); i++) {
            if (ed.variants[i].name == ec->variantName) { tag = (int)i; break; }
        }
        if (tag < 0) { std::cerr << "codegen: unknown variant '" << ec->variantName << "'\n"; return nullptr; }

        auto* alloca = createEntryAlloca(enumTy, ec->enumName + "_tmp");
        // Store tag byte
        auto* tagPtr = builder->CreateStructGEP(enumTy, alloca, 0);
        builder->CreateStore(llvm::ConstantInt::get(i8Ty, tag), tagPtr);
        // Store payload if any
        if (!ec->args.empty()) {
            // Build variant-specific LLVM type: { i8, T1, T2, ... }
            std::vector<llvm::Type*> fldTys = {i8Ty};
            for (auto& pt : ed.variants[tag].payloadTypes)
                fldTys.push_back(llvmType(pt, *ctx, mod.get()));
            auto* varTy = llvm::StructType::get(*ctx, fldTys);
            auto* bcPtr = builder->CreateBitCast(alloca, i8PtrTy);
            for (size_t i = 0; i < ec->args.size(); i++) {
                auto* val = emitExpr(ec->args[i].get());
                if (!val) return nullptr;
                auto* fldPtr = builder->CreateStructGEP(varTy, bcPtr, (unsigned)(i + 1));
                builder->CreateStore(val, fldPtr);
            }
        }
        return builder->CreateLoad(enumTy, alloca, ec->enumName + "_val");
    }

    llvm::Value* emitMatch(MatchExprAST* mn) {
        auto* scrutinee = emitExpr(mn->scrutinee.get());
        if (!scrutinee) return nullptr;
        llvm::Type* enumTy = scrutinee->getType();
        // Store scrutinee in alloca for bitcast access
        auto* alloca = createEntryAlloca(enumTy, "match_scrutinee");
        builder->CreateStore(scrutinee, alloca);

        // Get enum definition from first arm
        if (mn->arms.empty()) return nullptr;
        auto eit = enumRegistry.find(mn->arms[0].enumName);
        if (eit == enumRegistry.end()) { std::cerr << "codegen: unknown enum in match\n"; return nullptr; }
        auto& ed = eit->second;

        // Create BBs: one per arm + merge
        std::vector<llvm::BasicBlock*> armBBs;
        for (auto& arm : mn->arms)
            armBBs.push_back(llvm::BasicBlock::Create(*ctx, arm.variantName, currentFunc));
        auto* mergeBB = llvm::BasicBlock::Create(*ctx, "match_end", currentFunc);

        // Load tag and switch
        auto* tagPtr = builder->CreateStructGEP(enumTy, alloca, 0);
        auto* tag = builder->CreateLoad(i8Ty, tagPtr, "tag");
        auto* switchInst = builder->CreateSwitch(tag, mergeBB, (unsigned)mn->arms.size());
        for (size_t i = 0; i < mn->arms.size(); i++) {
            // Find the actual tag value for this variant
            int armTag = -1;
            for (size_t v = 0; v < ed.variants.size(); v++) {
                if (ed.variants[v].name == mn->arms[i].variantName) { armTag = (int)v; break; }
            }
            if (armTag >= 0)
                switchInst->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(i8Ty, (uint64_t)armTag)), armBBs[i]);
        }

        // Emit each arm
        for (size_t i = 0; i < mn->arms.size(); i++) {
            builder->SetInsertPoint(armBBs[i]);
            auto& arm = mn->arms[i];
            // Bind payload if specified (before body scope so it's visible inside)
            int armTag = -1;
            for (size_t v = 0; v < ed.variants.size(); v++) {
                if (ed.variants[v].name == arm.variantName) { armTag = (int)v; break; }
            }
            if (!arm.bindName.empty() && armTag >= 0 && !ed.variants[armTag].payloadTypes.empty()) {
                auto& vt = ed.variants[armTag];
                std::vector<llvm::Type*> fldTys = {i8Ty};
                for (auto& pt : ed.variants[armTag].payloadTypes)
                    fldTys.push_back(llvmType(pt, *ctx, mod.get()));
                auto* varTy = llvm::StructType::get(*ctx, fldTys);
                auto* bcPtr = builder->CreateBitCast(alloca, i8PtrTy);
                // For single-field payload, bind directly
                if (ed.variants[armTag].payloadTypes.size() == 1) {
                    auto* valPtr = builder->CreateStructGEP(varTy, bcPtr, 1);
                    auto* val = builder->CreateLoad(llvmType(ed.variants[armTag].payloadTypes[0], *ctx, mod.get()), valPtr, arm.bindName);
                    auto* bindAlloca = createEntryAlloca(val->getType(), arm.bindName);
                    builder->CreateStore(val, bindAlloca);
                    symTable.declare(arm.bindName, {vt.payloadTypes[0], bindAlloca, nullptr, false, false, 0});
                }
            }
            emitStmt(arm.body.get());
            if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);
        }

        builder->SetInsertPoint(mergeBB);
        return llvm::ConstantInt::get(i64Ty, 0);
    }

    void emitPythonBlock(PyBlockStmtAST* py) {
        hasPython = true;
        for (auto& code : py->codeStrings) {
            auto* str = builder->CreateGlobalString(code, "pycode");
            auto it = functionMap.find("flint_py_run");
            if (it != functionMap.end()) builder->CreateCall(it->second, {str});
        }
    }

    std::string mangle(const std::string& fnName, const std::vector<Type>& typeArgs) {
        std::string s = fnName;
        for (auto& ta : typeArgs) {
            s += ".";
            switch (ta.kind) {
                case TypeKind::I64: s += "i64"; break;
                case TypeKind::Str: s += "str"; break;
                case TypeKind::Bool: s += "bool"; break;
                case TypeKind::Ref: s += "ref"; break;
                case TypeKind::Array: s += "arr"; break;
                case TypeKind::Struct: s += ta.structName; break;
                case TypeKind::Enum: s += ta.structName; break;
                default: s += "any"; break;
            }
        }
        return s;
    }

    llvm::Function* specialize(FunctionAST* generic, const std::vector<Type>& typeArgs) {
        if (generic->typeParams.size() != typeArgs.size()) return nullptr;
        std::string mangled = mangle(generic->name, typeArgs);

        // Build substitution map
        std::unordered_map<std::string, Type> subst;
        for (size_t i = 0; i < generic->typeParams.size(); i++)
            subst[generic->typeParams[i]] = typeArgs[i];

        // Build LLVM function type with concrete types
        std::vector<llvm::Type*> paramTys;
        for (auto& p : generic->params) {
            Type rt = p.second;
            auto it = subst.find(rt.structName);
            if (rt.kind == TypeKind::TypeParam && it != subst.end()) rt = it->second;
            paramTys.push_back(llvmType(rt, *ctx, mod.get()));
        }
        Type retRt = generic->returnType;
        auto rit = subst.find(retRt.structName);
        if (retRt.kind == TypeKind::TypeParam && rit != subst.end()) retRt = rit->second;
        llvm::Type* retTy = llvmType(retRt, *ctx, mod.get());

        auto fTy = llvm::FunctionType::get(retTy, paramTys, false);
        auto f = llvm::Function::Create(fTy, llvm::Function::ExternalLinkage, mangled, mod.get());
        size_t i = 0;
        for (auto& arg : f->args()) {
            if (i < generic->params.size()) arg.setName(generic->params[i].first);
            i++;
        }
        functionMap[mangled] = f;

        // Emit the function body with substitution active
        auto savedMap = typeSubstMap;
        typeSubstMap = subst;

        llvm::Function* savedFunc = currentFunc;
        auto savedInsertBlock = builder->GetInsertBlock();
        currentFunc = f;
        auto entry = llvm::BasicBlock::Create(*ctx, "entry", f);
        builder->SetInsertPoint(entry);
        symTable.enterScope();

        i = 0;
        for (auto& arg : f->args()) {
            std::string pName = generic->params[i].first;
            Type pType = subst[generic->params[i].second.structName];
            if (generic->params[i].second.kind != TypeKind::TypeParam) pType = generic->params[i].second;
            llvm::AllocaInst* alloca = createEntryAlloca(llvmType(pType, *ctx, mod.get()), pName);
            builder->CreateStore(&arg, alloca);
            symTable.declare(pName, {pType, alloca, nullptr, false, false, 0});
            i++;
        }

        if (generic->body) emitStmt(generic->body.get());
        else if (generic->bodyStart > 0 && generic->bodyEnd > 0 && parser) {
            auto savedPos = parser->getPos();
            parser->setPos(generic->bodyStart);
            auto bodyAst = parser->reparseBlock();
            if (bodyAst) emitStmt(bodyAst.get());
            parser->setPos(generic->bodyEnd);
            parser->setPos(savedPos);
        }

        bool hasRet = retRt.kind != TypeKind::Void;
        if (!builder->GetInsertBlock()->getTerminator()) {
            if (hasRet)
                builder->CreateRet(llvm::ConstantInt::get(retTy, 0));
            else
                builder->CreateRetVoid();
        }

        symTable.exitScope();
        currentFunc = savedFunc;
        typeSubstMap = savedMap;
        if (savedInsertBlock) builder->SetInsertPoint(savedInsertBlock);

        genericCache[mangled] = f;
        return f;
    }

    llvm::Value* emitCall(CallExprAST* call) {
        // Handle generic function call
        auto git = genericFunctions.find(call->callee);
        if (git != genericFunctions.end()) {
            auto* generic = git->second;
            // Infer type args from argument expressions
            std::vector<Type> typeArgs;
            for (size_t ai = 0; ai < generic->typeParams.size(); ai++) {
                // Find the first parameter that uses this type param
                bool found = false;
                for (auto& p : generic->params) {
                    if (p.second.kind == TypeKind::TypeParam && p.second.structName == generic->typeParams[ai]) {
                        // Try to infer from the argument expression
                        if (ai < call->args.size()) {
                            switch (call->args[ai]->kind) {
                                case NodeKind::Number:
                                    typeArgs.push_back(Type::i64()); found = true; break;
                                case NodeKind::String:
                                    typeArgs.push_back(Type::str()); found = true; break;
                                case NodeKind::Variable: {
                                    auto* var = static_cast<VariableExprAST*>(call->args[ai].get());
                                    auto* sym = symTable.lookup(var->name);
                                    if (sym) { typeArgs.push_back(sym->type); found = true; }
                                    break;
                                }
                                case NodeKind::EnumConstruct: {
                                    auto* ec = static_cast<EnumConstructAST*>(call->args[ai].get());
                                    auto eit = enumRegistry.find(ec->enumName);
                                    if (eit != enumRegistry.end()) { typeArgs.push_back(Type::enum_(ec->enumName)); found = true; }
                                    break;
                                }
                                default: break;
                            }
                        }
                        break;
                    }
                }
                if (!found) {
                    // Default to i64
                    typeArgs.push_back(Type::i64());
                }
            }

            std::string mangled = mangle(call->callee, typeArgs);
            llvm::Function* f = nullptr;
            auto cit = genericCache.find(mangled);
            if (cit != genericCache.end()) {
                f = cit->second;
            } else {
                f = specialize(generic, typeArgs);
            }
            if (!f) { std::cerr << "codegen: failed to specialize '" << call->callee << "'\n"; return nullptr; }

            // Emit args and call
            std::vector<llvm::Value*> args;
            for (auto& a : call->args) {
                auto* v = emitExpr(a.get());
                if (!v) return nullptr;
                args.push_back(v);
                if (a->kind == NodeKind::Variable) {
                    auto* varArg = static_cast<VariableExprAST*>(a.get());
                    auto* srcSym = symTable.lookup(varArg->name);
                    if (srcSym && !srcSym->type.isCopyType()) {
                        if (srcSym->borrowCount > 0) {
                            std::cerr << "codegen: cannot move while borrowed\n";
                            return nullptr;
                        }
                        srcSym->moved = true;
                    }
                }
            }
            return builder->CreateCall(f, args);
        }

        if (call->callee == "print") {
            if (call->args.empty()) { std::cerr << "print() needs an arg\n"; return nullptr; }
            auto* arg = emitExpr(call->args[0].get());
            if (!arg) return nullptr;
            if (arg->getType() == i64Ty) {
                auto it = functionMap.find("flint_println_i64");
                if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
            }
            if (arg->getType()->isPointerTy()) {
                auto it = functionMap.find("flint_println_str");
                if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
            }
            return nullptr;
        }

        if (call->callee == "py_eval") {
            hasPython = true;
            if (call->args.size() != 1) { std::cerr << "py_eval() needs one string arg\n"; return nullptr; }
            auto* arg = emitExpr(call->args[0].get());
            if (!arg) return nullptr;
            auto it = functionMap.find("flint_py_eval_int");
            if (it != functionMap.end()) return builder->CreateCall(it->second, {arg});
            return nullptr;
        }

        auto it = functionMap.find(call->callee);
        if (it == functionMap.end()) {
            std::cerr << "codegen: undefined function '" << call->callee << "'\n";
            return nullptr;
        }
        std::vector<llvm::Value*> args;
        for (auto& a : call->args) {
            auto* v = emitExpr(a.get());
            if (!v) return nullptr;
            args.push_back(v);
            // Move: passing a non-Copy variable to a function consumes it
            if (a->kind == NodeKind::Variable) {
                auto* varArg = static_cast<VariableExprAST*>(a.get());
                auto* srcSym = symTable.lookup(varArg->name);
                if (srcSym && !srcSym->type.isCopyType()) {
                    if (srcSym->borrowCount > 0) {
                        std::cerr << "codegen: cannot move '" << varArg->name << "' while borrowed\n";
                        return nullptr;
                    }
                    srcSym->moved = true;
                }
            }
        }
        return builder->CreateCall(it->second, args);
    }
};

// ============================================================================
// INTERFACE SYSTEM — emit and load .flint.bc declaration files
// ============================================================================

static bool emitInterface(ProgramAST& prog, const std::string& outputPath) {
    Codegen cg;
    if (!cg.generateDeclarations(prog)) return false;
    std::error_code ec;
    llvm::raw_fd_ostream out(outputPath, ec);
    if (ec) { std::cerr << "interface error: " << ec.message() << "\n"; return false; }
    llvm::WriteBitcodeToFile(*cg.mod, out);
    out.close();
    return true;
}

static std::unique_ptr<llvm::Module> loadInterface(const std::string& path, llvm::LLVMContext& ctx) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if (!buf) return nullptr;
    auto mod = llvm::parseBitcodeFile(buf.get()->getMemBufferRef(), ctx);
    if (!mod) {
        llvm::handleAllErrors(mod.takeError(), [](const llvm::ErrorInfoBase& e) {
            std::cerr << "interface error: " << e.message() << "\n";
        });
        return nullptr;
    }
    return std::move(*mod);
}

// ============================================================================
// Parser emit-mode method implementations (out-of-line, after Codegen)
// ============================================================================

int Parser::tokPrec(TokenType t) {
    switch (t) {
        case TokenType::PIPE_PIPE: return PREC_OR;
        case TokenType::EQ_EQ: case TokenType::NE:
        case TokenType::LT: case TokenType::GT:
        case TokenType::LE: case TokenType::GE: return PREC_COMPARE;
        case TokenType::PLUS: case TokenType::MINUS: return PREC_TERM;
        case TokenType::STAR: case TokenType::SLASH: return PREC_FACTOR;
        case TokenType::LBRACKET: case TokenType::DOT: return PREC_POSTFIX;
        case TokenType::LPAREN: return PREC_CALL;
        default: return PREC_NONE;
    }
}

llvm::Value* Parser::parseExpressionEmit(int minPrec) {
    if (minPrec == PREC_NONE && check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
        std::string name = peek().lexeme;
        auto* sym = cg->symTable.lookup(name);
        if (sym) {
            if (!sym->isMutable) { advance(); advance(); parseError("cannot assign to immutable '" + name + "'"); return nullptr; }
            if (sym->borrowCount > 0) { advance(); advance(); parseError("cannot assign to '" + name + "' while borrowed"); return nullptr; }
            advance(); advance();
            auto val = parseExpressionEmit();
            if (!val) return nullptr;
            auto* store = cg->builder->CreateStore(val, sym->alloca);
            if (sym->moved) sym->moved = false;
            return store;
        }
        auto git = cg->globalSymTable.find(name);
        if (git != cg->globalSymTable.end()) {
            if (!git->second.isMutable) { advance(); advance(); parseError("cannot assign to immutable global '" + name + "'"); return nullptr; }
            advance(); advance();
            auto val = parseExpressionEmit();
            if (!val) return nullptr;
            return cg->builder->CreateStore(val, git->second.global);
        }
        parseError("variable '" + name + "' not declared");
        return nullptr;
    }
    auto left = parseNudEmit();
    if (!left) return nullptr;

    while (true) {
        TokenType tt = peek().type;
        int p = tokPrec(tt);
        if (p < minPrec) break;

        if (tt == TokenType::LBRACKET) {
            advance();
            auto idx = parseExpressionEmit();
            consume(TokenType::RBRACKET, "expected ']' after index");
            left = emitIndexEmit(left, idx);
        } else if (tt == TokenType::DOT) {
            advance();
            Token fName = consume(TokenType::IDENTIFIER, "expected field name after '.'");
            left = emitFieldAccessEmit(left, fName.lexeme);
            } else if (p > PREC_NONE) {
                Token opTok = advance();
                auto right = parseExpressionEmit(p + 1);
            if (!right) return nullptr;
            left = emitBinaryOpEmit(opTok, left, right);
        } else {
            break;
        }
        if (!left) return nullptr;
    }
    return left;
}

llvm::Value* Parser::parseNudEmit() {
    if (match(TokenType::NUMBER_LITERAL)) {
        return llvm::ConstantInt::get(cg->i64Ty, std::stoll(previous().lexeme));
    }
    if (match(TokenType::STRING_LITERAL)) {
        return cg->builder->CreateGlobalString(previous().lexeme, "str");
    }
    if (match(TokenType::LBRACKET)) {
        std::vector<llvm::Value*> elems;
        if (!check(TokenType::RBRACKET)) {
            do {
                auto e = parseExpressionEmit();
                if (e) elems.push_back(e);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACKET, "expected ']' after array literal");
        return emitArrayLiteralEmit(elems);
    }
    if (match(TokenType::KW_MATCH)) return parseMatchEmit();
    if (match(TokenType::IDENTIFIER)) return parseIdentEmit();
    if (match(TokenType::LPAREN)) {
        auto e = parseExpressionEmit();
        consume(TokenType::RPAREN, "expected ')' after expression");
        return e;
    }
    if (match(TokenType::MINUS)) {
        auto rhs = parseNudEmit();
        if (!rhs) return nullptr;
        auto zero = llvm::ConstantInt::get(cg->i64Ty, 0);
        return emitArithOpEmit('-', zero, rhs);
    }
    if (match(TokenType::AMPERSAND)) {
        if (check(TokenType::IDENTIFIER)) {
            std::string vname = peek().lexeme;
            advance();
            auto* sym = cg->symTable.lookup(vname);
            if (!sym) { parseError("undefined variable '" + vname + "'"); return nullptr; }
            if (sym->moved) { parseError("cannot borrow moved variable '" + vname + "'"); return nullptr; }
            sym->borrowCount++;
            cg->symTable.recordBorrow(vname);
            return sym->alloca;
        }
        parseError("can only reference variables");
        return nullptr;
    }
    if (match(TokenType::STAR)) {
        auto target = parseNudEmit();
        if (!target) return nullptr;
        return cg->builder->CreateLoad(cg->i64Ty, target, "deref");
    }
    parseError("expected expression");
    return nullptr;
}

llvm::Value* Parser::parseIdentEmit() {
    std::string name = previous().lexeme;
    if (enumRegistry.count(name) && check(TokenType::DOT)) {
        advance();
        Token varTok = consume(TokenType::IDENTIFIER, "expected variant name");
        return emitEnumConstructEmit(name, varTok.lexeme);
    }
    if (match(TokenType::LPAREN)) {
        return emitCallEmit(name);
    }
    if (structRegistry.count(name) && match(TokenType::LBRACE)) {
        return emitStructLiteralEmit(name);
    }
    auto* sym = cg->symTable.lookup(name);
    if (sym) {
        if (sym->moved) { parseError("use of moved variable '" + name + "'"); return nullptr; }
        if (sym->borrowCount > 0 && !sym->type.isCopyType()) { parseError("cannot move '" + name + "' while borrowed"); return nullptr; }
        llvm::Value* ptr = sym->alloca ? (llvm::Value*)sym->alloca : (llvm::Value*)sym->global;
        return cg->builder->CreateLoad(cg->resolvedLlvmType(sym->type), ptr, name.c_str());
    }
    auto git = cg->globalSymTable.find(name);
    if (git != cg->globalSymTable.end()) {
        return cg->builder->CreateLoad(cg->resolvedLlvmType(git->second.type), git->second.global, name.c_str());
    }
    parseError("undefined variable '" + name + "'");
    return nullptr;
}

llvm::Value* Parser::emitArithOpEmit(char op, llvm::Value* l, llvm::Value* r) {
    llvm::Intrinsic::ID iid;
    switch (op) {
        case '+': iid = llvm::Intrinsic::sadd_with_overflow; break;
        case '-': iid = llvm::Intrinsic::ssub_with_overflow; break;
        default:  iid = llvm::Intrinsic::smul_with_overflow; break;
    }
    auto* ovFn = llvm::Intrinsic::getOrInsertDeclaration(cg->mod.get(), iid, {cg->i64Ty});
    auto* callRes = cg->builder->CreateCall(ovFn, {l, r}, "arith_ov");
    auto* val = cg->builder->CreateExtractValue(callRes, {0}, "arith_val");
    auto* ov = cg->builder->CreateExtractValue(callRes, {1}, "arith_ovfl");
    auto* okBB = llvm::BasicBlock::Create(*cg->ctx, "arith_ok", cg->currentFunc);
    auto* panicBB = llvm::BasicBlock::Create(*cg->ctx, "arith_panic", cg->currentFunc);
    cg->builder->CreateCondBr(ov, panicBB, okBB);
    cg->builder->SetInsertPoint(panicBB);
    auto* panicFn = cg->functionMap["flint_panic"];
    if (panicFn) {
        auto* msg = cg->builder->CreateGlobalString("integer overflow", "ovmsg");
        cg->builder->CreateCall(panicFn, {msg});
    }
    cg->builder->CreateBr(okBB);
    cg->builder->SetInsertPoint(okBB);
    return val;
}

llvm::Value* Parser::emitBinaryOpEmit(Token opTok, llvm::Value* l, llvm::Value* r) {
    TokenType tt = opTok.type;
    if (tt == TokenType::PLUS || tt == TokenType::MINUS || tt == TokenType::STAR || tt == TokenType::SLASH) {
        if (tt == TokenType::SLASH) return cg->builder->CreateSDiv(l, r, "div");
        return emitArithOpEmit(opTok.lexeme[0], l, r);
    }
    if (tt == TokenType::PIPE_PIPE) {
        auto* lBool = cg->builder->CreateICmpNE(l, llvm::ConstantInt::get(cg->i64Ty, 0), "lbool");
        auto* rBool = cg->builder->CreateICmpNE(r, llvm::ConstantInt::get(cg->i64Ty, 0), "rbool");
        auto* orVal = cg->builder->CreateOr(lBool, rBool, "or");
        return cg->builder->CreateZExt(orVal, cg->i64Ty, "or_ext");
    }
    llvm::CmpInst::Predicate pred;
    std::string& op = opTok.lexeme;
    if (op == "==") pred = llvm::CmpInst::ICMP_EQ;
    else if (op == "!=") pred = llvm::CmpInst::ICMP_NE;
    else if (op == "<")  pred = llvm::CmpInst::ICMP_SLT;
    else if (op == ">")  pred = llvm::CmpInst::ICMP_SGT;
    else if (op == "<=") pred = llvm::CmpInst::ICMP_SLE;
    else if (op == ">=") pred = llvm::CmpInst::ICMP_SGE;
    else { parseError("unknown operator '" + op + "'"); return nullptr; }
    auto* cmp = cg->builder->CreateICmp(pred, l, r, "cmp");
    return cg->builder->CreateZExt(cmp, cg->i64Ty, "cmp_ext");
}

llvm::Value* Parser::emitIndexEmit(llvm::Value* base, llvm::Value* index) {
    llvm::Value* dataPtr = cg->builder->CreateExtractValue(base, {0}, "arr_ptr");
    llvm::Value* len = cg->builder->CreateExtractValue(base, {1}, "arr_len");
    auto* boundsFn = cg->functionMap["flint_bounds_check"];
    if (boundsFn) cg->builder->CreateCall(boundsFn, {index, len});
    llvm::Value* elemPtr = cg->builder->CreateGEP(cg->i64Ty, dataPtr, index, "arr_elem");
    return cg->builder->CreateLoad(cg->i64Ty, elemPtr, "arr_elem_val");
}

llvm::Value* Parser::emitFieldAccessEmit(llvm::Value* base, const std::string& field) {
    for (auto& [name, def] : structRegistry) {
        auto* st = llvm::StructType::getTypeByName(*cg->ctx, name);
        if (!st) continue;
        if (base->getType() == st) {
            for (unsigned i = 0; i < def.fields.size(); i++) {
                if (def.fields[i].name == field)
                    return cg->builder->CreateExtractValue(base, {i}, field);
            }
            parseError("struct '" + name + "' has no field '" + field + "'");
            return nullptr;
        }
    }
    parseError("cannot access field '" + field + "' on non-struct type");
    return nullptr;
}

llvm::Value* Parser::emitArrayLiteralEmit(const std::vector<llvm::Value*>& elems) {
    size_t count = elems.size();
    auto* arrDataType = llvm::ArrayType::get(cg->i64Ty, count);
    auto* dataAlloca = cg->createEntryAlloca(arrDataType, "arr_data");
    for (size_t i = 0; i < count; i++) {
        auto* idxVal = llvm::ConstantInt::get(cg->i64Ty, i);
        auto* elemPtr = cg->builder->CreateGEP(arrDataType, dataAlloca, {llvm::ConstantInt::get(cg->i64Ty, 0), idxVal}, "arr_elem");
        cg->builder->CreateStore(elems[i], elemPtr);
    }
    auto* dataPtr = cg->builder->CreateGEP(arrDataType, dataAlloca, {llvm::ConstantInt::get(cg->i64Ty, 0), llvm::ConstantInt::get(cg->i64Ty, 0)}, "arr_data_ptr");
    auto* structTy = cg->resolvedLlvmType(Type::array(Type::i64()));
    auto* tempAlloca = cg->createEntryAlloca(structTy, "arr_tmp");
    auto* ptrField = cg->builder->CreateStructGEP(structTy, tempAlloca, 0);
    cg->builder->CreateStore(dataPtr, ptrField);
    auto* lenField = cg->builder->CreateStructGEP(structTy, tempAlloca, 1);
    cg->builder->CreateStore(llvm::ConstantInt::get(cg->i64Ty, count), lenField);
    return cg->builder->CreateLoad(structTy, tempAlloca, "arr_val");
}

llvm::Value* Parser::emitStructLiteralEmit(const std::string& name) {
    auto it = structRegistry.find(name);
    if (it == structRegistry.end()) { parseError("unknown struct '" + name + "'"); return nullptr; }
    auto& def = it->second;
    auto* st = cg->resolvedLlvmType(Type::struct_(name));
    if (!st) { parseError("struct type not found '" + name + "'"); return nullptr; }
    llvm::Value* s = llvm::UndefValue::get(st);
    for (unsigned i = 0; i < def.fields.size(); i++) {
        Token fName = consume(TokenType::IDENTIFIER, "expected field name");
        consume(TokenType::COLON, "expected ':'");
        auto* fv = parseExpressionEmit();
        if (!fv) return nullptr;
        s = cg->builder->CreateInsertValue(s, fv, {i}, name + "." + def.fields[i].name);
        if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ',' or '}'");
    }
    consume(TokenType::RBRACE, "expected '}' to close struct literal");
    return s;
}

llvm::Value* Parser::emitEnumConstructEmit(const std::string& enumName, const std::string& variantName) {
    auto eit = enumRegistry.find(enumName);
    if (eit == enumRegistry.end()) { parseError("unknown enum '" + enumName + "'"); return nullptr; }
    auto& ed = eit->second;
    auto* enumTy = cg->resolvedLlvmType(Type::enum_(enumName));
    int tag = -1;
    for (size_t i = 0; i < ed.variants.size(); i++)
        if (ed.variants[i].name == variantName) { tag = (int)i; break; }
    if (tag < 0) { parseError("unknown variant '" + variantName + "'"); return nullptr; }

    std::vector<llvm::Value*> args;
    if (match(TokenType::LPAREN)) {
        if (!check(TokenType::RPAREN)) {
            do { args.push_back(parseExpressionEmit()); } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "expected ')' after variant args");
    }

    auto* alloca = cg->createEntryAlloca(enumTy, enumName + "_tmp");
    auto* tagPtr = cg->builder->CreateStructGEP(enumTy, alloca, 0);
    cg->builder->CreateStore(llvm::ConstantInt::get(cg->i8Ty, tag), tagPtr);
    if (!args.empty()) {
        std::vector<llvm::Type*> fldTys = {cg->i8Ty};
        for (auto& pt : ed.variants[tag].payloadTypes)
            fldTys.push_back(cg->resolvedLlvmType(pt));
        auto* varTy = llvm::StructType::get(*cg->ctx, fldTys);
        auto* bcPtr = cg->builder->CreateBitCast(alloca, cg->i8PtrTy);
        for (size_t i = 0; i < args.size(); i++) {
            auto* fldPtr = cg->builder->CreateStructGEP(varTy, bcPtr, (unsigned)(i + 1));
            cg->builder->CreateStore(args[i], fldPtr);
        }
    }
    return cg->builder->CreateLoad(enumTy, alloca, enumName + "_val");
}

llvm::Value* Parser::emitCallEmit(const std::string& callee) {
    std::vector<llvm::Value*> args;
    if (!check(TokenType::RPAREN)) {
        do {
            auto a = parseExpressionEmit();
            if (a) args.push_back(a);
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "expected ')' after arguments");

    if (callee == "py_eval") {
        cg->hasPython = true;
        if (args.size() != 1) { parseError("py_eval() needs one string arg"); return nullptr; }
        auto it = cg->functionMap.find("flint_py_eval_int");
        if (it != cg->functionMap.end()) return cg->builder->CreateCall(it->second, {args[0]});
        parseError("py_eval_int not declared");
        return nullptr;
    }

    if (callee == "print") {
        if (args.empty()) { parseError("print() needs an arg"); return nullptr; }
        if (args[0]->getType() == cg->i8PtrTy) {
            auto it = cg->functionMap.find("flint_println_str");
            if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {args[0]});
        } else {
            auto it = cg->functionMap.find("flint_println_i64");
            if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {args[0]});
        }
        return nullptr;
    }

    auto fIt = cg->functionMap.find(callee);
    if (fIt != cg->functionMap.end()) {
        return cg->builder->CreateCall(fIt->second, args, callee);
    }
    auto gIt = cg->genericFunctions.find(callee);
    if (gIt != cg->genericFunctions.end()) {
        cg->typeSubstMap.clear();
        auto* genFn = gIt->second;
        for (size_t i = 0; i < genFn->typeParams.size(); i++) {
            if (i < genFn->params.size() && i < args.size()) {
                auto* argTy = args[i]->getType();
                for (auto& [sName, sd] : structRegistry) {
                    auto* st = llvm::StructType::getTypeByName(*cg->ctx, sName);
                    if (st && argTy == st) {
                        cg->typeSubstMap[genFn->typeParams[i]] = Type::struct_(sName);
                        break;
                    }
                }
                auto eit = enumRegistry.begin();
                while (eit != enumRegistry.end()) {
                    auto* enumTy = llvm::StructType::getTypeByName(*cg->ctx, eit->second.name);
                    if (enumTy && argTy == enumTy) {
                        cg->typeSubstMap[genFn->typeParams[i]] = Type::enum_(eit->second.name);
                        break;
                    }
                    ++eit;
                }
                if (argTy == cg->i64Ty) cg->typeSubstMap[genFn->typeParams[i]] = Type::i64();
                if (argTy == cg->i8PtrTy) cg->typeSubstMap[genFn->typeParams[i]] = Type::str();
            }
        }
        std::string instanceKey = callee;
        for (auto& [k, v] : cg->typeSubstMap) instanceKey += ":" + k + "=" + std::to_string((int)v.kind);
        auto cacheIt = cg->genericCache.find(instanceKey);
        if (cacheIt != cg->genericCache.end()) {
            cg->typeSubstMap.clear();
            return cg->builder->CreateCall(cacheIt->second, args, callee);
        }
        {
            std::vector<Type> typeArgs;
            for (auto& tp : genFn->typeParams) {
                auto it = cg->typeSubstMap.find(tp);
                typeArgs.push_back(it != cg->typeSubstMap.end() ? it->second : Type::i64());
            }
            auto* spec = cg->specialize(genFn, typeArgs);
            cg->typeSubstMap.clear();
            if (!spec) return nullptr;
            return cg->builder->CreateCall(spec, args, callee);
        }
    }
    parseError("call to unknown function '" + callee + "'");
    return nullptr;
}

llvm::Value* Parser::parseMatchEmit() {
    auto* scrutinee = parseExpressionEmit();
    if (!scrutinee) return nullptr;
    consume(TokenType::LBRACE, "expected '{' after match expression");
    llvm::Type* enumTy = scrutinee->getType();
    auto* alloca = cg->createEntryAlloca(enumTy, "match_scrutinee");
    cg->builder->CreateStore(scrutinee, alloca);

    struct MatchArmEmit {
        std::string enumName, variantName, bindName;
        size_t bodyStart, bodyEnd;
    };
    std::vector<MatchArmEmit> arms;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        MatchArmEmit arm;
        Token enumTk = consume(TokenType::IDENTIFIER, "expected enum name");
        consume(TokenType::DOT, "expected '.'");
        Token varTk = consume(TokenType::IDENTIFIER, "expected variant name");
        arm.enumName = enumTk.lexeme;
        arm.variantName = varTk.lexeme;
        if (match(TokenType::LPAREN)) {
            Token bind = consume(TokenType::IDENTIFIER, "expected binding name");
            arm.bindName = bind.lexeme;
            consume(TokenType::RPAREN, "expected ')'");
        }
        consume(TokenType::FAT_ARROW, "expected '=>'");
        // Save body region (skip over it)
        arm.bodyStart = pos;
        if (check(TokenType::LBRACE)) {
            skipBlock();
        } else {
            while (!check(TokenType::COMMA) && !check(TokenType::RBRACE) && !isAtEnd())
                advance();
        }
        arm.bodyEnd = pos;
        arms.push_back(std::move(arm));
        if (!check(TokenType::RBRACE)) consume(TokenType::COMMA, "expected ','");
    }
    consume(TokenType::RBRACE, "expected '}' to close match");

    if (arms.empty()) return llvm::ConstantInt::get(cg->i64Ty, 0);
    auto eit = enumRegistry.find(arms[0].enumName);
    if (eit == enumRegistry.end()) { parseError("unknown enum '" + arms[0].enumName + "'"); return nullptr; }
    auto& ed = eit->second;

    std::vector<llvm::BasicBlock*> armBBs;
    for (auto& arm : arms)
        armBBs.push_back(llvm::BasicBlock::Create(*cg->ctx, arm.variantName, cg->currentFunc));
    auto* mergeBB = llvm::BasicBlock::Create(*cg->ctx, "match_end", cg->currentFunc);

    auto* tagPtr = cg->builder->CreateStructGEP(enumTy, alloca, 0);
    auto* tag = cg->builder->CreateLoad(cg->i8Ty, tagPtr, "tag");
    auto* switchInst = cg->builder->CreateSwitch(tag, mergeBB, (unsigned)arms.size());
    for (size_t i = 0; i < arms.size(); i++) {
        int armTag = -1;
        for (size_t v = 0; v < ed.variants.size(); v++)
            if (ed.variants[v].name == arms[i].variantName) { armTag = (int)v; break; }
        if (armTag >= 0)
            switchInst->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(cg->i8Ty, (uint64_t)armTag)), armBBs[i]);
    }

    for (size_t i = 0; i < arms.size(); i++) {
        cg->builder->SetInsertPoint(armBBs[i]);
        auto& arm = arms[i];
        int armTag = -1;
        for (size_t v = 0; v < ed.variants.size(); v++)
            if (ed.variants[v].name == arm.variantName) { armTag = (int)v; break; }
        if (!arm.bindName.empty() && armTag >= 0 && !ed.variants[armTag].payloadTypes.empty()) {
            auto& vt = ed.variants[armTag];
            std::vector<llvm::Type*> fldTys = {cg->i8Ty};
            for (auto& pt : ed.variants[armTag].payloadTypes)
                fldTys.push_back(cg->resolvedLlvmType(pt));
            auto* varTy = llvm::StructType::get(*cg->ctx, fldTys);
            auto* bcPtr = cg->builder->CreateBitCast(alloca, cg->i8PtrTy);
            if (ed.variants[armTag].payloadTypes.size() == 1) {
                auto* valPtr = cg->builder->CreateStructGEP(varTy, bcPtr, 1);
                auto* val = cg->builder->CreateLoad(cg->resolvedLlvmType(ed.variants[armTag].payloadTypes[0]), valPtr, arm.bindName);
                auto* bindAlloca = cg->createEntryAlloca(val->getType(), arm.bindName);
                cg->builder->CreateStore(val, bindAlloca);
                cg->symTable.declare(arm.bindName, {vt.payloadTypes[0], bindAlloca, nullptr, false, false, 0});
            }
        }
        // Re-parse and emit arm body from saved token range
        size_t savedPos = getPos();
        setPos(arm.bodyStart);
        if (check(TokenType::LBRACE)) {
            parseBlockEmit();
        } else {
            cg->symTable.enterScope();
            parseStatementEmit();
            cg->symTable.exitScope();
        }
        setPos(arm.bodyEnd);
        setPos(savedPos);
        if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(mergeBB);
    }
    cg->builder->SetInsertPoint(mergeBB);
    return llvm::ConstantInt::get(cg->i64Ty, 0);
}

void Parser::parseStatementEmit() {
    if (check(TokenType::SEMICOLON)) { advance(); return; }
    if (check(TokenType::KW_RETURN)) { parseReturnEmit(); return; }
    if (check(TokenType::KW_IF))     { parseIfEmit(); return; }
    if (check(TokenType::KW_WHILE))  { parseWhileEmit(); return; }
    if (check(TokenType::KW_BREAK))  { advance(); parseBreakEmit(); return; }
    if (check(TokenType::KW_PYTHON)) { advance(); parsePythonBlockEmit(); return; }
    if (check(TokenType::LBRACE))    { parseBlockEmit(); return; }
    if (check(TokenType::KW_MUT))    { advance(); parseVarDeclEmit(true); return; }
    if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::COLON) {
        parseVarDeclEmit(false); return;
    }
        if (check(TokenType::IDENTIFIER) && peek(1).type == TokenType::ASSIGN) {
            std::string name = peek().lexeme;
            if (cg->symTable.lookup(name) || cg->globalSymTable.count(name)) {
                parseExpressionEmit();
                match(TokenType::SEMICOLON); match(TokenType::NEWLINE);
                return;
            }
        parseVarDeclEmit(false); return;
    }
    parseExpressionStmtEmit();
}

void Parser::parseExpressionStmtEmit() {
    auto val = parseExpressionEmit();
    (void)val;
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseBlockEmit() {
    consume(TokenType::LBRACE, "expected '{'");
    cg->symTable.enterScope();
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        parseStatementEmit();
    }
    consume(TokenType::RBRACE, "expected '}'");
    cg->symTable.exitScope();
}

void Parser::parseVarDeclEmit(bool isMutable) {
    Token nameTok = advance();
    if (cg->symTable.lookup(nameTok.lexeme)) {
        parseError("variable '" + nameTok.lexeme + "' already declared");
    }
    Type varType = Type::i64();
    bool hasTypeAnnotation = match(TokenType::COLON);
    if (hasTypeAnnotation) varType = parseType();
    consume(TokenType::ASSIGN, "expected '=' in variable declaration");
    size_t exprStart = getPos();
    auto initExpr = parseExpressionEmit();
    if (!initExpr) { parseError("expected expression after '='"); return; }
    if (!hasTypeAnnotation) {
        auto* initTy = initExpr->getType();
        if (initTy == cg->i8PtrTy) varType = Type::str();
        else if (initTy->isArrayTy() || initTy->isStructTy()) {
            bool found = false;
            for (auto& [sn, sd] : structRegistry) {
                auto* st = llvm::StructType::getTypeByName(*cg->ctx, sn);
                if (st && initTy == st) { varType = Type::struct_(sn); found = true; break; }
            }
            if (!found) {
                for (auto& [en, ed] : enumRegistry) {
                    auto* et = llvm::StructType::getTypeByName(*cg->ctx, en);
                    if (et && initTy == et) { varType = Type::enum_(en); found = true; break; }
                }
            }
            if (!found) varType = Type::array(Type::i64());
        } else if (initTy == cg->i64Ty) varType = Type::i64();
        else if (initTy->isPointerTy()) varType = Type::ref(Type::i64());
    }
    varTypeMap[nameTok.lexeme] = varType;
    auto* lty = cg->resolvedLlvmType(varType);
    auto* alloca = cg->createEntryAlloca(lty, nameTok.lexeme);
    cg->symTable.declare(nameTok.lexeme, {varType, alloca, nullptr, isMutable, false, 0});
    // Detect move source: if RHS was a single identifier token
    if (getPos() == exprStart + 1 && previous().type == TokenType::IDENTIFIER) {
        std::string srcName = previous().lexeme;
        auto* srcSym = cg->symTable.lookup(srcName);
        if (srcSym && !srcSym->type.isCopyType()) {
            if (srcSym->borrowCount > 0)
                parseError("cannot move '" + srcName + "' while borrowed");
            else
                srcSym->moved = true;
        }
    }
    cg->builder->CreateStore(initExpr, alloca);
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseReturnEmit() {
    advance();
    if (cg->currentFunc && cg->currentFunc->getName() == "main" && cg->hasPython) {
        auto it = cg->functionMap.find("flint_py_fini");
        if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {});
    }
    if (!check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE) && !check(TokenType::RBRACE) && !isAtEnd()) {
        auto val = parseExpressionEmit();
        if (val) {
            if (cg->currentFunc && cg->currentFunc->getName() == "main") {
                auto* tr = cg->builder->CreateTrunc(val, cg->i32Ty, "mainret");
                cg->builder->CreateRet(tr);
            } else {
                cg->builder->CreateRet(val);
            }
        }
    } else if (cg->currentFunc && cg->currentFunc->getName() == "main") {
        cg->builder->CreateRet(llvm::ConstantInt::get(cg->i32Ty, 0));
    } else {
        cg->builder->CreateRetVoid();
    }
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseBreakEmit() {
    if (cg->breakStack.empty()) { parseError("break outside loop"); return; }
    cg->builder->CreateBr(cg->breakStack.back());
    match(TokenType::SEMICOLON);
    match(TokenType::NEWLINE);
}

void Parser::parseIfEmit() {
    advance();
    auto cond = parseExpressionEmit();
    if (!cond) return;
    auto* condVal = cg->builder->CreateICmpNE(cond, llvm::ConstantInt::get(cg->i64Ty, 0), "ifcond");
    auto* thenBB = llvm::BasicBlock::Create(*cg->ctx, "then", cg->currentFunc);
    auto* elseBB = llvm::BasicBlock::Create(*cg->ctx, "else", cg->currentFunc);
    auto* mergeBB = llvm::BasicBlock::Create(*cg->ctx, "ifend", cg->currentFunc);
    cg->builder->CreateCondBr(condVal, thenBB, elseBB);
    cg->builder->SetInsertPoint(thenBB);
    cg->symTable.enterScope();
    parseBlockEmit();
    cg->symTable.exitScope();
    if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(mergeBB);
    cg->builder->SetInsertPoint(elseBB);
    cg->symTable.enterScope();
    if (match(TokenType::KW_ELIF)) {
        parseIfEmit();
        if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(mergeBB);
    } else if (match(TokenType::KW_ELSE)) {
        parseBlockEmit();
        if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(mergeBB);
    } else {
        cg->builder->CreateBr(mergeBB);
    }
    cg->symTable.exitScope();
    cg->builder->SetInsertPoint(mergeBB);
}

void Parser::parseWhileEmit() {
    advance();
    auto* condBB = llvm::BasicBlock::Create(*cg->ctx, "while_cond", cg->currentFunc);
    auto* bodyBB = llvm::BasicBlock::Create(*cg->ctx, "while_body", cg->currentFunc);
    auto* endBB = llvm::BasicBlock::Create(*cg->ctx, "while_end", cg->currentFunc);
    cg->builder->CreateBr(condBB);
    cg->builder->SetInsertPoint(condBB);
    auto cond = parseExpressionEmit();
    if (!cond) { cg->builder->SetInsertPoint(endBB); return; }
    auto* condVal = cg->builder->CreateICmpNE(cond, llvm::ConstantInt::get(cg->i64Ty, 0), "whilecond");
    cg->builder->CreateCondBr(condVal, bodyBB, endBB);
    cg->builder->SetInsertPoint(bodyBB);
    cg->symTable.enterScope();
    cg->breakStack.push_back(endBB);
    parseBlockEmit();
    cg->breakStack.pop_back();
    cg->symTable.exitScope();
    if (!cg->builder->GetInsertBlock()->getTerminator()) cg->builder->CreateBr(condBB);
    cg->builder->SetInsertPoint(endBB);
}

void Parser::parsePythonBlockEmit() {
    cg->hasPython = true;
    consume(TokenType::LBRACE, "expected '{' after 'python'");
    while (check(TokenType::STRING_LITERAL)) {
        auto* strVal = cg->builder->CreateGlobalString(peek().lexeme, "pycode");
        advance();
        auto it = cg->functionMap.find("flint_py_exec");
        if (it != cg->functionMap.end()) cg->builder->CreateCall(it->second, {strVal});
    }
    consume(TokenType::RBRACE, "expected '}' to close python block");
}

std::string Parser::generateGenericInstance(FunctionAST* genFn, const std::string& callee) {
    setCodegen(cg, false);
    skipBodies = false;
    std::string result = "ok";
    cg->typeSubstMap.clear();
    for (size_t i = 0; i < genFn->typeParams.size(); i++) {
        auto it = cg->typeSubstMap.find(genFn->typeParams[i]);
        if (it != cg->typeSubstMap.end()) continue;
        cg->typeSubstMap[genFn->typeParams[i]] = Type::i64();
    }
    auto* oldParser = cg->parser;
    cg->parser = this;
    auto savedPos = getPos();
    setPos(genFn->bodyStart);
    cg->symTable.enterScope();
    for (auto& p : genFn->params) {
        Type pType = cg->resolveType(p.second);
        auto* lty = cg->resolvedLlvmType(p.second);
        auto* alloca = cg->createEntryAlloca(lty, p.first);
        cg->symTable.declare(p.first, {pType, alloca, nullptr, false, false, 0});
    }
    // Use old AST path for generic instantiation
    auto bodyCopy = parseBlock();
    if (bodyCopy) {
        cg->emitStmt(bodyCopy.get());
    }
    cg->symTable.exitScope();
    setPos(savedPos);
    cg->parser = oldParser;
    return result;
}

// ============================================================================
// MAIN
// ============================================================================

static void mergeProgram(std::unique_ptr<ProgramAST>& dest, std::unique_ptr<ProgramAST>& src) {
    for (auto& f : src->functions) dest->functions.push_back(std::move(f));
    for (auto& s : src->structs) dest->structs.push_back(s);
    for (auto& e : src->enums) dest->enums.push_back(e);
    for (auto& ext : src->externs) dest->externs.push_back(ext);
    for (auto& imp : src->imports) dest->imports.push_back(imp);
    for (auto& g : src->globals) dest->globals.push_back(std::move(g));
}

static std::string dirName(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? "." : path.substr(0, pos);
}

static std::string resolveImportPath(const std::string& basePath, const std::string& importName) {
    if (importName.empty()) return importName;
    if (importName[0] == '/') return importName;
    auto dir = dirName(basePath);
    std::string result = dir + "/" + importName;
    if (result.size() < 3 || result.substr(result.size() - 3) != ".fl")
        result += ".fl";
    return result;
}

static void processFile(const std::string& path, std::unique_ptr<ProgramAST>& combined,
                         std::unordered_set<std::string>& seen, std::string& firstError) {
    std::string absPath = path;
    // Simple absolute path resolution (relative to CWD)
    if (absPath.size() > 0 && absPath[0] != '/') {
        absPath = std::string(getenv("PWD") ? getenv("PWD") : ".") + "/" + absPath;
    }
    // Normalize (remove ./ etc) — basic version
    if (seen.count(absPath)) return;
    seen.insert(absPath);

    auto fileBuf = llvm::MemoryBuffer::getFile(path);
    if (!fileBuf) { firstError = "error: cannot open '" + path + "'"; return; }
    llvm::StringRef sourceRef = fileBuf.get()->getBuffer();

    Lexer lexer(sourceRef);
    auto tokens = lexer.tokenize();

    Parser parser(std::move(tokens));
    auto prog = parser.parseProgram();

    if (parser.hadError()) {
        firstError = parser.errorMsg();
        return;
    }

    mergeProgram(combined, prog);

    // Recursively process imports
    for (auto& imp : prog->imports) {
        std::string impPath;
        if (imp.size() > 0 && imp[0] == '/') {
            impPath = imp;
        } else {
            impPath = dirName(path) + "/" + imp;
        }
        // Add .fl extension if not present
        if (impPath.size() < 3 || impPath.substr(impPath.size() - 3) != ".fl") {
            impPath += ".fl";
        }
        processFile(impPath, combined, seen, firstError);
        if (!firstError.empty()) return;
    }
}

int main(int argc, char* argv[]) {
    PROFILE_BEGIN("total");
    if (argc < 3) {
        std::cerr << "usage: flintc <input.fl> <output> [--link \"linker flags\"]\n";
        std::cerr << "       flintc --emit-interface <input.fl> <output.flint.bc>\n";
        std::cerr << "       flintc --use-interface <file.flint.bc> <input.fl> <output> [--use-interface ...]\n";
        std::cerr << "       flintc --parallel <N> <input.fl> <output> [--link ...]\n";
        return 1;
    }

    std::string inputPath;
    std::string outputPath;
    std::string linkFlags;
    bool emitInterfaceMode = false;
    std::vector<std::string> useInterfaces;
    int parallelThreads = 1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--link" && i + 1 < argc) {
            linkFlags = argv[++i];
        } else if (arg == "--emit-interface") {
            emitInterfaceMode = true;
        } else if (arg == "--use-interface" && i + 1 < argc) {
            useInterfaces.push_back(argv[++i]);
        } else if (arg == "--parallel" && i + 1 < argc) {
            int n = atoi(argv[++i]);
            // Clamp to a sane range: at least 1, at most 4x hardware threads
            // (or 64). Prevents resource-exhaustion from `--parallel 999999999`.
            unsigned hw = std::thread::hardware_concurrency();
            int maxThreads = (int)(hw ? hw * 4 : 16);
            if (maxThreads > 64) maxThreads = 64;
            if (n < 1) n = 1;
            if (n > maxThreads) n = maxThreads;
            parallelThreads = n;
        } else if (inputPath.empty()) {
            inputPath = arg;
        } else if (outputPath.empty()) {
            outputPath = arg;
        }
    }

    if (inputPath.empty() || outputPath.empty()) {
        std::cerr << "usage: flintc <input.fl> <output> [--link \"flags\"] [--emit-interface] [--use-interface <file>] [--parallel <N>]\n";
        return 1;
    }

    // --emit-interface mode: parse declarations only, emit .flint.bc
    if (emitInterfaceMode) {
        auto fileBuf = llvm::MemoryBuffer::getFile(inputPath);
        if (!fileBuf) { std::cerr << "error: cannot open '" << inputPath << "'\n"; return 1; }
        llvm::StringRef sourceRef = fileBuf.get()->getBuffer();
        Lexer lexer(sourceRef);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        parser.setSkipBodies(true);
        auto combined = parser.parseProgram();
        if (parser.hadError()) { std::cerr << parser.errorMsg() << "\n"; return 1; }
        if (!emitInterface(*combined, outputPath)) {
            std::cerr << "error: failed to emit interface\n";
            return 1;
        }
        PROFILE_END(); // total
        PROFILE_REPORT();
        return 0;
    }

    bool emitLl = outputPath.size() >= 3 && outputPath.substr(outputPath.size() - 3) == ".ll";
    bool emitObj = outputPath.size() >= 2 && outputPath.substr(outputPath.size() - 2) == ".o";

    // Phase 1: Read, lex, and parse declarations for the main file
    PROFILE_BEGIN("file_io");
    auto fileBuf = llvm::MemoryBuffer::getFile(inputPath);
    if (!fileBuf) { std::cerr << "error: cannot open '" << inputPath << "'\n"; return 1; }
    llvm::StringRef sourceRef = fileBuf.get()->getBuffer();
    PROFILE_END(); // file_io

    // Determine the .o path for codegen
    std::string objectPath;
    if (emitObj) objectPath = outputPath;
    else objectPath = outputPath + ".flint.o";

    // Check content-addressed cache
    uint64_t sourceHash = fnv1a(sourceRef);
    ModuleCache cache;

    // For binary output, check if cached binary exists (skip lex/parse/codegen/link entirely)
    bool emitBinary = !emitLl && !emitObj;

    if (emitBinary && cache.hasBinary(sourceHash)) {
        if (cache.loadBinary(sourceHash, outputPath)) {
            PROFILE_BEGIN("total");
            PROFILE_END(); // total
            PROFILE_REPORT();
            return 0;
        }
    }

    PROFILE_BEGIN("cache_check");
    if (cache.has(sourceHash)) {
        llvm::LLVMContext ctx;
        if (auto cachedMod = cache.load(sourceHash, ctx)) {
            PROFILE_END(); // cache_check
            PROFILE_BEGIN("codegen_output");
            if (emitLl) {
                if (!emitModuleOutput(cachedMod.get(), outputPath)) return 1;
            } else if (emitObj) {
                if (!emitModuleOutput(cachedMod.get(), outputPath)) return 1;
            } else {
                if (!emitModuleOutput(cachedMod.get(), objectPath)) return 1;
                PROFILE_END(); // codegen_output
                PROFILE_BEGIN("link");
                if (!spawnLinker(objectPath, outputPath, linkFlags)) return 1;
                PROFILE_END(); // link
                cache.saveBinary(sourceHash, outputPath);
                unlink(objectPath.c_str());
                PROFILE_END(); // total
                PROFILE_REPORT();
                return 0;
            }
            PROFILE_END(); // codegen_output
            PROFILE_END(); // total
            PROFILE_REPORT();
            return 0;
        }
    }
    PROFILE_END(); // cache_check

    PROFILE_BEGIN("lex");
    Lexer lexer(sourceRef);
    std::vector<Token> tokens = lexer.tokenize();
    PROFILE_END(); // lex

    PROFILE_BEGIN("parse_decls");
    Parser parser(tokens);
    parser.setSkipBodies(true);
    auto combined = parser.parseProgram();
    PROFILE_BEGIN("parse_error_check");
    if (parser.hadError()) {
        std::cerr << parser.errorMsg() << "\n";
        return 1;
    }
    PROFILE_END(); // parse_error_check
    PROFILE_END(); // parse_decls

    PROFILE_BEGIN("imports");
    std::unordered_set<std::string> seen;
    std::string firstError;
    std::mutex importMutex;

    if (parallelThreads > 1 && !combined->imports.empty()) {
        // Parallel import processing for top-level imports only
        // (sub-imports within each file are processed sequentially by processFile)
        ThreadPool pool(parallelThreads);
        for (auto& imp : combined->imports) {
            auto impPath = resolveImportPath(inputPath, imp);
            pool.enqueue([&, impPath] {
                std::string localError;
                std::unordered_set<std::string> localSeen;
                {
                    std::lock_guard<std::mutex> lock(importMutex);
                    if (seen.count(impPath)) return;
                    seen.insert(impPath);
                }
                auto buf = llvm::MemoryBuffer::getFile(impPath);
                if (!buf) {
                    std::lock_guard<std::mutex> lock(importMutex);
                    if (firstError.empty()) firstError = "error: cannot open '" + impPath + "'";
                    return;
                }
                llvm::StringRef src = buf.get()->getBuffer();
                Lexer lex(src);
                auto toks = lex.tokenize();
                Parser p(toks);
                auto prog = p.parseProgram();
                if (p.hadError()) {
                    std::lock_guard<std::mutex> lock(importMutex);
                    if (firstError.empty()) firstError = p.errorMsg();
                    return;
                }
                // Process sub-imports sequentially within this thread
                for (auto& subImp : prog->imports) {
                    auto subPath = resolveImportPath(impPath, subImp);
                    processFile(subPath, prog, localSeen, localError);
                    if (!localError.empty()) break;
                }
                if (!localError.empty()) {
                    std::lock_guard<std::mutex> lock(importMutex);
                    if (firstError.empty()) firstError = localError;
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(importMutex);
                    mergeProgram(combined, prog);
                }
            });
        }
        pool.wait();
        if (!firstError.empty()) {
            std::cerr << firstError << "\n";
            return 1;
        }
    } else {
        // Sequential import processing (original behavior)
        for (auto& imp : combined->imports) {
            auto impPath = resolveImportPath(inputPath, imp);
            processFile(impPath, combined, seen, firstError);
            if (!firstError.empty()) {
                std::cerr << firstError << "\n";
                return 1;
            }
        }
    }
    PROFILE_END(); // imports

    // Phase 2: Codegen with access to Parser for function body re-parsing
    PROFILE_BEGIN("codegen");
    Codegen codegen;
    codegen.setParser(&parser);

    // Generate declarations from the combined AST
    if (!codegen.generateDeclarations(*combined)) {
        PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
        std::cerr << "declaration generation failed\n";
        return 1;
    }

    // Load pre-compiled interfaces (--use-interface)
    // These add declarations NOT in the AST (e.g., pre-compiled libraries)
    for (auto& iface : useInterfaces) {
        auto ifaceMod = loadInterface(iface, *codegen.ctx);
        if (!ifaceMod) { PROFILE_END(); PROFILE_END(); PROFILE_REPORT(); std::cerr << "error: cannot load interface '" << iface << "'\n"; return 1; }
        // Manually copy function declarations from interface module into the main module.
        // We avoid llvm::Linker::linkModules because it can silently drop declarations
        // when the modules have slightly different data layouts (e.g. LLVM version skew).
        for (auto& f : ifaceMod->functions()) {
            std::string name = f.getName().str();
            if (f.isDeclaration() && !codegen.mod->getFunction(name)) {
                auto* newF = llvm::Function::Create(
                    llvm::cast<llvm::FunctionType>(f.getValueType()),
                    llvm::Function::ExternalLinkage, name, codegen.mod.get());
                newF->setAttributes(f.getAttributes());
                newF->setCallingConv(f.getCallingConv());
            }
        }
        for (auto& gv : ifaceMod->globals()) {
            std::string name = gv.getName().str();
            if (!codegen.mod->getGlobalVariable(name)) {
                new llvm::GlobalVariable(*codegen.mod, gv.getValueType(), gv.isConstant(),
                    llvm::GlobalValue::ExternalLinkage, nullptr, name);
            }
        }
    }
    // Sync LLVM declarations into C++ symbol tables for call resolution
    codegen.syncInterfaceSymbols();

    // Emit function bodies and output
    if (!codegen.emitFunctionBodies(*combined)) {
        PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
        std::cerr << "function body emission failed\n";
        return 1;
    }

    if (!emitModuleOutput(codegen.mod.get(), emitLl || emitObj ? outputPath : objectPath)) {
        PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
        std::cerr << "code generation failed\n";
        return 1;
    }
    PROFILE_END(); // codegen

    // For binary output, spawn linker and cache
    if (!emitLl && !emitObj) {
        // Check for main function before linking
        bool hasMain = codegen.functionMap.count("main") > 0;
        if (!hasMain) {
            std::cerr << "warning: no 'main' function — skipping linker, producing .o only\n";
        } else {
            PROFILE_BEGIN("link");
            if (!spawnLinker(objectPath, outputPath, linkFlags)) {
                PROFILE_END(); PROFILE_END(); PROFILE_REPORT();
                std::cerr << "linking failed\n";
                return 1;
            }
            PROFILE_END(); // link
            cache.saveBinary(sourceHash, outputPath);
            unlink(objectPath.c_str());
        }
    }

    // Save to content-addressed cache
    PROFILE_BEGIN("cache_save");
    cache.save(codegen.mod.get(), sourceHash);
    PROFILE_END(); // cache_save

    PROFILE_END(); // total
    PROFILE_REPORT();
    return 0;
}
