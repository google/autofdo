
# 1. Install prerequisite
```
    $ sudo apt install libunwind-dev libgflags-dev libssl-dev libelf-dev protobuf-compiler
```

To build autofdo tool for llvm, it needs llvm major version >= 10. You can either install llvm using command like "sudo apt install llvm-10", or you can build the latest llvm from source.

To build autofdo tool for gcc, no llvm installation is needed.

# 2. Commands
## 2.1 Build autofdo tool for llvm
### 2.1.1 If build llvm from source
```
    $ git clone https://github.com/llvm/llvm-project.git
    $ mkdir build
    $ cd build
    $ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON \
        -DBUILD_SHARED_LIBS=OFF -DLLVM_PARALLEL_LINK_JOBS=1 -DLLVM_INCLUDE_TESTS=OFF \
        -DLLVM_ENABLE_RTTI=ON -DCMAKE_INSTALL_PREFIX=/path/to/llvm/install \
        -DLLVM_ENABLE_PROJECTS="clang" ../llvm-project
    $ ninja
    $ ninja install
```

### 2.1.2 Build autofdo tools
```
    $ git clone --recursive https://github.com/google/autofdo.git
    $ cd autofdo
    $ mkdir build
    $ cd build
    $ # Note: "-DCMAKE_INSTALL_PREFIX=." must be used, because there is a bug in the basil cmakelist.
    $ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=. -DLLVM_PATH=/path/to/llvm/install ../
    $ ninja
    $ ninja test
```

## 2.2 Build autofdo tool for gcc
### 2.2.1 Build autofdo tools using system gcc
```
    $ git clone --recursive https://github.com/google/autofdo.git
    $ cd autofdo
    $ mkdir build
    $ cd build
    $ # Note: "-DCMAKE_INSTALL_PREFIX=." must be used, because there is a bug in the basil cmakelist.
    $ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=. ../
    $ ninja
```

### 2.2.2 Build autofdo tools using customized built gcc
1. download gcc tarball, for example, gcc-11.2.0.tar.gz (other versions of gcc are similar), and extract
2. download prerequisites by
```
    $ cd gcc-release-11.2.0
    $ ./contrib/download_prerequisites
```
3. config, build and install gcc
```
    $ cd gcc-release-11.2.0
    $ mkdir build
    $ cd build
    $ ../gcc-releases-gcc-11.2.0/configure \
          --build=x86_64-linux-gnu --host=x86_64-linux-gnu --target=x86_64-linux-gnu \
          --prefix=${HOME}/installs/gcc-11.2.0 \
          --enable-checking=release \
          --enable-languages=c,c++ \
          --disable-multilib \
          --program-suffix=-11.2
    $ make -j24 install
```
4. config autofdo and build:
```
    $ export PATH=${HOME}/installs/gcc-11.2.0/bin:$PATH
    $ CXX=${HOME}/installs/gcc-11.2.0/bin/x86_64-linux-gnu-g++-11.2
    $ CC=${HOME}/installs/gcc-11.2.0/bin/x86_64-linux-gnu-gcc-11.2

    $  mkdir -p build-with-gcc
    $  cd build-with-gcc
    $  cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=. \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" ../autofdo
```
