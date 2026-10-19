cmake_minimum_required(VERSION 3.24)
project(eca_recomp C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

set(SM ${CMAKE_SOURCE_DIR}/third_party/supermodel)
option(ECA_VENDOR_SDL "Build SDL2 from source instead of using the system package" ${WIN32})

# --- Dependencies -------------------------------------------------------------
if(ECA_VENDOR_SDL)
  FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-2.32.10 GIT_SHALLOW ON)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL2)
else()
  find_package(SDL2 REQUIRED)
endif()

find_package(SDL2_net QUIET)
if(NOT TARGET SDL2_net::SDL2_net AND NOT TARGET SDL2_net::SDL2_net-static)
  FetchContent_Declare(SDL2_net GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git GIT_TAG release-2.2.0 GIT_SHALLOW ON)
  set(SDL2NET_SAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL2NET_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_MakeAvailable(SDL2_net)
endif()
if(TARGET SDL2_net::SDL2_net-static)
  set(SDL2NET_TARGET SDL2_net::SDL2_net-static)
else()
  set(SDL2NET_TARGET SDL2_net::SDL2_net)
endif()

find_package(OpenGL REQUIRED)
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  FetchContent_Declare(zlib GIT_REPOSITORY https://github.com/madler/zlib.git GIT_TAG v1.3.1 GIT_SHALLOW ON)
  FetchContent_MakeAvailable(zlib)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
  target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
endif()

# --- Musashi 68K core (generated sources) -------------------------------------
# m68kmake runs on the build host; when cross-compiling pass -DM68KMAKE_EXECUTABLE=<host binary>.
set(MUSASHI_DIR ${SM}/Src/CPU/68K/Musashi)
set(MUSASHI_GEN ${CMAKE_BINARY_DIR}/musashi)
if(NOT M68KMAKE_EXECUTABLE)
  add_executable(m68kmake ${MUSASHI_DIR}/m68kmake.c)
  set(M68KMAKE_EXECUTABLE $<TARGET_FILE:m68kmake>)
endif()
set(MUSASHI_OUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h ${MUSASHI_GEN}/m68kopac.c
                ${MUSASHI_GEN}/m68kopdm.c ${MUSASHI_GEN}/m68kopnz.c)
add_custom_command(
  OUTPUT ${MUSASHI_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSASHI_GEN}
  COMMAND ${M68KMAKE_EXECUTABLE} ${MUSASHI_GEN} ${MUSASHI_DIR}/m68k_in.c
  DEPENDS ${MUSASHI_DIR}/m68k_in.c ${MUSASHI_DIR}/m68kmake.c
  COMMENT "Generating Musashi 68K opcode handlers")

# --- Supermodel device library (everything except the PPC core and frontend) --
set(SM_SOURCES
  CPU/PowerPC/PPCDisasm.cpp BlockFile.cpp Pkgs/unzip.c Pkgs/ioapi.c Model3/93C46.cpp Model3/JTAG.cpp
  Pkgs/glew.c Graphics/Shader.cpp Model3/Real3D.cpp
  Graphics/New3D/GLSLShader.cpp Graphics/New3D/R3DFrameBuffers.cpp
  Graphics/New3D/Mat4.cpp Graphics/New3D/Model.cpp Graphics/New3D/PolyHeader.cpp Graphics/New3D/VBO.cpp
  Graphics/New3D/Vec.cpp Graphics/New3D/R3DFloat.cpp
  Graphics/New3D/R3DScrollFog.cpp Graphics/New3D/TextureBank.cpp
  Graphics/FBO.cpp Graphics/Render2D.cpp Graphics/Pro1000Render2D.cpp Graphics/SuperAA.cpp
  Graphics/Legacy3D/Error.cpp Graphics/Legacy3D/Legacy3D.cpp Graphics/Legacy3D/Models.cpp
  Graphics/Legacy3D/TextureRefs.cpp
  Model3/TileGen.cpp Model3/Model3.cpp
  Pro1000/Pro1000.cpp Pro1000/UART.cpp Pro1000/8259.cpp Pro1000/SCANPSC100F.cpp Pro1000/SCSITCPBridge.cpp
  Pro1000/SCSIHostAdapter.cpp Pro1000/SyntheticSCSIROM.cpp
  OSD/SDL/Audio.cpp OSD/SDL/Thread.cpp
  Model3/SoundBoard.cpp Sound/SCSP.cpp Sound/SCSPDSP.cpp CPU/68K/68K.cpp
  Model3/DSB.cpp CPU/Z80/Z80.cpp Model3/IRQ.cpp Model3/53C810.cpp Model3/PCI.cpp Model3/RTC72421.cpp
  Model3/DriveBoard/DriveBoard.cpp Model3/DriveBoard/WheelBoard.cpp Model3/DriveBoard/JoystickBoard.cpp
  Model3/DriveBoard/SkiBoard.cpp Model3/DriveBoard/BillBoard.cpp Model3/DriveBoard/Z80CTC.cpp
  Model3/MPC10x.cpp
  Inputs/Input.cpp Inputs/Inputs.cpp Inputs/InputSource.cpp Inputs/InputSystem.cpp Inputs/InputTypes.cpp
  Inputs/MultiInputSource.cpp OSD/SDL/SDLInputSystem.cpp OSD/SDL/Crosshair.cpp OSD/Outputs.cpp
  Sound/MPEG/MpegAudio.cpp Model3/Crypto.cpp OSD/Logger.cpp OSD/ConsoleInput.cpp
  Util/Format.cpp Util/NewConfig.cpp Util/ByteSwap.cpp Util/ConfigBuilders.cpp GameLoader.cpp
  Pkgs/tinyxml2.cpp Pkgs/imgui/imgui.cpp Pkgs/imgui/imgui_draw.cpp Pkgs/imgui/imgui_impl_sdl2.cpp
  Pkgs/imgui/imgui_impl_opengl3.cpp Pkgs/imgui/imgui_tables.cpp Pkgs/imgui/imgui_widgets.cpp
  ROMSet.cpp OSD/SDL/NetOutputs.cpp Network/TCPReceive.cpp Network/TCPSend.cpp Network/NetBoard.cpp
  Network/SimNetBoard.cpp)
if(WIN32)
  list(APPEND SM_SOURCES OSD/Windows/DirectInputSystem.cpp OSD/Windows/FileSystemPath.cpp OSD/Windows/WinOutputs.cpp)
else()
  list(APPEND SM_SOURCES OSD/Unix/FileSystemPath.cpp)
endif()
list(TRANSFORM SM_SOURCES PREPEND ${SM}/Src/)

add_library(supermodel_core STATIC ${SM_SOURCES}
  ${MUSASHI_DIR}/m68kcpu.c ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kopac.c
  ${MUSASHI_GEN}/m68kopdm.c ${MUSASHI_GEN}/m68kopnz.c)
set(SM_INCLUDE_DIRS ${SM}/Src ${SM}/Src/OSD ${SM}/Src/OSD/SDL ${SM}/Src/Pkgs ${MUSASHI_DIR} ${MUSASHI_GEN})
if(WIN32)
  list(APPEND SM_INCLUDE_DIRS ${SM}/Src/OSD/Windows)
endif()
target_include_directories(supermodel_core PUBLIC ${SM_INCLUDE_DIRS})
target_compile_definitions(supermodel_core PUBLIC GLEW_STATIC $<$<BOOL:${WIN32}>:SUPERMODEL_WIN32>)
set_source_files_properties(${MUSASHI_DIR}/m68kcpu.c ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kopac.c
  ${MUSASHI_GEN}/m68kopdm.c ${MUSASHI_GEN}/m68kopnz.c PROPERTIES COMPILE_DEFINITIONS "INLINE=static inline")
target_link_libraries(supermodel_core PUBLIC SDL2::SDL2 ${SDL2NET_TARGET} OpenGL::GL ZLIB::ZLIB)
if(NOT WIN32)
  target_link_libraries(supermodel_core PUBLIC OpenGL::GLU)
endif()
if(MINGW AND CMAKE_HOST_UNIX)
  # Case-sensitive host: map Windows SDK header spellings onto MinGW's lower-case files
  target_include_directories(supermodel_core PUBLIC ${CMAKE_SOURCE_DIR}/cmake/mingw-compat)
endif()
if(WIN32)
  target_link_libraries(supermodel_core PUBLIC dinput8 dxguid glu32 ole32 oleaut32 wbemuuid ws2_32 iphlpapi)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(supermodel_core PUBLIC Threads::Threads)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(supermodel_core PRIVATE -w)   # third-party code: silence warnings
endif()

# --- Reference build: stock Supermodel with its interpreter (lockstep oracle) --
# Supermodel's own launcher (Gui.cpp) and the stock New3D renderer core live here; eca links
# runtime/launcher.cpp and HD-texture-patched copies of New3D.cpp/R3DShader.cpp instead
add_executable(supermodel_ref ${SM}/Src/OSD/SDL/Main.cpp ${SM}/Src/OSD/SDL/Gui.cpp ${SM}/Src/CPU/PowerPC/ppc.cpp
  ${SM}/Src/Graphics/New3D/New3D.cpp ${SM}/Src/Graphics/New3D/R3DShader.cpp)
target_link_libraries(supermodel_ref PRIVATE supermodel_core $<$<BOOL:${WIN32}>:SDL2::SDL2main>)

# --- HD texture support ---------------------------------------------------------
# stb_image / stb_image_write (public domain) for texture pack PNGs
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(stb)
include(cmake/patch_supermodel.cmake)

# --- Tools --------------------------------------------------------------------
add_subdirectory(tools/romtool)
add_subdirectory(tools/ppcrecomp)

# --- The recompiled game ------------------------------------------------------
add_subdirectory(runtime)
