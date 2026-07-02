#!/data/data/com.termux/files/usr/bin/bash
set -e

CXX=clang++
CXXFLAGS=$(llvm-config --cxxflags | sed 's/-Werror//g')
LDFLAGS="$(llvm-config --ldflags) $(llvm-config --libs) -lc++_shared"

echo "=== Building Flint compiler (flintc) ==="
$CXX $CXXFLAGS -std=c++17 src/main.cpp $LDFLAGS -o flintc

echo "=== Building profiled build (FLINTC_PROFILE) ==="
$CXX $CXXFLAGS -std=c++17 -DFLINTC_PROFILE src/main.cpp $LDFLAGS -o flintc_prof 2>/dev/null && \
  echo "  profiled compiler: ./flintc_prof" || \
  echo "  (skipped)"

echo "=== Building Flint runtime library ==="
clang -c -O2 runtime/runtime.c -o runtime.o
clang -c -O2 -emit-bc runtime/runtime.c -o runtime.bc 2>/dev/null || \
  clang -c -O2 -emit-llvm runtime/runtime.c -o runtime.bc

echo "=== Building Flint Python runtime ==="
PYINC=$(python3-config --includes 2>/dev/null)
if clang -c -O2 $PYINC runtime/pyruntime.c -o pyruntime.o 2>/dev/null; then
    echo "  python runtime: ./pyruntime.o"
else
    echo "  (skipped - Python headers not available)"
    touch pyruntime.o 2>/dev/null || true
fi

echo "=== Building Flint FFI helper ==="
clang -c -O2 examples/ffi_helper.c -o ffi_helper.o
echo "  ffi helper:   ./ffi_helper.o"

echo "=== Build complete ==="
echo "  compiler:     ./flintc"
echo "  profiled:     ./flintc_prof"
echo "  runtime:      ./runtime.o"
