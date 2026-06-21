cmake_minimum_required(VERSION 3.12)
project(blockisa-model VERSION 0.20.0 LANGUAGES CXX)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib)

# 包含自定义 CMake 模块
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(GlobalSettings)
include(SOCSetup)

find_path(RAPIDJSON_INCLUDE_DIR document.h
    HINTS /opt/homebrew/include /opt/homebrew/opt/rapidjson/include
    PATH_SUFFIXES rapidjson
)
find_path(LIBELF_INCLUDE_DIR libelf.h
    HINTS /opt/homebrew/include /opt/homebrew/opt/libelf/include
    PATH_SUFFIXES libelf
)
find_path(LIBELF_GELF_INCLUDE_DIR gelf.h
    HINTS /opt/homebrew/include /opt/homebrew/opt/libelf/include
    PATH_SUFFIXES libelf
)
find_path(LIBELF_ROOT_INCLUDE_DIR libelf/libelf.h
    HINTS /opt/homebrew/include /opt/homebrew/opt/libelf/include
)
find_library(LIBELF_LIBRARY elf HINTS /opt/homebrew/lib /opt/homebrew/opt/libelf/lib)
if(LIBELF_LIBRARY)
    set(LINXCOREMODEL_LIBELF ${LIBELF_LIBRARY})
else()
    set(LINXCOREMODEL_LIBELF elf)
endif()

# 平台检测
gf_detect_platform()

# 设置全局编译选项
gf_set_global_compiler_options()

set(LINXCOREMODEL_CLANG_COMPAT_OPTIONS
    -Wno-error=c++17-extensions
    -Wno-error=defaulted-function-deleted
    -Wno-error=delete-non-abstract-non-virtual-dtor
    -Wno-error=final-dtor-non-final-class
    -Wno-error=format
    -Wno-error=inconsistent-missing-override
    -Wno-error=mismatched-tags
    -Wno-error=parentheses-equality
    -Wno-error=unused-const-variable
    -Wno-error=unused-lambda-capture
    -Wno-error=unused-private-field
    -Wno-error=unused-but-set-variable
)

if (GENERIC_SOC)
    include(SOCSetupGeneric)
    # 链接 GENERIC SOC 库（如果启用）
    if(GRAPHFLOW_X86_SUPPORT)
        # 将GENERIC SOC 启用
        message(STATUS "Configuring SOC GENERIC support")
        setup_generic_soc_libraries()

        # 检查 SOC 库是否成功设置（使用从函数导出的变量）
        if(SOC_GENERIC_LIBS_GLOBAL AND SOC_GENERIC_EXTRACT_TARGET_GLOBAL)
            # message(STATUS "SOC libraries available: ${SOC_GENERIC_LIBS_GLOBAL}")
            message(STATUS "SOC GENERIC extract target: ${SOC_GENERIC_EXTRACT_TARGET_GLOBAL}")
            
            # 将generic SOC 变量设置为全局变量，以便在子目录中使用
            set(SOC_GENERIC_LIB_NAMES_GLOBAL ${SOC_GENERIC_LIB_NAMES_GLOBAL} CACHE INTERNAL "GENERIC SOC library names for global use")
            set(SOC_GENERIC_LIBS_GLOBAL ${SOC_GENERIC_LIBS_GLOBAL} CACHE INTERNAL "GENERIC SOC libraries for global use")
            set(SOC_GENERIC_EXTRACT_TARGET_GLOBAL ${SOC_GENERIC_EXTRACT_TARGET_GLOBAL} CACHE INTERNAL "GENERIC SOC extract target for global use")
            
            # 设置SOC库目录和编译定义
            link_directories(${PROJECT_SOURCE_DIR}/model/generic_soc/lib)
            add_definitions(-DEXT_SOC_INTF)
        else()
            message(WARNING "SOC GENERIC libraries not available, skipping GENERIC SOC setup")
            set(GRAPHFLOW_X86_SUPPORT OFF CACHE BOOL "SOC GENERIC support status" FORCE)
        endif()

    else()
        message(STATUS "GENERIC SOC support disabled (not x86 platform)")
    endif()
elseif(GENERIC_SOC_NEW)
    include(SOCSetupNewGeneric)
    # 链接 GENERIC SOC 库（如果启用）
    if(GRAPHFLOW_X86_SUPPORT)
        # 将GENERIC New SOC 启用
        message(STATUS "Configuring SOC new GENERIC support")
        setup_generic_new_soc_libraries()

        # 检查 SOC 库是否成功设置（使用从函数导出的变量）
        if(SOC_NEW_GENERIC_LIBS_GLOBAL AND SOC_NEW_GENERIC_EXTRACT_TARGET_GLOBAL)
            # message(STATUS "SOC libraries available: ${SOC_NEW_GENERIC_LIBS_GLOBAL}")
            message(STATUS "SOC GENERIC extract target: ${SOC_NEW_GENERIC_EXTRACT_TARGET_GLOBAL}")
            
            # GENERIC SOC 变量设置为全局变量，以便在子目录中使用
            set(SOC_NEW_GENERIC_LIB_NAMES_GLOBAL ${SOC_NEW_GENERIC_LIB_NAMES_GLOBAL} CACHE INTERNAL "New GENERIC SOC library names for global use")
            set(SOC_NEW_GENERIC_LIBS_GLOBAL ${SOC_NEW_GENERIC_LIBS_GLOBAL} CACHE INTERNAL "New GENERIC SOC libraries for global use")
            set(SOC_NEW_GENERIC_EXTRACT_TARGET_GLOBAL ${SOC_NEW_GENERIC_EXTRACT_TARGET_GLOBAL} CACHE INTERNAL "New GENERIC SOC extract target for global use")
            
            # 设置SOC库目录和编译定义
            link_directories(${PROJECT_SOURCE_DIR}/model/generic_soc/lib)
            add_definitions(-DEXT_SOC_INTF)
        else()
            message(WARNING "NEW SOC GENERIC libraries not available, skipping GENERIC SOC setup")
            set(GRAPHFLOW_X86_SUPPORT OFF CACHE BOOL "SOC GENERIC support status" FORCE)
        endif()

    else()
        message(STATUS "GENERIC SOC support disabled (not x86 platform)")
    endif()
else()
    # 链接 P688 SOC 库（如果启用）
    if(GRAPHFLOW_X86_SUPPORT)
        # 将SOC 启用
        message(STATUS "Configuring SOC support")
        setup_soc_libraries()

        # 检查 SOC 库是否成功设置（使用从函数导出的变量）
        if(SOC_LIBS_GLOBAL AND SOC_EXTRACT_TARGET_GLOBAL)
            # message(STATUS "SOC libraries available: ${SOC_LIBS_GLOBAL}")
            message(STATUS "SOC extract target: ${SOC_EXTRACT_TARGET_GLOBAL}")
            
            # 将 SOC 变量设置为全局变量，以便在子目录中使用
            set(SOC_LIB_NAMES_GLOBAL ${SOC_LIB_NAMES_GLOBAL} CACHE INTERNAL "SOC library names for global use")
            set(SOC_LIBS_GLOBAL ${SOC_LIBS_GLOBAL} CACHE INTERNAL "SOC libraries for global use")
            set(SOC_EXTRACT_TARGET_GLOBAL ${SOC_EXTRACT_TARGET_GLOBAL} CACHE INTERNAL "SOC extract target for global use")
            
            # 设置SOC库目录和编译定义
            link_directories(${PROJECT_SOURCE_DIR}/model/soc/lib)
            add_definitions(-DEXT_SOC_INTF)
        else()
            message(WARNING "SOC libraries not available, skipping SOC setup")
            set(GRAPHFLOW_X86_SUPPORT OFF CACHE BOOL "SOC support status" FORCE)
        endif()

    else()
        message(STATUS "SOC support disabled (not x86 platform)")
    endif()
endif()


# 添加子目录
add_subdirectory(configs)
add_subdirectory(third_party)
add_subdirectory(common)
add_subdirectory(softfloat)
add_subdirectory(isa)
add_subdirectory(emulator)

add_subdirectory(tools)
add_subdirectory(model)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()


# 创建可执行文件
message(STATUS "Creating executable targets...")

# gfrun - 功能模拟器
add_executable(gfrun emulator/main.cpp)
target_link_libraries(gfrun PRIVATE soft_core isa_blockisa softfloat ${LINXCOREMODEL_LIBELF})
target_compile_options(gfrun PRIVATE ${LINXCOREMODEL_CLANG_COMPAT_OPTIONS})
message(STATUS "Created target: gfrun")

# gfsim
if (GENERIC_SOC)
    # gfsim - CA 模拟器
    add_executable(gfsim model/main.cpp model/generic_soc/soc_wrapper.cpp)
    target_link_libraries(gfsim PRIVATE soft_core model_lib isa_blockisa softfloat ${LINXCOREMODEL_LIBELF})
    target_compile_definitions(gfsim PRIVATE $<$<BOOL:${GENERIC_SOC}>:GENERIC_SOC>)

    message(STATUS "Created target: gfsim")

    # 链接 GENERIC SOC 库到在根目录中创建的目标
    if(GRAPHFLOW_X86_SUPPORT AND SOC_GENERIC_LIBS_GLOBAL AND SOC_GENERIC_EXTRACT_TARGET_GLOBAL)
        message(STATUS "Adding SOC GENERIC dependencies to targets built in root directory")

        if(TARGET gfsim)
            link_soc_generic_libraries(gfsim)
        else()
            message(WARNING "Target gfsim generic does not exist, cannot add SOC dependencies")
        endif()
    elseif(GRAPHFLOW_X86_SUPPORT)
        message(WARNING "SOC support enabled but SOC generic libraries not available for root targets")
    endif()
elseif(GENERIC_SOC_NEW)
    add_executable(gfsim model/main.cpp model/generic_soc/soc_wrapper.cpp)
    target_link_libraries(gfsim PRIVATE soft_core model_lib isa_blockisa softfloat ${LINXCOREMODEL_LIBELF})
    target_compile_definitions(gfsim PRIVATE $<$<BOOL:${GENERIC_SOC_NEW}>:GENERIC_SOC_NEW>)

    message(STATUS "Created target: gfsim")

    # 链接 GENERIC SOC 库到在根目录中创建的目标
    if(GRAPHFLOW_X86_SUPPORT AND SOC_NEW_GENERIC_LIBS_GLOBAL AND SOC_NEW_GENERIC_EXTRACT_TARGET_GLOBAL)
        message(STATUS "Adding SOC GENERIC dependencies to targets built in root directory")

        if(TARGET gfsim)
            link_soc_generic_libraries(gfsim)
        else()
            message(WARNING "Target gfsim generic does not exist, cannot add SOC dependencies")
        endif()
    elseif(GRAPHFLOW_X86_SUPPORT)
        message(WARNING "SOC support enabled but SOC generic libraries not available for root targets")
    endif()
else()
    # gfsim - CA 模拟器
    add_executable(gfsim model/main.cpp model/soc/soc_wrapper.cpp)
    target_link_libraries(gfsim PRIVATE soft_core model_lib isa_blockisa softfloat ${LINXCOREMODEL_LIBELF})
    message(STATUS "Created target: gfsim")

    # 链接P688 SOC 库到在根目录中创建的目标
    if(GRAPHFLOW_X86_SUPPORT AND SOC_LIBS_GLOBAL AND SOC_EXTRACT_TARGET_GLOBAL)
        message(STATUS "Adding P688 SOC dependencies to targets built in root directory")

        if(TARGET gfsim)
            link_soc_libraries(gfsim)
        else()
            message(WARNING "Target gfsim does not exist, cannot add SOC dependencies")
        endif()
    elseif(GRAPHFLOW_X86_SUPPORT)
        message(WARNING "SOC support enabled but SOC libraries not available for root targets")
    endif()
endif()

if(TARGET gfsim)
    target_compile_options(gfsim PRIVATE ${LINXCOREMODEL_CLANG_COMPAT_OPTIONS})
endif()

message(STATUS "CMake configuration completed suaccelssfully")
