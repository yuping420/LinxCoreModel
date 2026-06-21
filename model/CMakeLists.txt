cmake_minimum_required(VERSION 3.0)

# 收集所有源文件 - 只处理本目录下的文件
file(GLOB_RECURSE MODEL_SRC CONFIGURE_DEPENDS *.cpp)
# 排除不应该包含在库中的文件
list(FILTER MODEL_SRC EXCLUDE REGEX ".*/main\\.cpp$")
# 利用编译选项过滤SOC源文件
if (GENERIC_SOC OR GENERIC_SOC_NEW) 
    list(FILTER MODEL_SRC EXCLUDE REGEX "./soc/soc_wrapper\\.cpp$")
else()
     list(FILTER MODEL_SRC EXCLUDE REGEX "./generic_soc/soc_wrapper\\.cpp$")
endif()

# 创建模型库
add_library(model_lib STATIC ${MODEL_SRC})
target_compile_options(model_lib PRIVATE
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
target_include_directories(model_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(model_lib PRIVATE
    isa_blockisa
    tools_lib
    soft_core
    configs
)

if (GENERIC_SOC)  
    # 如果启用了GENERIC SOC 支持，添加 SOC 库链接
    target_compile_definitions(model_lib PRIVATE $<$<BOOL:${GENERIC_SOC}>:GENERIC_SOC>)
    if(GRAPHFLOW_X86_SUPPORT)
        message(STATUS "Checking SOC GENERIC support for model_lib")
        
        # 使用统一的链接函数
        if(SOC_GENERIC_LIBS_GLOBAL AND SOC_GENERIC_EXTRACT_TARGET_GLOBAL)
            message(STATUS "Adding SOC generic dependencies to model_lib")
            link_soc_generic_libraries(model_lib)
        else()
            message(WARNING "SOC support enabled but SOC generic libraries not available for model_lib")
            message(WARNING "SOC_GENERIC_LIBS_GLOBAL: ${SOC_GENERIC_LIBS_GLOBAL}")
            message(WARNING "SOC_GENERIC_EXTRACT_TARGET_GLOBAL: ${SOC_GENERIC_EXTRACT_TARGET_GLOBAL}")
        endif()
    else()
        message(STATUS "SOC generic support disabled for model_lib (not x86 platform)")
    endif()
elseif (GENERIC_SOC_NEW) 
    # 如果启用了NEW GENERIC SOC 支持，添加 SOC 库链接
    target_compile_definitions(model_lib PRIVATE $<$<BOOL:${GENERIC_SOC_NEW}>:GENERIC_SOC_NEW>)
    if(GRAPHFLOW_X86_SUPPORT)
        message(STATUS "Checking SOC New GENERIC support for model_lib")
        
        # 使用统一的链接函数
        if(SOC_NEW_GENERIC_LIBS_GLOBAL AND SOC_NEW_GENERIC_EXTRACT_TARGET_GLOBAL)
            message(STATUS "Adding NEW ver SOC generic dependencies to model_lib")
            link_soc_generic_libraries(model_lib)
        else()
            message(WARNING "New SOC support enabled but SOC generic libraries not available for model_lib")
            message(WARNING "SOC_NEW_GENERIC_LIBS_GLOBAL: ${SOC_NEW_GENERIC_LIBS_GLOBAL}")
            message(WARNING "SOC_NEW_GENERIC_EXTRACT_TARGET_GLOBAL: ${SOC_NEW_GENERIC_EXTRACT_TARGET_GLOBAL}")
        endif()
    else()
        message(STATUS "SOC generic support disabled for model_lib (not x86 platform)")
    endif()
else()
    if(GRAPHFLOW_X86_SUPPORT)
        message(STATUS "Checking SOC P688 support for model_lib\n")
        
        # 使用统一的链接函数
        if(SOC_LIBS_GLOBAL AND SOC_EXTRACT_TARGET_GLOBAL)
            message(STATUS "Adding SOC P688 dependencies to model_lib")
            link_soc_libraries(model_lib)
        else()
            message(WARNING "SOC support enabled but SOC libraries not available for model_lib")
            message(WARNING "SOC_LIBS_GLOBAL: ${SOC_LIBS_GLOBAL}")
            message(WARNING "SOC_EXTRACT_TARGET_GLOBAL: ${SOC_EXTRACT_TARGET_GLOBAL}")
        endif()
    else()
        message(STATUS "SOC support disabled for model_lib (not x86 platform)")
    endif()
endif()
