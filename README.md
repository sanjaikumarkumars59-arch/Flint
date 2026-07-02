# Flint Programming Language

A native compiled language on Termux/Android with Rust-like memory safety.

## Quick Start

```bash
# Build the compiler
cd ~/flint
bash build.sh

# Compile and run a Flint program
./flintc examples/hello.fl output.ll
clang output.ll runtime.o -o hello
./hello
```

## Features

- **Compiled to native code** via LLVM IR — no interpreter, no transpiler
- **Immutable by default** — variables are immutable unless declared with `mut`
- **Memory safe** — no null keyword, all variables must be initialized, array bounds checked
- **Minimal syntax** — inspired by Python/Rust

## Syntax

```
# Variable declaration (immutable by default)
x = 5

# Mutable variable
mut y = 10
y = y + 1

# Arithmetic: +, -, *, /
z = x * 2 + y / 3

# Function calls
print("hello")     # string
print(z)           # integer
```

## Building

```bash
# Build the compiler
bash build.sh

# You can also do it manually:
clang++ $(llvm-config --cxxflags) -std=c++17 src/main.cpp \
    $(llvm-config --ldflags) $(llvm-config --libs) -lc++_shared -o flintc
clang -c -O2 runtime/runtime.c -o runtime.o
```

## Pipeline

```
source.fl  ──→  flintc  ──→  output.ll  ──→  clang + runtime.o  ──→  executable
```

## Testing

```bash
./flintc examples/hello.fl output.ll && clang output.ll runtime.o -o hello && ./hello
./flintc examples/basic.fl output.ll && clang output.ll runtime.o -o basic && ./basic
./flintc examples/strings.fl output.ll && clang output.ll runtime.o -o strings && ./strings
./flintc examples/counter.fl output.ll && clang output.ll runtime.o -o counter && ./counter
./flintc examples/comprehensive.fl output.ll && clang output.ll runtime.o -o comprehensive && ./comprehensive
./flintc examples/mut.fl output.ll && clang output.ll runtime.o -o mut && ./mut
```
