
1. Install prerequisite
sudo apt install libunwind-dev libgflags-dev

2. commands:

. git clone --recursive git@github.com:shenhanc78/autofdo.git 
. cd autofdo
. git branch --track internal-sync origin/internal-sync
. git checkout internal-sync
. git submodule update
. mkdir build
. cd build
. cmake -G Ninja -DLLVM_PATH=/path/to/llvm/install ../
. ninja

