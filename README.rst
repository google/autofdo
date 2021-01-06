
1. Install prerequisite
***********************
sudo apt install libunwind-dev libgflags-dev libssl-dev libelf-dev protobuf-compiler

Autofdo tool depends on llvm. Currently it needs llvm major version >= 10. You can either install llvm using command like "sudo apt install llvm-10", or you can build the latest llvm from source.

2. Commands
***********
2.1 If build llvm from source
  - git clone https://github.com/llvm/llvm-project.git
  - mkdir build
  - cd build
  - cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON -DBUILD_SHARED_LIBS=OFF -DLLVM_PARALLEL_LINK_JOBS=1 -DLLVM_INCLUDE_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/path/to/llvm/install -DLLVM_ENABLE_PROJECTS="clang"
  - ninja
  - ninja install

2.2 Build autofdo tools
  - git clone --recursive --branch internal-sync https://github.com/shenhanc78/autofdo.git
  - cd autofdo
  - mkdir build
  - cd build
  - cmake -G Ninja -DCMAKE_INSTALL_PREFIX=. -DLLVM_PATH=/path/to/llvm/install ../   # Note: "-DCMAKE_INSTALL_PREFIX=." must be used, because there is a bug in the basil cmakelist.
  - ninja
