# ToyC LLVM Backend Compiler

This project implements a ToyC compiler with the front end copied from the
existing ToyC C++ implementation and an LLVM-based RISC-V32 backend.

## Build

```sh
mingw32-make
```

The build produces `compiler` on Linux and `compiler.exe` on Windows.

## Run

```sh
./compiler < input.tc > output.s
./compiler -opt < input.tc > output.s
```

The compiler reads ToyC source code from standard input and writes RISC-V32
assembly to standard output. Passing `-opt` enables LLVM `-O2` for the generated
LLVM IR before assembly emission.

## LLVM Tool

The backend invokes LLVM clang as the RISC-V code generator. It first checks the
`TOYC_CLANG` environment variable, then falls back to `clang` on Linux and:

```text
C:\Program Files\LLVM\bin\clang.exe
```
