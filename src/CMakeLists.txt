FILE(GLOB_RECURSE SOURCES  "*.cpp")
FILE(GLOB_RECURSE HEADERS  "*.h")

add_executable(kaleidoscope ${SOURCES})

message("llvm dir path " ${llvm_dir})

target_link_libraries(
    kaleidoscope
    PRIVATE
    ${llvm_libs}
)