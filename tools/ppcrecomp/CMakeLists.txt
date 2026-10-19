# Host tool: PowerPC -> C++ recompiler. Uses Supermodel's disassembler for comments.
add_executable(ppcrecomp ppcrecomp.cpp ${SM}/Src/CPU/PowerPC/PPCDisasm.cpp)
target_include_directories(ppcrecomp PRIVATE ${SM}/Src ${SM}/Src/OSD ${SM}/Src/OSD/SDL)
target_compile_features(ppcrecomp PRIVATE cxx_std_17)
