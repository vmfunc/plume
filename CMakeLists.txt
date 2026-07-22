cmake_minimum_required(VERSION 3.25)
project(plume CXX)

if(NOT DEFINED PLUME_VERSION)
	set(PLUME_VERSION "0.1.0")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

option(PLUME_WERROR "treat warnings as errors" ON)
option(PLUME_TESTS "build the test suite" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(LUAJIT REQUIRED IMPORTED_TARGET luajit)
pkg_check_modules(CHAFA REQUIRED IMPORTED_TARGET chafa)
pkg_check_modules(TREESITTER IMPORTED_TARGET tree-sitter)

find_package(ftxui REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(spdlog REQUIRED)
find_package(sol2 REQUIRED)

find_path(STB_INCLUDE_DIR stb/stb_image.h REQUIRED)

# warning policy lives on an interface target our code links; third-party
# headers come in as SYSTEM so -Werror only ever polices plume itself.
add_library(plume_flags INTERFACE)
target_compile_options(plume_flags INTERFACE
	-Wall -Wextra -Wpedantic
	-Wshadow -Wnon-virtual-dtor -Wcast-align -Wnull-dereference
	-fstack-protector-strong)
if(PLUME_WERROR)
	target_compile_options(plume_flags INTERFACE -Werror)
endif()

file(GLOB_RECURSE PLUME_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER PLUME_SOURCES EXCLUDE REGEX "src/main\\.cpp$")

add_library(plume_core STATIC ${PLUME_SOURCES})
target_include_directories(plume_core PUBLIC include)
target_include_directories(plume_core SYSTEM PRIVATE ${STB_INCLUDE_DIR})
target_compile_definitions(plume_core PRIVATE PLUME_VERSION="${PLUME_VERSION}")
target_link_libraries(plume_core
	PUBLIC
		plume_flags
	PRIVATE
		PkgConfig::CURL
		PkgConfig::SQLITE
		PkgConfig::LUAJIT
		PkgConfig::CHAFA
		ftxui::screen ftxui::dom ftxui::component
		nlohmann_json::nlohmann_json
		tomlplusplus::tomlplusplus
		spdlog::spdlog
		sol2::sol2)
if(TARGET PkgConfig::TREESITTER)
	target_link_libraries(plume_core PRIVATE PkgConfig::TREESITTER)
	target_compile_definitions(plume_core PRIVATE PLUME_HAVE_TREESITTER=1)
endif()

if(UNIX AND NOT APPLE)
	pkg_check_modules(SECRET IMPORTED_TARGET libsecret-1)
	if(TARGET PkgConfig::SECRET)
		target_link_libraries(plume_core PRIVATE PkgConfig::SECRET)
		target_compile_definitions(plume_core PRIVATE PLUME_HAVE_LIBSECRET=1)
	endif()
endif()

add_executable(plume src/main.cpp)
target_link_libraries(plume PRIVATE plume_core plume_flags)

install(TARGETS plume RUNTIME DESTINATION bin)

# man page + completions
find_program(SCDOC scdoc)
if(SCDOC AND EXISTS "${CMAKE_SOURCE_DIR}/man/plume.1.scd")
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/plume.1
		COMMAND ${SCDOC} < ${CMAKE_SOURCE_DIR}/man/plume.1.scd > ${CMAKE_BINARY_DIR}/plume.1
		DEPENDS ${CMAKE_SOURCE_DIR}/man/plume.1.scd)
	add_custom_target(man ALL DEPENDS ${CMAKE_BINARY_DIR}/plume.1)
	install(FILES ${CMAKE_BINARY_DIR}/plume.1 DESTINATION share/man/man1)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/completions")
	install(DIRECTORY completions/ DESTINATION share/plume/completions)
endif()
install(DIRECTORY themes/ DESTINATION share/plume/themes OPTIONAL)

if(PLUME_TESTS)
	enable_testing()
	find_package(doctest REQUIRED)
	file(GLOB PLUME_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
	add_executable(plume_tests ${PLUME_TEST_SOURCES})
	target_link_libraries(plume_tests PRIVATE plume_core plume_flags
		ftxui::screen ftxui::dom ftxui::component
		nlohmann_json::nlohmann_json doctest::doctest)
	target_include_directories(plume_tests PRIVATE src)
	target_include_directories(plume_tests SYSTEM PRIVATE ${STB_INCLUDE_DIR})
	add_test(NAME plume_tests COMMAND plume_tests)
endif()
