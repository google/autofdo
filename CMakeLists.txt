cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)

project(autofdo)

set (llvm_search_paths
  ${LLVM_PATH}
  ${LLVM_PATH}/lib/cmake
  ${LLVM_PATH}/lib/cmake/llvm
  ${LLVM_PATH}/lib/cmake/clang
)
find_package(LLVM REQUIRED CONFIG
             PATHS ${llvm_search_paths}
             NO_DEFAULT_PATH)

add_subdirectory(third_party/abseil)
add_subdirectory(third_party/glog)
add_subdirectory(third_party/googletest)

add_custom_target(exclude_extlib_tests ALL 
    COMMAND rm -f ${gtest_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${gmock_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${googletest-distribution_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${glog_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${absl_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${RE2_BINARY_DIR}/CTestTestfile.cmake
)

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
)

add_library(sample_reader OBJECT sample_reader.cc)
target_include_directories(sample_reader PUBLIC util)
target_link_libraries(sample_reader absl::base quipper_perf)

add_library(symbol_map OBJECT
  symbol_map.cc
  llvm_profile_reader.cc
  util/symbolize/elf_reader.cc
)
target_include_directories(symbol_map PUBLIC util)
target_link_libraries(symbol_map
  absl::container
  absl::strings
  absl::memory
  absl::flags
  glog
  LLVMProfileData
)

enable_testing()
add_executable(symbol_map_test symbol_map_test.cc)
target_link_libraries(symbol_map_test symbol_map gtest gtest_main)
add_test(NAME example_test COMMAND symbol_map_test)

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
  third_party/perf_data_converter/src/quipper/binary_data_utils.cc
  third_party/perf_data_converter/src/quipper/binary_data_utils.cc
  third_party/perf_data_converter/src/quipper/buffer_reader.cc
  third_party/perf_data_converter/src/quipper/buffer_writer.cc
  third_party/perf_data_converter/src/quipper/compat/log_level.cc
  third_party/perf_data_converter/src/quipper/dso.cc
  third_party/perf_data_converter/src/quipper/file_reader.cc
  third_party/perf_data_converter/src/quipper/file_utils.cc
  third_party/perf_data_converter/src/quipper/perf_data_utils.cc
  third_party/perf_data_converter/src/quipper/perf_serializer.cc
  third_party/perf_data_converter/src/quipper/perf_reader.cc
  third_party/perf_data_converter/src/quipper/perf_parser.cc
  third_party/perf_data_converter/src/quipper/sample_info_reader.cc
  ${PERF_DATA_PROTO_HDR}
  ${PERF_PARSER_OPTIONS_HDR}
  ${PERF_STAT_HDR})
target_include_directories(quipper_perf PRIVATE
  third_party/abseil)
target_include_directories(quipper_perf BEFORE
  PUBLIC
  third_party/perf_data_converter/src
  third_party/perf_data_converter/src/quipper)

add_custom_command(PRE_BUILD
    OUTPUT prepare_cmds
    COMMAND ln -s -f ${CMAKE_HOME_DIRECTORY}/testdata testdata
)
add_custom_target(prepare ALL
    DEPENDS prepare_cmds
)

# when linking against absl, add the following targets:
# target_link_libraries(create_llvm_prof 
# absl::base 
# absl::str_format 
# absl::strings
# LLVMSupport)



