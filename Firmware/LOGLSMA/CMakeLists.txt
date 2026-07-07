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
set(CMAKE_PROJECT_NAME LOGLSMA)

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

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    App/Src/Data.c
    App/Src/FlashQ.c
    App/Src/com.c
    App/Src/iflash.c    # служебное хранилище во внутренней Flash (счётчики/паспорт/конфиг)
    #App/Src/command.c    # отключён: дублирует Service/print/send из com.c
    App/Src/fm25xx.c
    App/Src/ltp.c
    App/Src/lsm6dso.c
    App/Src/lsm6dso_bus.c
    App/Src/lsm6dso_reg.c
    App/Src/lsm6dso_temp.c
    App/Src/myfunc.c
    App/Src/p25q128.c
    App/Src/pwr.c
    App/Src/ring_buffer.c
    #App/Src/rtc.c    # дубль Core/Src/rtc.c — уже в cmake/stm32cubemx
    App/Src/stm_temp.c
    App/Src/tmp117.c
    #App/Src/wake.c    # отключён: заменён LTP (App/Src/ltp.c), оставлен как справочник
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App/Inc
    Core/Inc
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
