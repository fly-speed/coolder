cmake_minimum_required(VERSION 3.16)

project(coolder VERSION 1.0.0 LANGUAGES C CXX)

option(BUILD_ACL "Build ACL from the sibling third-party source tree" OFF)
set(ACL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../third-party/acl"
	CACHE PATH "ACL root")
if(BUILD_ACL)
	add_subdirectory("${ACL_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/acl"
		EXCLUDE_FROM_ALL)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBAI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libai"
	CACHE PATH "libai sources")
add_subdirectory("${LIBAI_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libai")

file(GLOB COOLDER_ACTIONS CONFIGURE_DEPENDS action/ai/*.cpp)
add_executable(coolder main.cpp server/host.cpp server/attachments.cpp server/auth.cpp server/ai_policy.cpp ${COOLDER_ACTIONS})
target_include_directories(coolder PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(coolder PRIVATE libai::ai)
target_precompile_headers(coolder PRIVATE stdafx.h)
target_compile_definitions(coolder PRIVATE
	COOLDER_HTML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/html")

if(APPLE)
	target_compile_definitions(coolder PRIVATE MACOSX)
endif()
if(MSVC)
	target_compile_options(coolder PRIVATE /utf-8 /W3)
else()
	target_compile_options(coolder PRIVATE -Wall -Wextra)
endif()

# libai locates this compatibility-named helper next to the host executable.
add_executable(webcool-sandbox-helper sandbox/sandbox_helper_main.cpp)
if(WIN32)
	target_link_libraries(webcool-sandbox-helper PRIVATE advapi32)
endif()
add_custom_target(coolder_frontend
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle_frontend.cmake"
  BYPRODUCTS "${CMAKE_CURRENT_SOURCE_DIR}/html/coolder.js"
  COMMENT "Bundling coolder browser scripts"
  VERBATIM)
add_dependencies(coolder webcool-sandbox-helper coolder_frontend)

install(TARGETS coolder webcool-sandbox-helper RUNTIME DESTINATION bin)
install(DIRECTORY html/ DESTINATION share/coolder/html)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
	add_test(NAME coolder_accounts
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/tests/accounts_test.py"
			"$<TARGET_FILE:coolder>")
	add_test(NAME coolder_http
		COMMAND "${Python3_EXECUTABLE}"
			"${CMAKE_CURRENT_SOURCE_DIR}/tests/http_test.py"
			"$<TARGET_FILE:coolder>")
endif()
