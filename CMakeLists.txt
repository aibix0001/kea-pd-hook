cmake_minimum_required(VERSION 3.10)
project(kea-pd-hook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages (optional for build demo)
find_package(CURL)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(JSONCPP jsoncpp)
endif()

# Kea include directories
include_directories(/usr/include/kea)

# Create shared library
add_library(pd_webhook SHARED pd_webhook.cc netbox_client.cc)

# Link libraries
target_link_libraries(pd_webhook
    $<$<TARGET_EXISTS:CURL::libcurl>:CURL::libcurl>
    jsoncpp
)

# Set output name
set_target_properties(pd_webhook PROPERTIES
    OUTPUT_NAME "pd_webhook"
    PREFIX "lib"
)

# Install target
install(TARGETS pd_webhook
    LIBRARY DESTINATION lib/x86_64-linux-gnu/kea/hooks
)