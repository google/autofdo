cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)

project(autofdo)

set (llvm_search_paths
  ${LLVM_PATH}
  ${LLVM_PATH}/lib/cmake
  ${LLVM_PATH}/lib/cmake/llvm
  ${LLVM_PATH}/lib/cmake/clang)
find_package(LLVM REQUIRED CONFIG
             PATHS ${llvm_search_paths}
             NO_DEFAULT_PATH)
#set (protobuf_BUILD_TESTS OFF)

add_subdirectory(third_party/abseil)
add_subdirectory(third_party/glog)
add_subdirectory(third_party/googletest)

add_custom_target(exclude_extlib_tests ALL 
    COMMAND rm -f ${gtest_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${gmock_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${googletest-distribution_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${glog_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${absl_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${RE2_BINARY_DIR}/CTestTestfile.cmake)

add_definitions(-DHAVE_LLVM=1)
include_directories(${LLVM_INCLUDE_DIRS}
    ${CMAKE_HOME_DIRECTORY}
    third_party/glog/src
    third_party/abseil
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    ${PROJECT_BINARY_DIR}
    ${PROJECT_BINARY_DIR}/third_party/glog
    ${gtest_SOURCE_DIR}/include
    ${gmock_SOURCE_DIR}/include
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    util/regexp)

add_definitions(-DLLVM_VERSION_MAJOR=${LLVM_VERSION_MAJOR})
add_definitions(-DLLVM_VERSION_MINOR=${LLVM_VERSION_MINOR})

add_library(sample_reader OBJECT sample_reader.cc)
target_include_directories(sample_reader PUBLIC util)
target_link_libraries(sample_reader absl::base quipper_perf LLVMObject)

add_library(symbol_map OBJECT
  symbol_map.cc
  util/symbolize/elf_reader.cc)
target_include_directories(symbol_map PUBLIC util)
target_link_libraries(symbol_map
  absl::container
  absl::strings
  absl::memory
  absl::flags
  glog
  LLVMProfileData)

add_library(source_info OBJECT source_info.cc)
target_link_libraries(source_info LLVMCore)
add_library(llvm_profile_writer OBJECT
  gcov.cc
  llvm_profile_writer.cc
  profile_writer.cc)
target_include_directories(llvm_profile_writer PUBLIC
  libprotobuf
  third_party/perf_data_converter/src
  third_party/perf_data_converter/src/quipper
  util/regexp)
target_link_libraries(llvm_profile_writer
  absl::container
  absl::flags
  source_info
  symbol_map)
add_library(llvm_profile_reader OBJECT llvm_profile_reader.cc)
target_link_libraries(llvm_profile_reader
  absl::container
  source_info
  symbol_map)

add_library(profile_creator OBJECT
  addr2line.cc
  instruction_map.cc
  profile.cc
  profile_creator.cc
  profile_symbol_list.cc)
target_include_directories(profile_creator PUBLIC
  third_party/perf_data_converter/src
  third_party/perf_data_converter/src/quipper
  util/regexp)
target_link_libraries(profile_creator
  llvm_profile_writer)

enable_testing()
add_executable(symbol_map_test symbol_map_test.cc)
target_link_libraries(symbol_map_test
  gtest
  gtest_main
  llvm_profile_reader
  symbol_map)
add_test(NAME symbol_map_test COMMAND symbol_map_test)

find_library (LIBELF_LIBRARIES NAMES elf REQUIRED)
find_library (LIBCRYPTO_LIBRARIES NAMES crypto REQUIRED)

add_executable(llvm_profile_reader_test llvm_profile_reader_test.cc)
target_link_libraries(llvm_profile_reader_test
  gtest
  gtest_main
  llvm_profile_reader
  symbol_map)
add_test(NAME llvm_profile_reader_test COMMAND llvm_profile_reader_test)

add_executable(llvm_profile_writer_test llvm_profile_writer_test.cc)
target_include_directories(llvm_profile_writer_test PUBLIC
  libprotobuf
  third_party/perf_data_converter/src
  third_party/perf_data_converter/src/quipper
  util/regexp)
target_link_libraries(llvm_profile_writer_test
  gtest
  gtest_main
  llvm_profile_writer
  profile_creator
  quipper_perf
  sample_reader
  symbol_map
  LLVMDebugInfoDWARF)
add_test(NAME llvm_profile_writer_test COMMAND llvm_profile_writer_test)

add_library(llvm_propeller_cfg OBJECT llvm_propeller_cfg.cc)
add_library(llvm_propeller_formatting OBJECT llvm_propeller_formatting.cc)
add_library(llvm_propeller_whole_program_info OBJECT llvm_propeller_whole_program_info.cc)

find_package(Protobuf REQUIRED)
protobuf_generate_cpp(PERF_DATA_PROTO_CC PERF_DATA_PROTO_HDR third_party/perf_data_converter/src/quipper/perf_data.proto)
protobuf_generate_cpp(PERF_PARSER_OPTIONS_CC PERF_PARSER_OPTIONS_HDR third_party/perf_data_converter/src/quipper/perf_parser_options.proto)
protobuf_generate_cpp(PERF_STAT_CC PERF_STAT_HDR third_party/perf_data_converter/src/quipper/perf_stat.proto)
add_library(perf_data_proto OBJECT ${PERF_DATA_PROTO_CC})

add_library(quipper_perf OBJECT 
  third_party/perf_data_converter/src/quipper/address_mapper.cc
  third_party/perf_data_converter/src/quipper/base/logging.cc
  third_party/perf_data_converter/src/quipper/binary_data_utils.cc
  third_party/perf_data_converter/src/quipper/binary_data_utils.cc
  third_party/perf_data_converter/src/quipper/buffer_reader.cc
  third_party/perf_data_converter/src/quipper/buffer_writer.cc
  third_party/perf_data_converter/src/quipper/compat/log_level.cc
  third_party/perf_data_converter/src/quipper/data_reader.cc
  third_party/perf_data_converter/src/quipper/data_writer.cc
  third_party/perf_data_converter/src/quipper/dso.cc
  third_party/perf_data_converter/src/quipper/file_reader.cc
  third_party/perf_data_converter/src/quipper/file_utils.cc
  third_party/perf_data_converter/src/quipper/huge_page_deducer.cc
  third_party/perf_data_converter/src/quipper/perf_data_utils.cc
  third_party/perf_data_converter/src/quipper/perf_serializer.cc
  third_party/perf_data_converter/src/quipper/perf_reader.cc
  third_party/perf_data_converter/src/quipper/perf_parser.cc
  third_party/perf_data_converter/src/quipper/sample_info_reader.cc
  ${PERF_DATA_PROTO_HDR}
  ${PERF_DATA_PROTO_CC}
  ${PERF_PARSER_OPTIONS_HDR}
  ${PERF_PARSER_OPTIONS_CC}
  ${PERF_STAT_CC}
  ${PERF_STAT_HDR})
target_include_directories(quipper_perf PRIVATE
  third_party/abseil)
target_include_directories(quipper_perf BEFORE
  PUBLIC
  third_party/perf_data_converter/src
  third_party/perf_data_converter/src/quipper)
target_link_libraries(quipper_perf ${Protobuf_LIBRARIES} ${LIBELF_LIBRARIES} ${LIBCRYPTO_LIBRARIES})

add_custom_command(PRE_BUILD
    OUTPUT prepare_cmds
    COMMAND ln -s -f ${CMAKE_HOME_DIRECTORY}/testdata testdata)
add_custom_target(prepare ALL
    DEPENDS prepare_cmds)

# when linking against absl, add the following targets:
# target_link_libraries(create_llvm_prof 
# absl::base 
# absl::str_format 
# absl::strings
# LLVMSupport)



