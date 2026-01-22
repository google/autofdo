cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)
set(ABSL_PROPAGATE_CXX_STD on)

project(autofdo)

if (NOT DEFINED BUILD_SHARED)
  set(BUILD_SHARED FALSE)
endif()

if (NOT ${BUILD_SHARED})
  set (Protobuf_USE_STATIC_LIBS TRUE)
endif()

function (execute_perf_protobuf)

  find_package(Protobuf REQUIRED)

  add_library(perf_proto
    third_party/perf_data_converter/src/quipper/perf_data.proto
    third_party/perf_data_converter/src/quipper/perf_parser_options.proto
    third_party/perf_data_converter/src/quipper/perf_stat.proto)
  protobuf_generate(TARGET perf_proto LANGUAGE cpp)
  target_link_libraries(perf_proto PUBLIC protobuf::libprotobuf)

endfunction()

function (build_gcov)
  add_subdirectory(third_party/abseil)
  add_subdirectory(third_party/glog)
  add_subdirectory(third_party/googletest)

  add_custom_target(exclude_extlib_tests ALL
    COMMAND rm -f ${gtest_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${gmock_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${googletest-distribution_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${glog_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${absl_BINARY_DIR}/CTestTestfile.cmake)

  include_directories(${CMAKE_HOME_DIRECTORY}
    third_party/glog/src
    third_party/abseil
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    third_party/googletest/googletest/include
    third_party/googletest/googlemock/include
    util
    ${PROJECT_BINARY_DIR}
    ${PROJECT_BINARY_DIR}/third_party/glog
    ${PROJECT_BINARY_DIR}/third_party/perf_data_converter/src/quipper)

  find_library (LIBELF_LIBRARIES NAMES elf REQUIRED)
  find_library (LIBCRYPTO_LIBRARIES NAMES crypto REQUIRED)

  add_library(addr2line_lib OBJECT
    legacy_addr2line.cc
    util/symbolize/addr2line_inlinestack.cc
    util/symbolize/bytereader.cc
    util/symbolize/functioninfo.cc
    util/symbolize/dwarf2reader.cc
    util/symbolize/dwarf3ranges.cc
    util/symbolize/elf_reader.cc
    util/symbolize/index_helper.cc
  )

  add_library(create_gcov_lib OBJECT
    create_gcov.cc
    gcov.cc
    instruction_map.cc
    profile.cc
    profile_creator.cc
    profile_writer.cc
    sample_reader.cc
    simple_spe_sample_reader.cc
    symbol_map.cc
  )
  target_link_libraries(create_gcov_lib perf_proto)

  add_library(quipper_perf OBJECT
    third_party/perf_data_converter/src/quipper/address_mapper.cc
    third_party/perf_data_converter/src/quipper/arm_spe_decoder.cc
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
    third_party/perf_data_converter/src/quipper/string_utils.cc)
  target_include_directories(quipper_perf PRIVATE
    third_party/abseil)
  target_include_directories(quipper_perf BEFORE
    PUBLIC
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper)
  target_link_libraries(quipper_perf
    perf_proto ${Protobuf_LIBRARIES} ${LIBELF_LIBRARIES} ${LIBCRYPTO_LIBRARIES})

  add_executable(create_gcov)
  target_link_libraries(create_gcov
    absl::flags
    absl::flags_parse
    create_gcov_lib
    addr2line_lib
    glog
    quipper_perf
  )

  add_library(profile_merger_lib OBJECT
    gcov.cc
    instruction_map.cc
    profile_merger.cc
    profile.cc
    profile_reader.cc
    profile_writer.cc
    symbol_map.cc
    util/symbolize/elf_reader.cc
  )
  target_link_libraries(profile_merger_lib perf_proto)

  add_executable(profile_merger)
  target_link_libraries(profile_merger
    absl::flags
    absl::flags_parse
    profile_merger_lib
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
    util/symbolize/elf_reader.cc)
  target_link_libraries(dump_gcov_lib perf_proto)

  add_executable(dump_gcov)
  target_link_libraries(dump_gcov
    absl::flags
    absl::flags_parse
    dump_gcov_lib
    glog)

    add_executable(addr2line_test addr2line_test.cc)
    target_link_libraries(addr2line_test
      absl::base
      gtest
      gtest_main
      absl::flags
      absl::flags_parse
      addr2line_lib
      glog
      quipper_perf)
    add_test(NAME addr2line_test COMMAND addr2line_test)

    add_executable(summary_calculation_test
      summary_calculation_test.cc
      gcov.cc
      profile_reader.cc
      profile_writer.cc
      symbol_map.cc)
    target_link_libraries(summary_calculation_test
      gtest
      gtest_main
      absl::flags
      addr2line_lib
      glog)
    add_test(NAME summary_calculation_test COMMAND summary_calculation_test)

    add_custom_command(PRE_BUILD
      OUTPUT prepare_cmds
      COMMAND ln -s -f ${CMAKE_HOME_DIRECTORY}/testdata)
    add_custom_target(prepare ALL
      DEPENDS prepare_cmds)

endfunction()

function (build_llvm)
  execute_process(COMMAND sh -c "git -C ${CMAKE_HOME_DIRECTORY}/third_party/llvm-project log -1 --format=%H"
     RESULT_VARIABLE clang_version_status
     OUTPUT_VARIABLE CLANG_VERSION_OUTPUT)

  set(CLANG_KNOWN_GIT_COMMIT_HASH "9fde1a498f2dc97a737a3564cb427c6f2a7bfe6c")
  if (clang_version_status)
     message(WARNING "Could not get clang commit hash : Use clang git hash " ${CLANG_KNOWN_GIT_COMMIT_HASH})
  else()
     string(REGEX MATCH "([0-9|a-f]+)" CLANG_GIT_COMMIT_HASH ${CLANG_VERSION_OUTPUT})
     if ("${CLANG_GIT_COMMIT_HASH}" STREQUAL ${CLANG_KNOWN_GIT_COMMIT_HASH})
       message(STATUS "Found known git commit hash of clang (" ${CLANG_GIT_COMMIT_HASH} ") - Success")
     else()
       message(FATAL_ERROR "Unverified git commit hash of llvm source - ${CLANG_GIT_COMMIT_HASH}. Do not modify or update third_party/llvm-project")
     endif()
  endif()

  # Disable GFLAGS and UNWIND for glog, we do not control glog using
  # gflags, nor does call stacks used in log information. And glog
  # does not provide a way to force static link these two libraries.
  set (WITH_GFLAGS OFF)
  set (WITH_UNWIND OFF)

  ### LLVM Configuration
  set (LLVM_INCLUDE_UTILS OFF)
  set (LLVM_INCLUDE_TESTS OFF)
  set (LLVM_INCLUDE_TOOLS OFF)
  set (LLVM_INCLUDE_DOCS OFF)

  set (LLVM_ENABLE_RTTI ON)
  set (LLVM_ENABLE_PROJECTS clang CACHE STRING
    "Semicolon-separated list of projects to build (${LLVM_KNOWN_PROJECTS}), or \"all\".")
  set (LLVM_TARGETS_TO_BUILD X86 AArch64 CACHE STRING
    "Semicolon-separated list of LLVM targets to build, or \"all\".")
  set (LLVM_ENABLE_ZSTD FORCE_ON)
  if (NOT ${BUILD_SHARED})
    set (LLVM_USE_STATIC_ZSTD TRUE CACHE BOOL "use static zstd")
  endif()
  # terminfo is not needed by create_llvm_prof
  set (LLVM_ENABLE_TERMINFO OFF CACHE BOOL "enable terminfo")
  ###

  add_subdirectory(third_party/abseil)
  add_subdirectory(third_party/glog)
  add_subdirectory(third_party/googletest)
  add_subdirectory(third_party/llvm-project/llvm)

  add_custom_target(exclude_extlib_tests ALL
    COMMAND rm -f ${gtest_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${gmock_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${googletest-distribution_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${glog_BINARY_DIR}/CTestTestfile.cmake
    COMMAND rm -f ${absl_BINARY_DIR}/CTestTestfile.cmake)

  add_definitions(-DHAVE_LLVM=1)
  include_directories(${CMAKE_HOME_DIRECTORY}
    third_party/glog/src
    third_party/abseil
    third_party/llvm-project/llvm/include
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    ${PROJECT_BINARY_DIR}
    ${PROJECT_BINARY_DIR}/third_party/glog
    ${PROJECT_BINARY_DIR}/third_party/llvm-project/llvm/include
    ${gtest_SOURCE_DIR}/include
    ${gmock_SOURCE_DIR}/include
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    util/regexp)

  add_library(llvm_propeller_cfg_proto llvm_propeller_cfg.proto)
  protobuf_generate(TARGET llvm_propeller_cfg_proto LANGUAGE cpp)
  target_link_libraries(llvm_propeller_cfg_proto PUBLIC protobuf::libprotobuf)

  add_library(llvm_propeller_options_proto llvm_propeller_options.proto)
  target_link_libraries(llvm_propeller_options_proto PUBLIC
    protobuf::libprotobuf)
  protobuf_generate(
    TARGET llvm_propeller_options_proto
    LANGUAGE cpp)
  add_library(llvm_propeller_options STATIC llvm_propeller_options_builder.cc)
  target_link_libraries(llvm_propeller_options quipper_perf llvm_propeller_options_proto)

  add_library(mini_disassembler OBJECT mini_disassembler.cc)
  target_link_libraries(mini_disassembler
    absl::base
    absl::status
    absl::statusor
    LLVMMC
    LLVMObject
    LLVMSupport
    LLVMTargetParser)
  foreach (tgt ${LLVM_TARGETS_TO_BUILD})
    set( tp ${PROJECT_BINARY_DIR}/third_party/llvm-project/llvm/lib/libLLVM${tgt})
    foreach (tool AsmParser Desc Disassembler Info)
      target_link_libraries(mini_disassembler LLVM${tgt}${tool})
    endforeach()
  endforeach()

  add_library(sample_reader OBJECT sample_reader.cc spe_sample_reader.cc)
  target_include_directories(sample_reader PUBLIC util)
  target_link_libraries(sample_reader absl::base quipper_perf perf_proto LLVMObject)
  add_dependencies(sample_reader llvm_propeller_objects)

  add_library(perfdata_reader OBJECT perfdata_reader.cc)
  target_include_directories(perfdata_reader
    PUBLIC ${PROJECT_BINARY_DIR}/third_party/perf_data_converter/src/quipper)
  target_link_libraries(perfdata_reader PUBLIC perf_proto)

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
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    util/regexp
    ${PROTOBUF_INCLUDE_DIR}
    ${PROJECT_BINARY_DIR}/third_party/perf_data_converter/src/quipper)
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
  add_dependencies(profile_reader LLVMCore)

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
    llvm_profile_writer
    sample_reader)

  add_executable(profile_diff profile_diff.cc)
  target_link_libraries(profile_diff
    absl::flags_parse
    llvm_profile_reader
    symbol_map
    LLVMSupport)

  add_executable(profile_merger profile_merger.cc)
  target_link_libraries(profile_merger
    absl::flags_parse
    llvm_profile_reader
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    profile_creator
    profile_reader
    quipper_perf
    sample_reader
    status_provider
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport)

  add_executable(sample_merger sample_merger.cc)
  target_link_libraries(sample_merger
    absl::flags_parse
    llvm_profile_reader
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    status_provider
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport)

  add_executable(create_llvm_prof create_llvm_prof.cc)
  target_link_libraries(create_llvm_prof
    absl::base
    absl::check
    absl::flags
    absl::flags_parse
    absl::status
    absl::statusor
    absl::str_format
    absl::strings
    glog
    llvm_profile_reader
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    status_provider
    symbol_map
    status_provider
    LLVMDebugInfoDWARF
    LLVMSupport)

  add_executable(symbol_map_test symbol_map_test.cc)
  target_link_libraries(symbol_map_test
    gtest
    gtest_main
    llvm_profile_reader
    symbol_map
    LLVMSupport)
  add_test(NAME symbol_map_test COMMAND symbol_map_test)

  find_library (LIBELF_LIBRARIES NAMES elf REQUIRED)
  find_library (LIBZ_LIBRARIES NAMES z REQUIRED)
  find_library (LIBCRYPTO_LIBRARIES NAMES crypto REQUIRED)

  add_executable(llvm_profile_reader_test llvm_profile_reader_test.cc)
  target_link_libraries(llvm_profile_reader_test
    gtest
    gtest_main
    llvm_profile_reader
    symbol_map
    LLVMSupport)
  add_test(NAME llvm_profile_reader_test COMMAND llvm_profile_reader_test)

  add_executable(llvm_profile_writer_test llvm_profile_writer_test.cc)
  target_include_directories(llvm_profile_writer_test PUBLIC
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    util/regexp)
  target_link_libraries(llvm_profile_writer_test
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    profile_creator
    quipper_perf
    sample_reader
    status_provider
    symbol_map
    LLVMDebugInfoDWARF
    LLVMProfileData
    LLVMSupport)
  add_test(NAME llvm_profile_writer_test COMMAND llvm_profile_writer_test)

  add_library(status_provider OBJECT
    status_provider.cc
    status_consumer_registry.cc)

  add_library(llvm_propeller_perf_data_provider OBJECT
    llvm_propeller_file_perf_data_provider.cc)

  add_library(llvm_propeller_test_objects OBJECT
    llvm_propeller_cfg_testutil.cc
    llvm_propeller_function_cluster_info_matchers.cc
    llvm_propeller_mock_program_cfg_builder.cc)
  target_include_directories(llvm_propeller_test_objects
    PUBLIC ${PROTOBUF_INCLUDE_DIR})
  target_link_libraries(llvm_propeller_test_objects
    llvm_propeller_cfg_proto)

  add_library(llvm_propeller_objects OBJECT
    addr2cu.cc
    branch_aggregation.cc
    branch_frequencies_autofdo_sample.cc
    frequencies_branch_aggregator.cc
    lbr_branch_aggregator.cc
    llvm_propeller_binary_address_mapper.cc
    llvm_propeller_binary_content.cc
    llvm_propeller_cfg.cc
    llvm_propeller_chain_cluster_builder.cc
    llvm_propeller_code_layout.cc
    llvm_propeller_code_layout_scorer.cc
    llvm_propeller_formatting.cc
    llvm_propeller_node_chain.cc
    llvm_propeller_node_chain_assembly.cc
    llvm_propeller_node_chain_builder.cc
    llvm_propeller_perf_branch_frequencies_aggregator.cc
    llvm_propeller_perf_lbr_aggregator.cc
    llvm_propeller_profile_computer.cc
    llvm_propeller_profile_generator.cc
    llvm_propeller_profile_writer.cc
    llvm_propeller_program_cfg.cc
    llvm_propeller_program_cfg_builder.cc
    llvm_propeller_program_cfg_proto_builder.cc
    llvm_propeller_statistics.cc
    llvm_propeller_telemetry_reporter.cc
    spe_tid_pid_provider.cc)
  target_link_libraries(llvm_propeller_objects
    absl::statusor
    llvm_profile_writer
    llvm_propeller_options
    llvm_propeller_cfg_proto
    status_provider
    LLVMDebugInfoDWARF)

  add_executable(instruction_map_test addr2line.cc instruction_map.cc instruction_map_test.cc)
  target_link_libraries(instruction_map_test
    gtest
    gtest_main
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    sample_reader
    status_provider
    symbol_map
    LLVMDebugInfoDWARF
    LLVMSupport)
  add_test(NAME instruction_map_test COMMAND instruction_map_test)

  add_executable(profile_symbol_list_test profile_symbol_list.cc)
  target_link_libraries(profile_symbol_list_test
    gtest
    gtest_main
    symbol_map
    LLVMSupport)
  add_test(NAME profile_symbol_list_test COMMAND profile_symbol_list_test)

  add_executable(sample_reader_test sample_reader_test.cc)
  target_link_libraries(sample_reader_test
    absl::base
    absl::check
    absl::strings
    glog
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    sample_reader
    status_provider
    symbol_map
    LLVMObject
    LLVMDebugInfoDWARF
    LLVMSupport)
  add_test(NAME sample_reader_test COMMAND sample_reader_test)

  add_executable(addr2cu_test addr2cu_test.cc)
  target_link_libraries(addr2cu_test
    absl::base
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME addr2cu_test COMMAND addr2cu_test)

  add_executable(branch_frequencies_autofdo_sample_test branch_frequencies_autofdo_sample_test.cc)
  target_link_libraries(branch_frequencies_autofdo_sample_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME branch_frequencies_autofdo_sample_test COMMAND branch_frequencies_autofdo_sample_test)

  add_executable(lbr_branch_aggregator_test lbr_branch_aggregator_test.cc)
  target_link_libraries(lbr_branch_aggregator_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME lbr_branch_aggregator_test COMMAND lbr_branch_aggregator_test)

  add_executable(llvm_propeller_binary_address_mapper_test llvm_propeller_binary_address_mapper_test.cc)
  target_link_libraries(llvm_propeller_binary_address_mapper_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_binary_address_mapper_test COMMAND llvm_propeller_binary_address_mapper_test)

  add_executable(llvm_propeller_cfg_test llvm_propeller_cfg_test.cc)
  target_link_libraries(llvm_propeller_cfg_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    llvm_propeller_test_objects
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_cfg_test COMMAND llvm_propeller_cfg_test)

  add_executable(llvm_propeller_file_perf_data_provider_test llvm_propeller_file_perf_data_provider_test.cc)
  target_link_libraries(llvm_propeller_file_perf_data_provider_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_file_perf_data_provider_test COMMAND llvm_propeller_file_perf_data_provider_test)

  add_executable(llvm_propeller_perf_branch_frequencies_aggregator_test llvm_propeller_perf_branch_frequencies_aggregator_test.cc)
  target_link_libraries(llvm_propeller_perf_branch_frequencies_aggregator_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_perf_branch_frequencies_aggregator_test COMMAND llvm_propeller_perf_branch_frequencies_aggregator_test)

  add_executable(llvm_propeller_perf_lbr_aggregator_test llvm_propeller_perf_lbr_aggregator_test.cc)
  target_link_libraries(llvm_propeller_perf_lbr_aggregator_test
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_perf_lbr_aggregator_test COMMAND llvm_propeller_perf_lbr_aggregator_test)

  add_executable(llvm_propeller_profile_computer_test llvm_propeller_profile_computer_test.cc)
  target_link_libraries(llvm_propeller_profile_computer_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_profile_computer_test COMMAND llvm_propeller_profile_computer_test)

  add_executable(llvm_propeller_profile_generator_test llvm_propeller_profile_generator_test.cc)
  target_link_libraries(llvm_propeller_profile_generator_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_profile_generator_test COMMAND llvm_propeller_profile_generator_test)

  add_executable(llvm_propeller_program_cfg_builder_test llvm_propeller_program_cfg_builder_test.cc)
  target_link_libraries(llvm_propeller_program_cfg_builder_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_program_cfg_builder_test COMMAND llvm_propeller_program_cfg_builder_test)

  add_executable(llvm_propeller_program_cfg_proto_builder_test llvm_propeller_program_cfg_proto_builder_test.cc)
  target_link_libraries(llvm_propeller_program_cfg_proto_builder_test
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_program_cfg_proto_builder_test COMMAND llvm_propeller_program_cfg_proto_builder_test)

  add_executable(llvm_propeller_program_cfg_test llvm_propeller_program_cfg_test.cc)
  target_link_libraries(llvm_propeller_program_cfg_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_test_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_program_cfg_test COMMAND llvm_propeller_program_cfg_test)

  add_executable(llvm_propeller_statistics_test llvm_propeller_statistics_test.cc)
  target_link_libraries(llvm_propeller_statistics_test
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_statistics_test COMMAND llvm_propeller_statistics_test)

  add_executable(perfdata_reader_test perfdata_reader_test.cc)
  target_link_libraries(perfdata_reader_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME perfdata_reader_test COMMAND perfdata_reader_test)

  add_executable(spe_sample_reader_test spe_sample_reader_test.cc)
  target_link_libraries(spe_sample_reader_test
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    mini_disassembler
    perfdata_reader
    quipper_perf
    sample_reader
    status_provider
    symbol_map)
  add_test(NAME spe_sample_reader_test COMMAND spe_sample_reader_test)

  add_executable(llvm_propeller_code_layout_test llvm_propeller_code_layout_test.cc)
  target_link_libraries(llvm_propeller_code_layout_test
    gmock
    gtest
    gtest_main
    llvm_profile_writer
    llvm_propeller_objects
    llvm_propeller_perf_data_provider
    llvm_propeller_test_objects
    mini_disassembler
    perfdata_reader
    quipper_perf
    status_provider
    symbol_map)
  add_test(NAME llvm_propeller_code_layout_test COMMAND llvm_propeller_code_layout_test)

  add_library(llvm_propeller_cfg OBJECT llvm_propeller_cfg.cc)
  add_library(llvm_propeller_formatting OBJECT llvm_propeller_formatting.cc)

  add_library(quipper_perf OBJECT
    third_party/perf_data_converter/src/quipper/address_mapper.cc
    third_party/perf_data_converter/src/quipper/arm_spe_decoder.cc
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
    third_party/perf_data_converter/src/quipper/string_utils.cc)

  target_include_directories(quipper_perf PRIVATE
    third_party/abseil)
  target_include_directories(quipper_perf BEFORE
    PUBLIC
    third_party/perf_data_converter/src
    third_party/perf_data_converter/src/quipper
    ${PROJECT_BINARY_DIR}/third_party/perf_data_converter/src/quipper)
  target_link_libraries(quipper_perf
    perf_proto ${LIBELF_LIBRARIES} ${LIBZ_LIBRARIES} ${LIBCRYPTO_LIBRARIES})

  add_custom_command(PRE_BUILD
    OUTPUT prepare_cmds
    COMMAND ln -s -f ${CMAKE_HOME_DIRECTORY}/testdata)
  add_custom_target(prepare ALL
    DEPENDS prepare_cmds)
endfunction()

if (DEFINED ENABLE_TOOL)
   string(TOLOWER ${ENABLE_TOOL} enable_tool)
else()
   set(enable_tool llvm)
endif()

# perf protobuf is required by both tools.
execute_perf_protobuf()
if (${enable_tool} STREQUAL gcov)
  message(STATUS "Building tool \"GCOV\" ...")
  build_gcov()
elseif (${enable_tool} STREQUAL llvm)
  message(STATUS "Building tool \"LLVM\" ...")

  if (NOT ${BUILD_SHARED})
    # Build static binaries.
    set (BUILD_SHARED_LIBS OFF)
    set (CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    # Link static executables.
    set (CMAKE_EXE_LINKER_FLAGS "-static")
  endif()

  build_llvm()
else ()
  message(FATAL_ERROR
    "ENABLE_TOOL must equal to one of \"LLVM\" (default) or \"GCOV\"")
endif ()

enable_testing()
