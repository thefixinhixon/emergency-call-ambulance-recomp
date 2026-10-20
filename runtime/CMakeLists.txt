# The recompiled game: Supermodel's frontend and devices + hybrid PPC core + generated code.

set(ECA_ROMSET "${CMAKE_SOURCE_DIR}/roms/eca.zip" CACHE FILEPATH "Your eca ROM set (zip, may be nested)")
set(ECA_GAME "eca" CACHE STRING "Game name from Games.xml to recompile")
option(ECA_RECOMPILE "Generate recompiled code from the ROM set" ON)
set(ECA_SHARDS 16)   # generated translation units; fixed so CMake knows the outputs up front

# Cross builds run the generators from a native build of the tools
set(ECA_HOST_ROMTOOL "" CACHE FILEPATH "Host romtool executable (cross-compiling only)")
set(ECA_HOST_PPCRECOMP "" CACHE FILEPATH "Host ppcrecomp executable (cross-compiling only)")
if(ECA_HOST_ROMTOOL)
  set(ROMTOOL_CMD ${ECA_HOST_ROMTOOL})
  set(ROMTOOL_DEP ${ECA_HOST_ROMTOOL})
else()
  set(ROMTOOL_CMD $<TARGET_FILE:romtool>)
  set(ROMTOOL_DEP romtool)
endif()
if(ECA_HOST_PPCRECOMP)
  set(PPCRECOMP_CMD ${ECA_HOST_PPCRECOMP})
  set(PPCRECOMP_DEP ${ECA_HOST_PPCRECOMP})
else()
  set(PPCRECOMP_CMD $<TARGET_FILE:ppcrecomp>)
  set(PPCRECOMP_DEP ppcrecomp)
endif()

set(GEN ${CMAKE_BINARY_DIR}/generated)
set(ECA_GEN_SOURCES)
if(ECA_RECOMPILE AND EXISTS "${ECA_ROMSET}")
  set(ROMDIR ${GEN}/rom)
  add_custom_command(
    OUTPUT ${ROMDIR}/crom.bin ${ROMDIR}/Games.xml ${ROMDIR}/${ECA_GAME}.zip
    COMMAND ${ROMTOOL_CMD} ${SM}/Config/Games.xml ${ECA_ROMSET} ${ECA_GAME} ${ROMDIR}
    DEPENDS ${ROMTOOL_DEP} ${ECA_ROMSET} ${SM}/Config/Games.xml
    COMMENT "Extracting ROM regions from ${ECA_ROMSET}")

  foreach(i RANGE 0 15)
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
      set(i "0${i}")
    endif()
    list(APPEND ECA_GEN_SOURCES ${GEN}/ppc_recomp_${i}.cpp)
  endforeach()
  list(APPEND ECA_GEN_SOURCES ${GEN}/ppc_table.cpp)

  set(RECOMP_CFG ${CMAKE_SOURCE_DIR}/config/${ECA_GAME}.cfg)
  set(RECOMP_SEEDS ${CMAKE_SOURCE_DIR}/config/${ECA_GAME}_seeds.txt)
  add_custom_command(
    OUTPUT ${ECA_GEN_SOURCES}
    COMMAND ${PPCRECOMP_CMD} --config ${RECOMP_CFG} --crom ${ROMDIR}/crom.bin --seeds ${RECOMP_SEEDS}
            --shards ${ECA_SHARDS} --out ${GEN}
    DEPENDS ${PPCRECOMP_DEP} ${ROMDIR}/crom.bin ${RECOMP_CFG} ${RECOMP_SEEDS}
    COMMENT "Recompiling PowerPC code")
else()
  message(STATUS "ECA: no ROM set at ${ECA_ROMSET}; building the hybrid core without recompiled code")
  list(APPEND ECA_GEN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/empty_table.cpp)
  # The run directory still needs a Games.xml that only lists ECA_GAME (see romtool.cpp)
  set(XMLDIR ${GEN}/xml)
  add_custom_command(
    OUTPUT ${XMLDIR}/Games.xml
    COMMAND ${ROMTOOL_CMD} ${SM}/Config/Games.xml - ${ECA_GAME} ${XMLDIR}
    DEPENDS ${ROMTOOL_DEP} ${SM}/Config/Games.xml
    COMMENT "Writing Games.xml for ${ECA_GAME}")
  add_custom_target(eca_games_xml DEPENDS ${XMLDIR}/Games.xml)
endif()

add_executable(eca ${SM}/Src/OSD/SDL/Main.cpp ppc_core.cpp launcher.cpp hdtex.cpp ${ECA_PATCHED_NEW3D_SOURCES} ${ECA_GEN_SOURCES})
target_include_directories(eca PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SM}/Src/Graphics/New3D ${stb_SOURCE_DIR})
# Built like the rest of Supermodel: C++17 (Util/BitCast.h breaks under C++20), no warnings
set_source_files_properties(${ECA_PATCHED_NEW3D_SOURCES} PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang>:-w;-std=c++17>$<$<CXX_COMPILER_ID:MSVC>:/std:c++17>")
target_link_libraries(eca PRIVATE supermodel_core $<$<BOOL:${WIN32}>:SDL2::SDL2main>)
target_compile_features(eca PRIVATE cxx_std_20)
set_source_files_properties(${ECA_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang>:-fno-strict-aliasing;-Wno-unused-label;-Wno-unused-variable>")

# Portable run directory next to the binary: Supermodel uses ./Config, ./Assets etc. when
# a Config directory exists in the working directory.
set(RUNDIR ${CMAKE_BINARY_DIR}/run)
add_custom_command(TARGET eca POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${RUNDIR}/Config ${RUNDIR}/NVRAM ${RUNDIR}/Saves
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${SM}/Assets ${RUNDIR}/Assets
  COMMAND ${CMAKE_COMMAND} -DINI=${RUNDIR}/Config/Supermodel.ini -DPROFILE=${CMAKE_SOURCE_DIR}/config/${ECA_GAME}_gamepad.ini
          -DSECTION=${ECA_GAME} -P ${CMAKE_SOURCE_DIR}/cmake/install_profile.cmake
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:eca> ${RUNDIR}/)
if(ECA_RECOMPILE AND EXISTS "${ECA_ROMSET}")
  add_custom_command(TARGET eca POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROMDIR}/Games.xml ${RUNDIR}/Config/Games.xml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROMDIR}/${ECA_GAME}.zip ${RUNDIR}/${ECA_GAME}.zip
    # Running ./eca with no arguments opens Supermodel's launcher, which lists ROMs/
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RUNDIR}/ROMs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROMDIR}/${ECA_GAME}.zip ${RUNDIR}/ROMs/${ECA_GAME}.zip)
else()
  add_dependencies(eca eca_games_xml)
  add_custom_command(TARGET eca POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XMLDIR}/Games.xml ${RUNDIR}/Config/Games.xml
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RUNDIR}/ROMs)
endif()
