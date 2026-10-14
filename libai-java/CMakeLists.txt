cmake_minimum_required(VERSION 3.16)
project(libai_java LANGUAGES C CXX)
find_package(Threads REQUIRED)
# Compile the bridge directly into JNI so deployment needs one native library.
add_library(libai_jni SHARED
 src/main/cpp/jni_rag.cpp src/main/cpp/jni_workflow.cpp src/main/cpp/jni_mcp.cpp src/main/cpp/jni_file_execution.cpp src/main/cpp/jni_execution.cpp src/main/cpp/jni_strings.cpp src/main/cpp/jni_callbacks.cpp src/main/cpp/jni_bridge.cpp src/main/cpp/jni_coding.cpp
 ../libai-c/src/rag.cpp ../libai-c/src/workflow.cpp ../libai-c/src/mcp.cpp ../libai-c/src/file_execution_store.cpp ../libai-c/src/bridge.cpp ../libai-c/src/builders.cpp ../libai-c/src/json_writer.cpp ../libai-c/src/coding.cpp ../libai-c/src/cancellation.cpp ../libai-c/src/provider.cpp )
set_target_properties(libai_jni PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME ai_jni)
target_compile_features(libai_jni PRIVATE cxx_std_11)
target_include_directories(libai_jni PRIVATE ../libai-c/include ..)
target_link_libraries(libai_jni PRIVATE Threads::Threads)
if(NOT ANDROID)
 include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/JNIHeaders.cmake")
endif()
install(TARGETS libai_jni LIBRARY DESTINATION lib RUNTIME DESTINATION bin)

option(LIBAI_CODING_FULL "Enable desktop coding service (ACL/OpenSSL required)" OFF)
if(LIBAI_CODING_FULL)
 include("${CMAKE_CURRENT_SOURCE_DIR}/../libai/cmake/CodingBackend.cmake")
 libai_enable_coding(libai_jni)
endif()

# Preserve notices for the private, vendored Schema validator in binary installs.
install(FILES ../libai/sdk/vendor/rapidjson/LICENSE.txt ../libai/sdk/vendor/rapidjson/SOURCE.md
 DESTINATION share/libai/licenses/rapidjson)
