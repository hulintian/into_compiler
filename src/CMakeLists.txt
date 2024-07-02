FILE(GLOB_RECURSE SOURCES  "*.cpp")
FILE(GLOB_RECURSE HEADERS  "*.h")

add_executable(kaleidoscope ${SOURCES})

message("llvm dir path " ${llvm_dir})
message("llvm dir path " ${llvm_libs})

llvm_map_components_to_libnames(llvm_libs core orcjit native)

target_link_libraries(
    kaleidoscope
    PRIVATE
    ${llvm_libs}
)