cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)

project(autofdo)

function (config_without_llvm)
  add_subdirectory(third_party/abseil)
  add_subdirectory(third_party/glog)

  include_directories(${LLVM_INCLUDE_DIRS}
    ${CMAKE_HOME_DIRECTORY}
    third_party/glog/src
    third_party/abseil
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    util
    ${PROJECT_BINARY_DIR}
    ${PROJECT_BINARY_DIR}/third_party/glog)

  find_library (LIBELF_LIBRARIES NAMES elf REQUIRED)
  find_library (LIBCRYPTO_LIBRARIES NAMES crypto REQUIRED)

  find_package(Protobuf REQUIRED)
  protobuf_generate_cpp(PERF_DATA_PROTO_CC PERF_DATA_PROTO_HDR third_party/perf_data_converter/src/quipper/perf_data.proto)
  protobuf_generate_cpp(PERF_PARSER_OPTIONS_CC PERF_PARSER_OPTIONS_HDR third_party/perf_data_converter/src/quipper/perf_parser_options.proto)
  protobuf_generate_cpp(PERF_STAT_CC PERF_STAT_HDR third_party/perf_data_converter/src/quipper/perf_stat.proto)
  add_library(perf_data_proto OBJECT ${PERF_DATA_PROTO_CC})
  add_library(perf_parser_options_proto OBJECT ${PERF_PARSER_OPTIONS_CC})
  add_library(perf_stat_proto OBJECT ${PERF_STAT_CC})

  add_library(create_gcov_lib OBJECT
    create_gcov.cc
    gcov.cc
    instruction_map.cc
    legacy_addr2line.cc
    profile.cc
    profile_creator.cc
    profile_writer.cc
    sample_reader.cc
    symbol_map.cc
    util/symbolize/addr2line_inlinestack.cc
    util/symbolize/bytereader.cc
    util/symbolize/functioninfo.cc
    util/symbolize/dwarf2reader.cc
    util/symbolize/dwarf3ranges.cc
    util/symbolize/elf_reader.cc
  )
  add_dependencies(create_gcov_lib perf_data_proto)
  add_dependencies(create_gcov_lib perf_parser_options_proto)
  add_dependencies(create_gcov_lib perf_stat_proto)

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
    third_party/perf_data_converter/src/quipper/perf_buildid.cc
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

  add_executable(create_gcov)
  target_link_libraries(create_gcov
    absl::flags
    absl::flags_parse
    create_gcov_lib
    glog
    quipper_perf
  )

  add_library(dump_gcov_lib OBJECT
    dump_gcov.cc
    gcov.cc
    instruction_map.cc
    profile.cc
    profile_reader.cc
    symbol_map.cc
    util/symbolize/elf_reader.cc
  )
  add_dependencies(dump_gcov_lib perf_data_proto)
  add_dependencies(dump_gcov_lib perf_parser_options_proto)
  add_dependencies(dump_gcov_lib perf_stat_proto)

  add_executable(dump_gcov)
  target_link_libraries(dump_gcov
    absl::flags
    absl::flags_parse
    dump_gcov_lib
    glog
  )
endfunction ()

function (config_with_llvm)
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
    COMMAND rm -f ${absl_BINARY_DIR}/CTestTestfile.cmake)

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

  set (CMAKE_REQUIRED_INCLUDES ${LLVM_INCLUDE_DIRS})
  set (CMAKE_REQUIRED_LIBRARIES LLVMObject)
  check_cxx_source_compiles(
    "
    #include \"llvm/Object/ELFObjectFile.h\"
  
    using llvmELF64LE = llvm::object::ELF64LE;
    using llvmELFFile = llvm::object::ELFFile<llvmELF64LE>;
    using llvmELFObjFile = llvm::object::ELFObjectFile<llvmELF64LE>;

    bool foo(llvmELFObjFile *elf_object) {
      const llvmELFFile &elf_file = elf_object->getELFFile();
      return elf_file.isLE();
    }
    int main() {
      foo(nullptr);
    }
    "
    LLVM_GETELFFILE_RET_REFERENCE
  )
  if (LLVM_GETELFFILE_RET_REFERENCE)
    add_definitions(-DLLVM_GETELFFILE_RET_REFERENCE=${LLVM_GETELFFILE_RET_REFERENCE})
  endif()

  set (CMAKE_REQUIRED_LIBRARIES LLVMProfileData)
  check_cxx_source_compiles(
    "
    #include \"llvm/ProfileData/SampleProfWriter.h\"
  
    void foo(llvm::sampleprof::SampleProfileWriter *sample_profile_writer) {
      llvm::sampleprof::SampleProfileMap ProfileMap;
      sample_profile_writer->write(ProfileMap);
    }
    int main() {
      foo(nullptr);
      return 0;
    }
    "
    LLVM_SAMPLEFDO_SPLIT_CONTEXT
  )
  if (NOT LLVM_SAMPLEFDO_SPLIT_CONTEXT)
    add_definitions(-DLLVM_BEFORE_SAMPLEFDO_SPLIT_CONTEXT)
  endif()

  find_package(Protobuf REQUIRED)
  protobuf_generate_cpp(PERF_DATA_PROTO_CC PERF_DATA_PROTO_HDR third_party/perf_data_converter/src/quipper/perf_data.proto)
  protobuf_generate_cpp(PERF_PARSER_OPTIONS_CC PERF_PARSER_OPTIONS_HDR third_party/perf_data_converter/src/quipper/perf_parser_options.proto)
  protobuf_generate_cpp(PERF_STAT_CC PERF_STAT_HDR third_party/perf_data_converter/src/quipper/perf_stat.proto)
  add_library(perf_data_proto OBJECT ${PERF_DATA_PROTO_CC})
  add_library(perf_parser_options_proto OBJECT ${PERF_PARSER_OPTIONS_CC})
  add_library(perf_stat_proto OBJECT ${PERF_STAT_CC})

  protobuf_generate_cpp(LLVM_PROPELLER_CFG_PROTO_CC LLVM_PROPELLER_CFG_PROTO_HDR llvm_propeller_cfg.proto)
  add_library(llvm_propeller_cfg_proto OBJECT ${LLVM_PROPELLER_CFG_PROTO_CC})

  protobuf_generate_cpp(LLVM_PROPELLER_OPTIONS_PROTO_CC LLVM_PROPELLER_OPTIONS_PROTO_HDR llvm_propeller_options.proto)
  add_library(llvm_propeller_options OBJECT llvm_propeller_options_builder.cc ${LLVM_PROPELLER_OPTIONS_PROTO_CC})
  add_dependencies(llvm_propeller_options quipper_perf)

  add_library(llvm_propeller_mock_whole_program_info OBJECT
    llvm_propeller_mock_whole_program_info.cc)
  add_dependencies(llvm_propeller_mock_whole_program_info llvm_propeller_options)
  add_dependencies(llvm_propeller_mock_whole_program_info llvm_propeller_cfg_proto)
  add_library(create_llvm_prof_object OBJECT create_llvm_prof.cc)
  add_dependencies(create_llvm_prof_object llvm_propeller_options)

  add_library(sample_reader OBJECT sample_reader.cc)
  target_include_directories(sample_reader PUBLIC util)
  target_link_libraries(sample_reader absl::base quipper_perf LLVMObject)
  add_dependencies(sample_reader perf_data_proto)

  add_library(perfdata_reader OBJECT perfdata_reader.cc)
  add_dependencies(perfdata_reader perf_data_proto)
  add_dependencies(perfdata_reader perf_parser_options_proto)
  add_dependencies(perfdata_reader perf_stat_proto)

  add_library(symbol_map OBJECT
    source_info.cc
    symbol_map.cc
    util/symbolize/elf_reader.cc)
  target_include_directories(symbol_map PUBLIC util)
  target_link_libraries(symbol_map
    absl::flat_hash_map
    absl::node_hash_set
    absl::strings
    absl::memory
    absl::flags
    glog
    LLVMCore
    LLVMProfileData)

  add_library(llvm_profile_writer OBJECT
    gcov.cc
    llvm_profile_writer.cc
    profile_writer.cc)
  add_dependencies(llvm_profile_writer quipper_perf)
  target_include_directories(llvm_profile_writer PUBLIC
    libprotobuf
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    util/regexp)
  target_link_libraries(llvm_profile_writer
    absl::flat_hash_map
    absl::node_hash_set
    absl::flags
    symbol_map)
  add_library(llvm_profile_reader OBJECT llvm_profile_reader.cc)
  target_link_libraries(llvm_profile_reader
    absl::flat_hash_map
    absl::node_hash_set
    symbol_map)
  add_library(profile_reader OBJECT profile_reader.cc)

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

  add_executable(profile_diff profile_diff.cc)
  target_link_libraries(profile_diff
    absl::flags_parse
    llvm_profile_reader
    symbol_map)

  add_executable(profile_merger profile_merger.cc)
  target_link_libraries(profile_merger
    absl::flags_parse
    llvm_profile_reader
    llvm_profile_writer
    profile_creator
    profile_reader
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF)

  add_executable(sample_merger sample_merger.cc)
  target_link_libraries(sample_merger
    absl::flags_parse
    llvm_profile_reader
    llvm_profile_writer
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF)

  add_executable(create_llvm_prof)
  target_link_libraries(create_llvm_prof 
    absl::base
    absl::flags
    absl::flags_parse
    absl::status
    absl::str_format
    absl::strings
    create_llvm_prof_object
    glog
    llvm_profile_reader
    llvm_profile_writer
    llvm_propeller_objects
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport)

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
    LLVMDebugInfoDWARF
    LLVMProfileData)
  add_test(NAME llvm_profile_writer_test COMMAND llvm_profile_writer_test)

  add_library(llvm_propeller_objects OBJECT 
    llvm_propeller_cfg.cc
    llvm_propeller_chain_cluster_builder.cc 
    llvm_propeller_code_layout.cc
    llvm_propeller_code_layout_scorer.cc
    llvm_propeller_formatting.cc
    llvm_propeller_node_chain.cc
    llvm_propeller_node_chain_builder.cc
    llvm_propeller_options_builder.cc
    ${LLVM_PROPELLER_OPTIONS_PROTO_CC}
    llvm_propeller_profile_writer.cc
    llvm_propeller_whole_program_info.cc)
  add_dependencies(llvm_propeller_objects llvm_profile_writer)

  add_executable(instruction_map_test addr2line.cc instruction_map.cc instruction_map_test.cc)
  target_link_libraries(instruction_map_test
    gtest
    gtest_main
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF)
  add_test(NAME instruction_map_test COMMAND instruction_map_test)

  add_executable(profile_symbol_list_test profile_symbol_list.cc)
  target_link_libraries(profile_symbol_list_test
    gtest
    gtest_main
    symbol_map)
  add_test(NAME profile_symbol_list_test COMMAND profile_symbol_list_test)

  add_executable(sample_reader_test sample_reader_test.cc)
  target_link_libraries(sample_reader_test
    absl::base
    absl::strings
    glog
    gtest
    gtest_main
    llvm_profile_writer
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMObject
    LLVMDebugInfoDWARF)
  add_test(NAME sample_reader_test COMMAND sample_reader_test)

  add_executable(llvm_propeller_profile_writer_test llvm_propeller_profile_writer_test.cc)
  target_link_libraries(llvm_propeller_profile_writer_test
    absl::base
    absl::flags
    absl::flags_parse
    absl::status
    absl::str_format
    absl::strings
    glog
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_cfg_proto
    llvm_propeller_objects
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport )
  add_test(NAME llvm_propeller_profile_writer_test COMMAND llvm_propeller_profile_writer_test)

  add_executable(llvm_propeller_whole_program_info_test llvm_propeller_whole_program_info_test.cc)
  target_link_libraries(llvm_propeller_whole_program_info_test
    absl::base
    absl::flags
    absl::flags_parse
    absl::status
    absl::str_format
    absl::strings
    glog
    gtest
    gtest_main
    llvm_profile_reader
    llvm_profile_writer
    llvm_propeller_cfg_proto
    llvm_propeller_mock_whole_program_info
    llvm_propeller_objects
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport )
  add_test(NAME llvm_propeller_whole_program_info_test COMMAND llvm_propeller_whole_program_info_test)

  add_executable(llvm_propeller_code_layout_test llvm_propeller_code_layout_test.cc)
  target_link_libraries(llvm_propeller_code_layout_test
    absl::base
    absl::flags
    absl::flags_parse
    absl::status
    absl::str_format
    absl::strings
    glog
    gtest
    gtest_main
    llvm_profile_reader
    llvm_profile_writer
    llvm_propeller_cfg_proto
    llvm_propeller_mock_whole_program_info
    llvm_propeller_objects
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport )
  add_test(NAME llvm_propeller_code_layout_test COMMAND llvm_propeller_code_layout_test)

  add_library(llvm_propeller_cfg OBJECT llvm_propeller_cfg.cc)
  add_library(llvm_propeller_formatting OBJECT llvm_propeller_formatting.cc)
  add_library(llvm_propeller_whole_program_info OBJECT llvm_propeller_whole_program_info.cc)
  add_dependencies(llvm_propeller_whole_program_info llvm_propeller_options)

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
    third_party/perf_data_converter/src/quipper/perf_buildid.cc
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

  add_executable(hot_list_creator hot_list_creator.cc)
  target_link_libraries(hot_list_creator 
    absl::base
    absl::flags
    absl::flags_parse
    absl::status
    absl::str_format
    absl::strings
    glog
    llvm_profile_reader
    llvm_profile_writer
    llvm_propeller_objects
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport)

  add_library(reg_profiler_objs OBJECT reg_profiler.cc)
  add_executable(reg_profiler reg_profiler_main.cc)
  target_link_libraries(reg_profiler
    absl::base
    absl::flags
    absl::flags_parse
    absl::status
    absl::str_format
    absl::strings
    glog
    llvm_profile_reader
    llvm_profile_writer
    reg_profiler_objs
    llvm_propeller_objects
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport
    LLVMTarget
    LLVMMC
    LLVMMCA
    LLVMMCDisassembler
    LLVMX86Info
    LLVMX86Desc
    LLVMX86TargetMCA
    LLVMX86Disassembler)

  add_executable(reg_profiler_test reg_profiler_test.cc)
  target_link_libraries(reg_profiler_test
    absl::base
    absl::flags
    absl::flags_parse
    absl::status
    absl::str_format
    absl::strings
    glog
    gtest
    gtest_main
    llvm_profile_reader
    llvm_profile_writer
    reg_profiler_objs
    llvm_propeller_objects
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport
    LLVMTarget
    LLVMMC
    LLVMMCA
    LLVMMCDisassembler
    LLVMX86Info
    LLVMX86Desc
    LLVMX86TargetMCA
    LLVMX86Disassembler)

  add_custom_command(PRE_BUILD
    OUTPUT prepare_cmds
    COMMAND ln -s -f ${CMAKE_HOME_DIRECTORY}/testdata testdata)
  add_custom_target(prepare ALL
    DEPENDS prepare_cmds)
endfunction()

if (DEFINED LLVM_PATH)
  config_with_llvm()
else ()
  config_without_llvm()
endif ()
enable_testing()
