
1. Install prerequisite
sudo apt install libunwind-dev libgflags-dev

2. commands:

. git clone --recursive --branch internal-sync git@github.com:shenhanc78/autofdo.git 
. cd autofdo
. mkdir build
. cd build
. cmake -G Ninja -DCMAKE_INSTALL_PREFIX=. -DLLVM_PATH=/path/to/llvm/install ../   # Note: "-DCMAKE_INSTALL_PREFIX=." must be used, because there is a bug in the basil cmakelist.
. ninja

