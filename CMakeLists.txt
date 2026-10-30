cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
# set(CMAKE_PROJECT_NAME AIOT_M4_F40x_L4a)
get_filename_component(CMAKE_PROJECT_NAME
    ${CMAKE_CURRENT_SOURCE_DIR} NAME)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# 收集 user 四层源文件（cfg 是纯头文件无 .c；common 为横切工具）
file(GLOB PERCEPTION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/perception/Src/*.c")  # 感知层
file(GLOB NETWORK_SOURCES    CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/network/Src/*.c")     # 网络层
file(GLOB PLATFORM_SOURCES   CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/platform/Src/*.c")    # 平台层
file(GLOB APP_SOURCES        CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/app/Src/*.c")         # 应用层
file(GLOB COMMON_SOURCES     CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/common/*.c")          # 横切：ringbuf 等

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${PERCEPTION_SOURCES}
    ${NETWORK_SOURCES}
    ${PLATFORM_SOURCES}
    ${APP_SOURCES}
    ${COMMON_SOURCES}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/user/perception/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/user/network/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/user/platform/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/user/app/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/user/cfg
    ${CMAKE_CURRENT_SOURCE_DIR}/user/common
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            ${CMAKE_PROJECT_NAME}.hex)
