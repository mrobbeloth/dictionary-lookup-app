cmake_minimum_required(VERSION 3.14)
project(DictionaryLookup VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(SOUP3 REQUIRED libsoup-3.0)

find_package(nlohmann_json REQUIRED)

add_executable(dictionary-lookup src/main.cpp)

target_include_directories(dictionary-lookup PRIVATE
    ${GTK3_INCLUDE_DIRS}
    ${SOUP3_INCLUDE_DIRS}
)

target_link_libraries(dictionary-lookup PRIVATE
    ${GTK3_LIBRARIES}
    ${SOUP3_LIBRARIES}
    nlohmann_json::nlohmann_json
)

target_compile_options(dictionary-lookup PRIVATE
    ${GTK3_CFLAGS_OTHER}
    ${SOUP3_CFLAGS_OTHER}
)

install(TARGETS dictionary-lookup DESTINATION bin)
