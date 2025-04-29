AutoFDO tools can be built on Ubuntu 20.04, 22.04 or CentOS 9, choose 1a or 1b to install prerequisites.

> [!NOTE]
> The Propeller codebase has moved to its own repository (as of
> 2025Q1). For the most up-to-date version, see
> [google/llvm-propeller](https://github.com/google/llvm-propeller).

# 1a. Install prerequisites on Ubuntu 20.04 and 22.04

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

# 1b. Install prerequisites on CentOS 9

```
    dnf config-manager --set-enabled crb
    dnf install epel-release epel-next-release
    dnf install git cmake ninja-build elfutils-libelf libunwind-devel clang clang-devel clang-libs protobuf-devel protobuf-compiler elfutils-libelf-devel gcc gcc-c++ openssl-devel
```


## 2 Build autofdo tools

Note, "-DBUILD_SHARED=On" is required for CentOS 9, this will build the tools linked with shared libraries. For Ubuntu 20.04 and Ubuntu 22.04, "-DBUILD_SHARED" is optional, when it is not given, this will build the tools linked statically.

```
    $ git clone --recursive --depth 1 https://github.com/google/autofdo.git
    $ cd autofdo
    $ mkdir build
    $ cd build
    $ # Build LLVM tools for AutoFDO and Propeller
    $ cmake -DENABLE_TOOL=LLVM -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=On ../
    $ # Build autofdo tools
    $ cmake -DENABLE_TOOL=GCOV -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=On ../
    $ make -j 4
```

To build with g++ installed from the package repository use `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++` instead in the cmake command above.
