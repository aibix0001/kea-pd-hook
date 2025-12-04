cmake_minimum_required(VERSION 3.10)
project(kea-pd-hook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# Kea include directories
include_directories(/usr/include/kea)

# Create shared library
add_library(pd_webhook SHARED pd_webhook.cc)

# Link libraries
target_link_libraries(pd_webhook 
    ${CURL_LIBRARIES}
    ${JSONCPP_LIBRARIES}
)

# Set output name
set_target_properties(pd_webhook PROPERTIES
    OUTPUT_NAME "pd_webhook"
    PREFIX "lib"
)