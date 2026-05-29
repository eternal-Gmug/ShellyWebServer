cmake_minimum_required(VERSION 3.16)

project(ShellyWebServer LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SRC_DIR "${PROJECT_SOURCE_DIR}/src")
set(INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")

function(add_module MODULE_NAME)
	file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS
		"${SRC_DIR}/${MODULE_NAME}/*.cpp"
		"${SRC_DIR}/${MODULE_NAME}/*.cc"
		"${SRC_DIR}/${MODULE_NAME}/*.cxx"
	)

	if(MODULE_SOURCES)
		add_library(${MODULE_NAME} STATIC ${MODULE_SOURCES})
		target_include_directories(${MODULE_NAME} PUBLIC ${INCLUDE_DIR} ${SRC_DIR})
	else()
		add_library(${MODULE_NAME} INTERFACE)
		target_include_directories(${MODULE_NAME} INTERFACE ${INCLUDE_DIR} ${SRC_DIR})
	endif()
endfunction()

function(link_module_deps TARGET)
	get_target_property(TARGET_TYPE ${TARGET} TYPE)
	if(TARGET_TYPE STREQUAL "INTERFACE_LIBRARY")
		target_link_libraries(${TARGET} INTERFACE ${ARGN})
	else()
		target_link_libraries(${TARGET} PUBLIC ${ARGN})
	endif()
endfunction()

add_module(concurrency)
add_module(database)
add_module(http)
add_module(limit)
add_module(log)
add_module(net)
add_module(router)
add_module(server)
add_module(services)
add_module(timer)
add_module(utils)

add_executable(shelly_webserver "${SRC_DIR}/main.cpp"
	include/sql_connection_pool.h)
target_include_directories(shelly_webserver PRIVATE ${INCLUDE_DIR} ${SRC_DIR})
target_link_libraries(shelly_webserver PRIVATE server)

link_module_deps(server router http net concurrency database timer log utils limit services)
link_module_deps(router http services)
link_module_deps(http net utils log)
link_module_deps(net concurrency utils)
link_module_deps(timer utils)
link_module_deps(limit utils log)
link_module_deps(services utils)
link_module_deps(concurrency utils)
link_module_deps(log utils)

# unit and integration tests (temporary test)(if needed)
add_executable(sql_connection_pool_selftest
    scripts/sql_connection_pool_selftest.cpp
)

target_include_directories(sql_connection_pool_selftest PRIVATE
    ${INCLUDE_DIR}
    ${SRC_DIR}
)

target_link_libraries(sql_connection_pool_selftest PRIVATE
    database
    log
    mysqlclient
)