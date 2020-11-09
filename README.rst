
1. Install prerequisite
***********************
sudo apt install libunwind-dev libgflags-dev libssl-dev libelf-dev protobuf-compiler

2. Commands
***********

- git clone --recursive --branch internal-sync git@github.com:shenhanc78/autofdo.git    
- cd autofdo
- mkdir build
- cd build
- cmake -G Ninja -DCMAKE_INSTALL_PREFIX=. -DLLVM_PATH=/path/to/llvm/install ../   # Note: "-DCMAKE_INSTALL_PREFIX=." must be used, because there is a bug in the basil cmakelist.
- ninja
