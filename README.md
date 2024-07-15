
# 1. Install prerequisites on Ubuntu 20.04

```
    $ sudo apt install libunwind-dev libgflags-dev libssl-dev libelf-dev protobuf-compiler cmake zstd clang g++
```

## 2 Build autofdo tools using clang-10

```
    $ git clone --recursive --depth 1 https://github.com/google/autofdo.git
    $ git checkout v0.20.1
    $ cd autofdo
    $ mkdir build
    $ cd build 
	$ # To build LLVM Propeller support
    $ cmake -DWITH_LLVM=On -DCMAKE_C_COMPILER=clang-10 -DCMAKE_CXX_COMPILER=clang++-10 -DCMAKE_BUILD_TYPE=Release ../
	$ # Or to build tools for GCC
	$ cmake -DCMAKE_C_COMPILER=clang-10 -DCMAKE_CXX_COMPILER=clang++-10 -DCMAKE_BUILD_TYPE=Release ../
    $ make -j 4
```
To build with g++-9 installed from the package repository use `-DCMAKE_C_COMPILER=gcc-9 -DCMAKE_CXX_COMPILER=g++9` instead in the cmake command above.
