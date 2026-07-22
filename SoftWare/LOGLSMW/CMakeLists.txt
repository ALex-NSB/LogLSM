cmake_minimum_required(VERSION 3.19)
project(LOGLSMW LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort PrintSupport)

qt_standard_project_setup()

qt_add_executable(LOGLSMW
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    activationbar.cpp
    activationbar.h
    comlink.cpp
    comlink.h
    devicecontroller.cpp
    devicecontroller.h
    portscanner.cpp
    portscanner.h
    qltp.cpp
    qltp.hpp
    qcustomplot/qcustomplot.cpp
    qcustomplot/qcustomplot.h
   
)

# ui_mainwindow.h (autogen) включает activationbar.h и qcustomplot/qcustomplot.h
# из папки исходников — она должна быть в путях поиска
target_include_directories(LOGLSMW PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Путь папки исходников — чтобы приложение подхватило logo.png/logo.ico отсюда
# в рантайме (иконка окна), если пользователь его положит (17.07.2026).
target_compile_definitions(LOGLSMW PRIVATE
    LOGLSMW_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# qcustomplot.cpp is a huge amalgamated file — Debug build exceeds MinGW COFF section limit
if(MINGW)
    set_source_files_properties(qcustomplot/qcustomplot.cpp PROPERTIES
        COMPILE_FLAGS "-Wa,-mbig-obj"
    )
endif()

target_link_libraries(LOGLSMW
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::SerialPort
        Qt::PrintSupport
)

install(TARGETS LOGLSMW
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET LOGLSMW
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
