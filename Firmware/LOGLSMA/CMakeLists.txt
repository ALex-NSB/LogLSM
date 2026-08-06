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
    App/Src/boot_loader.c   # загрузчик: линкер кладёт ЭТОТ объект в .bootsec (стр.113..120)
    App/Src/ltp_boot.c      # копия LTP для загрузчика (включает ltp.c) — тоже в .bootsec
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

# --- Образ для заливки по кабелю (05.08.2026) --------------------------------
# ⚠ БЕЗ .bootsec. Загрузчик — часть этого же образа, но лежит наверху флеша
# (0x08038800). Если оставить секцию, objcopy забьёт 150 КБ дыры нулями и,
# главное, загрузчик оказался бы в потоке, которым сам себя перезаписывает.
# Поэтому по кабелю едет только приложение (стр.0..112); сама .bootsec
# обновляется исключительно прошивкой по SWD — вместе с полным .elf.
#
# ИМЯ ФАЙЛА — с версией и приставкой Up (06.08.2026): «LOGLSMA.bin» слишком
# похоже на LOGLSMA.elf, их путали, а по имени ещё и не видно, что за версия.
# Получается UpLSMA_26.08.06_00.30.bin — сразу ясно, что это обновление и какое.
# Двоеточие в имени файла Windows не разрешает, поэтому время через точку.
#
# Версию берём прямо из com.c — она там пятью #define и правится руками; так
# имя не может разъехаться с тем, что покажет прибор.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/com.c)   # правка версии → переконфигурация
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/App/Src/com.c FW_COM_C)
set(FW_STAMP "")
foreach(part YY MM DD HH MI)
    if(FW_COM_C MATCHES "#define[ \t]+FW_${part}[ \t]+([0-9]+)")
        set(v ${CMAKE_MATCH_1})
        if(v LESS 10)
            set(v "0${v}")          # ведущий ноль: 8 → 08
        endif()
    else()
        set(v "xx")                 # не нашли — не срываем сборку, видно по имени
    endif()
    if(part STREQUAL "HH")
        set(FW_STAMP "${FW_STAMP}_${v}")     # дата_время
    elseif(FW_STAMP STREQUAL "")
        set(FW_STAMP "${v}")
    else()
        set(FW_STAMP "${FW_STAMP}.${v}")
    endif()
endforeach()
set(FW_BIN_NAME "UpLSMA_${FW_STAMP}.bin")
message("Образ обновления: ${FW_BIN_NAME}")

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary --remove-section=.bootsec
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${FW_BIN_NAME}
    COMMENT "Образ для заливки по кабелю (без .bootsec): ${FW_BIN_NAME}"
)
