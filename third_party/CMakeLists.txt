cmake_minimum_required(VERSION 3.0)

# 创建一个接口库，代表第三方库
add_library(third_party INTERFACE)

set(THIRD_PARTY_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
)

foreach(optional_include_dir IN ITEMS RAPIDJSON_INCLUDE_DIR LIBELF_INCLUDE_DIR LIBELF_GELF_INCLUDE_DIR LIBELF_ROOT_INCLUDE_DIR)
    if(DEFINED ${optional_include_dir} AND NOT "${${optional_include_dir}}" MATCHES "-NOTFOUND$")
        list(APPEND THIRD_PARTY_INCLUDE_DIRS ${${optional_include_dir}})
    endif()
endforeach()

# 为这个库添加头文件搜索路径
target_include_directories(third_party INTERFACE 
    ${THIRD_PARTY_INCLUDE_DIRS}
)

# 可选：如果第三方库有子目录需要包含
# target_include_directories(third_party INTERFACE 
#     ${CMAKE_CURRENT_SOURCE_DIR}/include
#     ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann
# )

# 可选：添加编译定义或其他属性
# target_compile_definitions(third_party INTERFACE 
#     -DUSE_JSON
# )

# 可选：如果第三方库需要链接其他系统库
# find_package(Threads REQUIRED)
# target_link_libraries(third_party INTERFACE Threads::Threads)
