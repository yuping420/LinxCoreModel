cmake_minimum_required(VERSION 3.0)

# 收集源文件 - 只处理本目录下的文件
file(GLOB_RECURSE EMULATOR_SRC CONFIGURE_DEPENDS *.cpp)
# 排除不应该包含在库中的文件
list(FILTER EMULATOR_SRC EXCLUDE REGEX ".*/main\\.cpp$")
# list(FILTER EMULATOR_SRC EXCLUDE REGEX ".*/TMAEngine\\.cpp$")
# list(FILTER EMULATOR_SRC EXCLUDE REGEX ".*/CubeEngine\\.cpp$")

# 创建模拟器库
add_library(soft_core STATIC ${EMULATOR_SRC})
target_compile_options(soft_core PRIVATE
    -Wno-error=format
    -Wno-error=nontrivial-memcall
)
target_include_directories(soft_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/softfloat/include
    ${CMAKE_SOURCE_DIR}/binutils/include/elf
    ${CMAKE_SOURCE_DIR}/binutils/rapidjson/include/rapidjson
    ${RAPIDJSON_INCLUDE_DIR}
    ${LIBELF_INCLUDE_DIR}
    ${LIBELF_GELF_INCLUDE_DIR}
    ${LIBELF_ROOT_INCLUDE_DIR}
)
target_link_libraries(soft_core PUBLIC
    isa_blockisa
    third_party
    configs
    ${LINXCOREMODEL_LIBELF}
)
