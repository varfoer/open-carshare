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
  ${CMAKE_CURRENT_SOURCE_DIR}/src/http/server.c
  ${TP}/mongoose/mongoose.c)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/main.c ${CMAKE_CURRENT_SOURCE_DIR}/src/http/server.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_compile_definitions(ocs-server PRIVATE MG_ENABLE_DIRLIST=0)
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
ocs_add_test(test_db)
ocs_add_test(test_password)
ocs_add_test(test_ratelimit)
ocs_add_test(test_mail_build)
ocs_add_test(test_booking_rules)
ocs_add_test(test_session)
ocs_add_test(test_api_auth)
ocs_add_test(test_api_booking)
ocs_add_test(test_api_admin)
ocs_add_test(test_api_tariff)
ocs_add_test(test_api_damage)
ocs_add_test(test_api_km)
ocs_add_test(test_config)
ocs_add_test(test_teltonika)
ocs_add_test(test_km_resolve)
ocs_add_test(test_tracker)
ocs_add_test(test_berlin)
ocs_add_test(test_pricing)

if(OCS_FUZZ)
  target_compile_options(ocs_core PRIVATE -fsanitize=fuzzer-no-link)
  add_executable(fuzz_api
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test/helpers.c)
  target_include_directories(fuzz_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_compile_options(fuzz_api PRIVATE -fsanitize=fuzzer-no-link)
  target_link_libraries(fuzz_api PRIVATE ocs_core)
  target_link_options(fuzz_api PRIVATE -fsanitize=fuzzer)
  add_executable(fuzz_teltonika
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_teltonika.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test/helpers.c)
  target_include_directories(fuzz_teltonika PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_compile_options(fuzz_teltonika PRIVATE -fsanitize=fuzzer-no-link)
  target_link_libraries(fuzz_teltonika PRIVATE ocs_core)
  target_link_options(fuzz_teltonika PRIVATE -fsanitize=fuzzer)
endif()
