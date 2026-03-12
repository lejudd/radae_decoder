add_library(yyjson STATIC yyjson.c)
target_include_directories(yyjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_source_files_properties(yyjson.c PROPERTIES
    SKIP_LINTING ON
)
