cmake_minimum_required(VERSION 3.20)
project(ueshm LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
	add_compile_options(/EHs-c- /D_HAS_EXCEPTIONS=0 /W4)
else()
	add_compile_options(-fno-exceptions -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
	set(SHM_SRC common/shm_win.cpp)
	set(PROCESS_SRC tool/process_win.cpp)
else()
	set(SHM_SRC common/shm_posix.cpp)
	set(PROCESS_SRC tool/process_posix.cpp)
endif()

add_library(shm ${SHM_SRC} common/shm.h)
target_include_directories(shm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
if(UNIX AND NOT APPLE)
	find_library(LIBRT rt)
	if(LIBRT)
		target_link_libraries(shm PUBLIC ${LIBRT})
	endif()
endif()

add_library(process ${PROCESS_SRC} tool/process.h)
target_include_directories(process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tool)

add_executable(uectl tool/main.cpp)
target_link_libraries(uectl PRIVATE shm process)

add_executable(shm_test tests/shm_test.cpp)
target_link_libraries(shm_test PRIVATE shm process)

add_executable(ring_test tests/ring_test.cpp)
target_include_directories(ring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(ring_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME shm_two_process COMMAND shm_test)
set_tests_properties(shm_two_process PROPERTIES TIMEOUT 30)
add_test(NAME ring_empty COMMAND ring_test empty)
add_test(NAME ring_full COMMAND ring_test full)
add_test(NAME ring_wrap COMMAND ring_test wrap)
add_test(NAME ring_stress COMMAND ring_test stress)
set_tests_properties(ring_empty ring_full ring_wrap ring_stress PROPERTIES TIMEOUT 30)
