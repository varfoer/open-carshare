find_package(CURL REQUIRED)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

file(GLOB_RECURSE OCS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
list(FILTER OCS_SOURCES EXCLUDE REGEX "/src/main\\.c$")
list(FILTER OCS_SOURCES EXCLUDE REGEX "/src/http/server\\.c$")

set(OCS_THIRD_PARTY_SOURCES
  ${TP}/cjson/cJSON.c
  ${TP}/monocypher/monocypher.c)

add_library(ocs_core STATIC ${OCS_SOURCES} ${OCS_THIRD_PARTY_SOURCES})
target_include_directories(ocs_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${TP}/cjson ${TP}/monocypher ${TP}/mongoose)
target_link_libraries(ocs_core PUBLIC CURL::libcurl m)
if(OCS_SOURCES)
  set_source_files_properties(${OCS_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
endif()

add_executable(ocs-server
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
  ${TP}/mongoose/mongoose.c)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/main.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_link_libraries(ocs-server PRIVATE ocs_core)

add_library(unity STATIC ${TP}/unity/unity.c)
target_include_directories(unity PUBLIC ${TP}/unity)

function(ocs_add_test name)
  add_executable(${name}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test/helpers.c)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(${name} PRIVATE ocs_core unity)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

ocs_add_test(test_smoke)
ocs_add_test(test_codec)
