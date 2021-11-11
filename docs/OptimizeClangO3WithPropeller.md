# How to optimize clang with Propeller

This document describes how to optimize a clang binary with
[Propeller](https://lists.llvm.org/pipermail/llvm-dev/2019-September/135393.html)
code layout optimizations.

There are essentially four steps to optimize any binary with Propeller:

1.  Build the peak optimized binary with the basic block labels sections
    embedded in it using the flag "-fbasic-block-sections=labels". This is used
    to map basic block addresses to the exact LLVM machine basic blocks.
2.  Profile the binary using the
    [perf](https://perf.wiki.kernel.org/index.php/Tutorial) tool to collect
    hardware samples of the LBR (last branch record) event.
3.  Convert the perf samples data to a custom Propeller format using the
    [create_llvm_prof](https://github.com/google/autofdo) tool.
4.  Re-build (essentially re-linking) the clang binary with the Propeller
    profiles to re-order and split basic blocks using
    [basic block sections.](https://clang.llvm.org/docs/UsersManual.html#controlling-code-generation)

In the following sections, we will show how to go about doing each step in
detail. We have uploaded the script to do all the steps mentioned below
[here.](../propeller_optimize_clang.sh)

## Build Trunk LLVM

```bash
$ BASE_PROPELLER_CLANG_DIR=<somepath>
$ PATH_TO_LLVM_SOURCES=${BASE_PROPELLER_CLANG_DIR}/sources
$ PATH_TO_TRUNK_LLVM_BUILD=${BASE_PROPELLER_CLANG_DIR}/trunk_llvm_build
$ PATH_TO_TRUNK_LLVM_INSTALL=${BASE_PROPELLER_CLANG_DIR}/trunk_llvm_install

# Build Trunk LLVM
$ mkdir -p ${PATH_TO_LLVM_SOURCES} && cd ${PATH_TO_LLVM_SOURCES}
$ git clone git@github.com:llvm/llvm-project.git
$ mkdir -p ${PATH_TO_TRUNK_LLVM_BUILD} && cd ${PATH_TO_TRUNK_LLVM_BUILD}
$ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${PATH_TO_TRUNK_LLVM_INSTALL}" \
    -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_RTTI=On -DLLVM_INCLUDE_TESTS=Off \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    ${PATH_TO_LLVM_SOURCES}/llvm-project/llvm
$ ninja install
```

We will use the trunk LLVM compiler to build the baseline and the propeller
optimized clang binaries.

## Build create_llvm_prof tool

The create_llvm_prof tool is a github hosted third party tool that is used to
convert hardware PMU profile samples to a custom format used by Propeller.

```bash
$ PATH_TO_CREATE_LLVM_PROF=${BASE_PROPELLER_CLANG_DIR}/create_llvm_prof
$ mkdir -p ${PATH_TO_CREATE_LLVM_PROF} && cd ${PATH_TO_CREATE_LLVM_PROF}
$ git clone --recursive git@github.com:google/autofdo.git
$ mkdir build && cd build
$ cmake -G Ninja -DCMAKE_INSTALL_PREFIX="." \
    -DCMAKE_C_COMPILER="${PATH_TO_TRUNK_LLVM_INSTALL}/bin/clang" \
    -DCMAKE_CXX_COMPILER="${PATH_TO_TRUNK_LLVM_INSTALL}/bin/clang++" \
    -DLLVM_PATH="${PATH_TO_TRUNK_LLVM_INSTALL}" ../autofdo/
$ ninja
```

## Build Vanilla Baseline clang binary

This binary will be used as the baseline to benchmark against the propeller
optimized clang binary. The baseline clang binary is a "-O3" optimized binary.
To optimize a clang binary built with PGO and ThinLTO is similar and the extra
steps needed to instrument and build the binary is not shown here.

First, we define a set of common cmake flags that we will use to build the
subsequent clang binaries:

```bash
$ COMMON_CMAKE_FLAGS=(
    "-DLLVM_OPTIMIZED_TABLEGEN=On"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DLLVM_TARGETS_TO_BUILD=X86"
    "-DLLVM_ENABLE_PROJECTS=clang"
    "-DCMAKE_C_COMPILER=${PATH_TO_TRUNK_LLVM_BUILD}/bin/clang"
    "-DCMAKE_CXX_COMPILER=${PATH_TO_TRUNK_LLVM_BUILD}/bin/clang++" )
```

The cmake flags to the compiler and linker to build the baseline binary and to
use **lld** as the linker:

```bash
$ BASELINE_CC_LD_CMAKE_FLAGS=(
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld" )
```

We now build the baseline clang binary with the following commands:

```bash
$ PATH_TO_BASELINE_CLANG_BUILD=${BASE_PROPELLER_CLANG_DIR}/baseline_clang_build
$ mkdir -p ${PATH_TO_BASELINE_CLANG_BUILD} && cd ${PATH_TO_BASELINE_CLANG_BUILD}
$ cmake -G Ninja "${COMMON_CMAKE_FLAGS[@]}" "${BASELINE_CC_LD_CMAKE_FLAGS[@]}" \
                 ${PATH_TO_LLVM_SOURCES}/llvm-project/llvm
$ ninja clang
```

## Build Basic Block Labels clang binary

This is the first step towards a Propeller optimized clang. Here, we use the
extra option, **-fbasic-block-sections=labels**, to add a new section to the
binary that maps virtual addresses to machine basic blocks which helps associate
sampled profiled info to the right basic block.

To do this, we modify the flags to CC and LD as:

```bash
$ LABELS_CC_LD_CMAKE_FLAGS=(
    "-DCMAKE_C_FLAGS=-funique-internal-linkage-names -fbasic-block-sections=labels"
    "-DCMAKE_CXX_FLAGS=-funique-internal-linkage-names -fbasic-block-sections=labels"
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld" )
```

and then build the basic block labels clang binary with:

```bash
$ PATH_TO_LABELS_CLANG_BUILD=${BASE_PROPELLER_CLANG_DIR}/labels_clang_build
$ mkdir -p ${PATH_TO_LABELS_CLANG_BUILD} && cd ${PATH_TO_LABELS_CLANG_BUILD}
$ cmake -G Ninja "${COMMON_CMAKE_FLAGS[@]}" "${LABELS_CC_LD_CMAKE_FLAGS[@]}" \
                 ${PATH_TO_LLVM_SOURCES}/llvm-project/llvm
$ ninja clang
```

The option **-funique-internal-linkage-names** uses unique names for internal
linkage functions which improves profile fidelity and helps propeller optimize
binaries better.

## Setup Benchmarking and Profiling Build

The second step towards a Propeller optimized clang is to run the binary and
collect sampled profiles. For the clang benchmark, building clang itself is the
benchmarking. To do this, we will set up a build directory where we can profile/
benchmark different clang binaries.

First, we create a new build directory as follows:

```bash
$ BENCHMARKING_CLANG_BUILD=${BASE_PROPELLER_CLANG_DIR}/benchmarking_clang_build
$ mkdir -p ${BENCHMARKING_CLANG_BUILD} && cd ${BENCHMARKING_CLANG_BUILD}
$ mkdir symlink_to_clang_binary && cd symlink_to_clang_binary
$ CLANG_VERSION=$(sed -Ene 's!^CLANG_EXECUTABLE_VERSION:STRING=(.*)$!\1!p' ${PATH_TO_TRUNK_LLVM_BUILD}/CMakeCache.txt)
$ ln -sf ${PATH_TO_LABELS_CLANG_BUILD}/bin/clang-${CLANG_VERSION} clang
$ ln -sf ${PATH_TO_LABELS_CLANG_BUILD}/bin/clang-${CLANG_VERSION} clang++
```

Notice that the clang binaries in symlink_to_clang_binary is the basic block
labels binary. Use the cmake command to set up the clang binaries in
symlink_to_clang_binary as the builders by modifying these cmake variables:

```bash
$ cd ${BENCHMARKING_CLANG_BUILD}
$ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS=clang \
                 -DCMAKE_C_COMPILER=${BENCHMARKING_CLANG_BUILD}/symlink_to_clang_binary/clang \
                 -DCMAKE_CXX_COMPILER=${BENCHMARKING_CLANG_BUILD}/symlink_to_clang_binary/clang++ \
                 ${PATH_TO_LLVM_SOURCES}/llvm-project/llvm
```

Next, we will collect a profile of this binary by using it to compile some files
(10) as part of building clang. A really short profile is enough and we can do
it with the following commands:

```bash
$ ninja -t commands | head -10 >& ./perf_commands.sh
$ chmod +x ./perf_commands.sh
$ perf record -e cycles:u -j any,u -- ./perf_commands.sh
$ ls perf.data
```

Increasing the number of files compiled improves profile fidelity but increases
the profile size. We have noticed that this is diminishing returns and a short
profile can get most of the gains.

## Convert profiles using create_llvm_prof

Now we have a profile of the basic block labels binary. As the third step
towards building a Propeller optimized clang, we convert it to a custom
propeller format using the create_llvm_prof tool we built earlier:

```bash
$ cd ${BENCHMARKING_CLANG_BUILD}
$ ${PATH_TO_CREATE_LLVM_PROF}/build/create_llvm_prof --format=propeller \
    --binary=${PATH_TO_LABELS_CLANG_BUILD}/bin/clang-${CLANG_VERSION} \
    --profile=perf.data --out=cluster.txt  --propeller_symorder=symorder.txt 2>&1 1>/dev/null
$ ls cluster.txt symorder.txt
```

This would give us two files, *cluster.txt* and *symorder.txt* that would be
used to build a Propeller optimized clang in the final step.

## Build a Propeller Optimized Binary

This is the final step towards building a propeller optimized binary.

Change the following CC and LD cmake flags as follows:

```bash
$ PROPELLER_CC_LD_CMAKE_FLAGS=(
    "-DCMAKE_C_FLAGS=-funique-internal-linkage-names -fbasic-block-sections=list=${BENCHMARKING_CLANG_BUILD}/cluster.txt"
    "-DCMAKE_CXX_FLAGS=-funique-internal-linkage-names -fbasic-block-sections=list=${BENCHMARKING_CLANG_BUILD}/cluster.txt"
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--symbol-ordering-file=${BENCHMARKING_CLANG_BUILD}/symorder.txt -Wl,--no-warn-symbol-ordering -fuse-ld=lld"
    "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--symbol-ordering-file=${BENCHMARKING_CLANG_BUILD}/symorder.txt -Wl,--no-warn-symbol-ordering -fuse-ld=lld"
    "-DCMAKE_MODULE_LINKER_FLAGS=-Wl,--symbol-ordering-file=${BENCHMARKING_CLANG_BUILD}/symorder.txt -Wl,--no-warn-symbol-ordering -fuse-ld=lld" )
```

1.  This basically adds **"-fbasic-block-sections=list=..."** to the compile
    flags which tell the compilation backends to selectively create basic block
    sections for certain groups of basic blocks.
2.  It also adds **"-Wl,--symbol-ordering-file=..."** to the link command to
    reorder the sections.

and then we repeat the exact commands used to build the labels binary but with
the new CC and LD flags:

```bash
$ PATH_TO_PROPELLER_CLANG_BUILD=${BASE_PROPELLER_CLANG_DIR}/propeller_build
$ mkdir -p ${PATH_TO_PROPELLER_CLANG_BUILD} && cd ${PATH_TO_PROPELLER_CLANG_BUILD}
$ cmake -G Ninja "${COMMON_CMAKE_FLAGS[@]}" "${PROPELLER_CC_LD_CMAKE_FLAGS[@]}" \
                 ${PATH_TO_LLVM_SOURCES}/llvm-project/llvm
$ ninja clang
```

Voila!, we have built a Propeller optimized clang binary.

## Measure improvements: Propeller versus Vanilla

We will use the benchmarking build directory to switch the clang binaries and
measure benchmark build time along with hardware events.

First, let's measure the build times using vanilla clang:

```bash
$ cd ${BENCHMARKING_CLANG_BUILD}/symlink_to_clang_binary
$ ln -sf ${PATH_TO_BASELINE_CLANG_BUILD}/bin/clang-${CLANG_VERSION} clang
$ ln -sf ${PATH_TO_BASELINE_CLANG_BUILD}/bin/clang-${CLANG_VERSION} clang++
$ cd ..
$ ninja clean
$ perf stat -r5 -e instructions,cycles,L1-icache-misses,iTLB-misses -- bash -c "ninja -j48 clang && ninja clean"

 Performance counter stats for 'bash -c ninja -j48 clang && ninja clean' (5 runs):

23,631,511,549,135      instructions:u            #    0.83  insn per cycle           ( +-  0.00% )
28,460,866,059,363      cycles:u                                                      ( +-  0.04% )
 1,436,793,481,950      L1-icache-misses:u                                            ( +-  0.04% )
     7,068,554,123      iTLB-misses:u                                                 ( +-  0.09% )

           191.661 +- 0.253 seconds time elapsed  ( +-  0.13% )
```

Then, let's benchmark using Propeller optimized clang:

```bash
$ cd ${BENCHMARKING_CLANG_BUILD}/symlink_to_clang_binary
$ ln -sf ${PATH_TO_PROPELLER_CLANG_BUILD}/bin/clang-${CLANG_VERSION} clang
$ ln -sf ${PATH_TO_PROPELLER_CLANG_BUILD}/bin/clang-${CLANG_VERSION} clang++
$ cd ..
$ ninja clean
$ perf stat -r5 -e instructions,cycles,L1-icache-misses,iTLB-misses -- bash -c "ninja -j48 clang && ninja clean"

 Performance counter stats for 'bash -c ninja -j48 clang && ninja clean' (5 runs):

23,566,560,085,637      instructions:u            #    1.03  insn per cycle           ( +-  0.00% )
22,849,378,796,755      cycles:u                                                      ( +-  0.06% )
   673,408,965,351      L1-icache-misses:u                                            ( +-  0.05% )
     3,210,966,869      iTLB-misses:u                                                 ( +-  0.28% )

           159.223 +- 0.296 seconds time elapsed  ( +-  0.19% )
```

The perf counters show a ~20% reduction in cycles, ~50% reduction in L1 icache
misses and a ~55% reduction in iTLB misses. The dynamic instruction count
reduces by ~0.3% which is mainly due to the reduction in branches as a better
code layout optimization can convert a branch into an implicit fall-through. The
numbers are over 5 runs with very low noise.
