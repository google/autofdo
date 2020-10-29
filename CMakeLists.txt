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
find_package(Clang REQUIRED CONFIG
             PATHS ${llvm_search_paths}
             NO_DEFAULT_PATH)

add_subdirectory(third_party/abseil)
add_subdirectory(third_party/glog)
add_subdirectory(util/regexp)

add_definitions(-DHAVE_LLVM=1)
include_directories(${LLVM_INCLUDE_DIRS}
    ${CMAKE_HOME_DIRECTORY}
    third_party/glog/src
    third_party/perf_data_converter/src/quipper
    ${PROJECT_BINARY_DIR}
    ${PROJECT_BINARY_DIR}/third_party/glog)

add_library(sample_reader OBJECT sample_reader.cc)
target_include_directories(sample_reader PUBLIC util util/regexp)
target_link_libraries(sample_reader absl::base re2::re2 quipper_perf)

add_library(llvm_propeller_cfg OBJECT llvm_propeller_cfg.cc)
add_library(llvm_propeller_formatting OBJECT llvm_propeller_formatting.cc)

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
target_include_directories(quipper_perf PUBLIC
  third_party/perf_data_converter/src
  third_party/perf_data_converter/src/quipper)

# when linking against absl, add the following targets:
# target_link_libraries(create_llvm_prof 
# absl::base 
# absl::str_format 
# absl::strings
# LLVMSupport)



