cmake_minimum_required(VERSION 3.19)

project(WaveformStudio VERSION 0.1 LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Widgets
    Multimedia
    MultimediaWidgets
)

qt_standard_project_setup()

qt_add_executable(WaveformStudio
    WIN32 MACOSX_BUNDLE

    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    spectrumanalyzer.cpp
    spectrumanalyzer.h
    spectrumrenderer.h
    spectrumexportframes.h
    gpurenderer.cpp
    gpurenderer.h
    nativegpupresenter.cpp
    nativegpupresenter.h
)

target_link_libraries(WaveformStudio
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Multimedia
        Qt6::MultimediaWidgets
)

install(TARGETS WaveformStudio
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET WaveformStudio
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)

install(SCRIPT ${deploy_script})

# Native Direct3D 11 runtime (Windows only).
if(WIN32)
    target_link_libraries(WaveformStudio PRIVATE d3d11 dxgi d3dcompiler)
endif()
