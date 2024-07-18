
# 1. Install prerequisites on Ubuntu 20.04 and 22.04

```
    $ sudo apt install libunwind-dev libgflags-dev libssl-dev libelf-dev protobuf-compiler cmake libzstd-dev clang g++
```

## 1.1 For Ubuntu 20.04 users

The cmake version (3.16.3) on Ubuntu 20.04 LTS is too old to build third_party/llvm, you can upgrade your cmake by following steps in https://askubuntu.com/questions/355565/how-do-i-install-the-latest-version-of-cmake-from-the-command-line

```
    $ sudo apt purge --auto-remove cmake
    $ wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | sudo tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
    $ sudo apt-add-repository 'deb https://apt.kitware.com/ubuntu/ focal main'     
    $ sudo apt update && sudo apt install cmake
```

## 2 Build autofdo tools

```
    $ git clone --recursive --depth 1 https://github.com/google/autofdo.git
    $ cd autofdo
    $ mkdir build
    $ cd build 
    $ # Build LLVM tools for AUtoFDO and Propeller
    $ cmake -DENABLE_TOOL=LLVM -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release ../
    $ # Build autofdo tools
    $ cmake -DENABLE_TOOL=GCOV -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release ../
    $ make -j 4
```

To build with g++ installed from the package repository use `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++` instead in the cmake command above.
