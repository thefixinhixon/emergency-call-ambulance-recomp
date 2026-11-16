# Splices the HD texture hooks (runtime/hdtex.h) into copies of Supermodel's New3D
# sources. The submodule stays pristine; the patched copies are compiled into the eca
# binary only. Every replacement must match its anchor exactly once, so an upstream
# change that moves the code fails the configure step instead of silently dropping a hook.
#
# Sets ECA_PATCHED_NEW3D_SOURCES and ECA_PATCHED_NEW3D_DIR in the caller's scope.

set(_src ${SM}/Src/Graphics/New3D)
set(_dst ${CMAKE_BINARY_DIR}/sm_patched/Graphics/New3D)
file(MAKE_DIRECTORY ${_dst})

function(_eca_replace_once var anchor replacement what)
  string(FIND "${${var}}" "${anchor}" first)
  if(first EQUAL -1)
    message(FATAL_ERROR "patch_supermodel: anchor for '${what}' not found; Supermodel changed, update cmake/patch_supermodel.cmake")
  endif()
  string(LENGTH "${anchor}" alen)
  math(EXPR after "${first} + ${alen}")
  string(SUBSTRING "${${var}}" ${after} -1 rest)
  string(FIND "${rest}" "${anchor}" second)
  if(NOT second EQUAL -1)
    message(FATAL_ERROR "patch_supermodel: anchor for '${what}' is not unique")
  endif()
  string(REPLACE "${anchor}" "${replacement}" out "${${var}}")
  set(${var} "${out}" PARENT_SCOPE)
endfunction()

function(_eca_write path content)
  if(EXISTS ${path})
    file(READ ${path} old)
    if(old STREQUAL content)
      return()   # keep the timestamp so nothing rebuilds
    endif()
  endif()
  file(WRITE ${path} "${content}")
endfunction()

# --- New3D.cpp: configuration, texture RAM, upload invalidation -----------------------
file(READ ${_src}/New3D.cpp s)
_eca_replace_once(s "#include \"New3D.h\"" "#include \"New3D.h\"\n#include \"hdtex.h\"   // ECA: HD texture replacement" "include")
_eca_replace_once(s
  "\tm_sunClamp\t\t= true;\n\tm_numPolyVerts\t= 3;"
  "\tECATex::Configure(config, gameName);   // ECA\n\tm_sunClamp\t\t= true;\n\tm_numPolyVerts\t= 3;"
  "constructor")
_eca_replace_once(s
  "\tm_textureBank[0].AttachMemory(textureRAMPtr);"
  "\tECATex::AttachTextureRAM(textureRAMPtr);   // ECA\n\tm_textureBank[0].AttachMemory(textureRAMPtr);"
  "AttachMemory")
_eca_replace_once(s
  "\t\theight = 1024;\n"
  "\t\theight = 1024;\n\t\tECATex::OnTextureSheetUpload();   // ECA\n"
  "whole-sheet upload")
_eca_replace_once(s
  "\tTranslateTexture(x, y, width, height, page);\n"
  "\tTranslateTexture(x, y, width, height, page);\n\tECATex::OnTextureUpload(page, level, x, y, width, height);   // ECA\n"
  "upload")
_eca_write(${_dst}/New3D.cpp "${s}")

# --- R3DShader.cpp: bind the replacement for each mesh ---------------------------------
file(READ ${_src}/R3DShader.cpp s)
_eca_replace_once(s "#include \"R3DShaderCommon.h\"" "#include \"R3DShaderCommon.h\"\n#include \"hdtex.h\"   // ECA" "include")
_eca_replace_once(s
  "\n\tm_dirtyMesh = false;\n}"
  "\n\tECATex::BindMesh(m_shaderProgram, m->textured, m->page ^ m_transPage, m->x + m_transX, m->y + m_transY,\n\t                  m->width, m->height, m->format, m->wrapModeU, m->wrapModeV);   // ECA\n\n\tm_dirtyMesh = false;\n}"
  "SetMeshUniforms")
_eca_write(${_dst}/R3DShader.cpp "${s}")

# --- R3DShaderCommon.h: sample the replacement instead of the texture sheet ------------
file(READ ${_src}/R3DShaderCommon.h s)
_eca_replace_once(s
  "static const char* fragmentShaderR3DCommon = R\"glsl(\n"
  "static const char* fragmentShaderR3DCommon = R\"glsl(\n\nuniform sampler2D ecaHdTex;\t\t// ECA: HD replacement texture (runtime/hdtex.cpp)\nuniform bool ecaHdEnabled;\nuniform float ecaHdLodBias;\t\t// ECA: log2(HD texture size / original size)\n"
  "uniforms")
_eca_replace_once(s
  "\tvec4 tex1Data = texBiLinear(textureBank[texturePage], textureWrapMode, vec2(tex1Size), tex1Pos, fsTexCoord, iLevel);\n"
  "\tvec4 tex1Data = texBiLinear(textureBank[texturePage], textureWrapMode, vec2(tex1Size), tex1Pos, fsTexCoord, iLevel);\n\tif (ecaHdEnabled) {\t// ECA: HD replacement texture\n\t\t// Level of detail comes from the Real3D's own per-fragment LOD (no derivatives: quad\n\t\t// rendering computes fsTexCoord per pixel, so helper pixels outside the polygon would\n\t\t// give garbage gradients at every edge), shifted by how much larger the HD texture is.\n\t\t// Wrap mode 1 repeats without filtering across the edge: the sampler clamps and the\n\t\t// coordinate repeats here. Modes 0 and 2/3 use GL_REPEAT / GL_MIRRORED_REPEAT.\n\t\tvec2 hdUV = fsTexCoord;\n\t\tif (textureWrapMode.x == 1) hdUV.x = fract(hdUV.x);\n\t\tif (textureWrapMode.y == 1) hdUV.y = fract(hdUV.y);\n\t\ttex1Data = textureLod(ecaHdTex, hdUV, max(lod + ecaHdLodBias, 0.0));\n\t}\n"
  "texture fetch")
_eca_replace_once(s
  "\tif (ffL > 0.0)\n"
  "\tif (ffL > 0.0 && !ecaHdEnabled)\t// ECA: the HD texture has its own mipmaps\n"
  "mip blend")
_eca_write(${_dst}/R3DShaderCommon.h "${s}")

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${_src}/New3D.cpp ${_src}/R3DShader.cpp ${_src}/R3DShaderCommon.h ${CMAKE_CURRENT_LIST_FILE})

set(ECA_PATCHED_NEW3D_SOURCES ${_dst}/New3D.cpp ${_dst}/R3DShader.cpp)
set(ECA_PATCHED_NEW3D_DIR ${_dst})

# --- Inputs: raise the joystick axis cap so wheels with 9+ axes can bind --------
# Supermodel caps joysticks at 8 axes (X Y Z RX RY RZ S1 S2). Wheels like the
# G29 in separate-pedal mode report the brake on an SDL axis index beyond that,
# so it can never bind (issue #2). Patched in place at configure time so every
# translation unit sees the same definitions; anchors fail loudly if upstream
# changes the code. Adds axes S3..S6 (AXIS9..AXIS12 + SLIDER3..SLIDER6 aliases).
# NOTE: this modifies the submodule working tree; re-applied automatically.
set(_isrc ${SM}/Src/Inputs)

# NOTE: CMake file(READ) normalizes CRLF to LF, so the on-disk EOL style is
# undetectable after reading; these Supermodel files are CRLF upstream, which
# is restored on write to keep the submodule diff minimal.
file(READ ${_isrc}/InputSystem.h ish)
if(ish MATCHES "ECA: was 8")
  message(STATUS "patch_supermodel: InputSystem.h already patched")
else()
_eca_replace_once(ish "#define NUM_JOY_AXES 8" "#define NUM_JOY_AXES 12  // ECA: was 8; G29 separate pedals need AXIS9+" "NUM_JOY_AXES")
_eca_replace_once(ish
  "#define AXIS_S1 6\n#define AXIS_S2 7"
  "#define AXIS_S1 6\n#define AXIS_S2 7\n#define AXIS_S3 8   // ECA\n#define AXIS_S4 9   // ECA\n#define AXIS_S5 10  // ECA\n#define AXIS_S6 11  // ECA"
  "AXIS_S3-S6")
_eca_replace_once(ish
  "  JoyS2Axis,\n  JoyS2AxisInv,\n  JoyS2AxisPos,\n  JoyS2AxisNeg,\n  JoyPOV0Up,"
  "  JoyS2Axis,\n  JoyS2AxisInv,\n  JoyS2AxisPos,\n  JoyS2AxisNeg,\n  JoyS3Axis,   // ECA\n  JoyS3AxisInv,   // ECA\n  JoyS3AxisPos,   // ECA\n  JoyS3AxisNeg,   // ECA\n  JoyS4Axis,   // ECA\n  JoyS4AxisInv,   // ECA\n  JoyS4AxisPos,   // ECA\n  JoyS4AxisNeg,   // ECA\n  JoyS5Axis,   // ECA\n  JoyS5AxisInv,   // ECA\n  JoyS5AxisPos,   // ECA\n  JoyS5AxisNeg,   // ECA\n  JoyS6Axis,   // ECA\n  JoyS6AxisInv,   // ECA\n  JoyS6AxisPos,   // ECA\n  JoyS6AxisNeg,   // ECA\n  JoyPOV0Up,"
  "EJoyPart S3-S6")
  string(REPLACE "\n" "\r\n" ish "${ish}")
  file(WRITE ${_isrc}/InputSystem.h "${ish}")
  message(STATUS "patch_supermodel: patched InputSystem.h (12 joystick axes)")
endif()

file(READ ${_isrc}/InputSystem.cpp isc)
if(isc MATCHES "Slider 3 \\(Axis 9\\) - ECA")
  message(STATUS "patch_supermodel: InputSystem.cpp already patched")
else()
_eca_replace_once(isc
  "  // Slider 2 (Axis 8)\n  { \"SLIDER2\",       JoyS2Axis },\n  { \"SLIDER2_INV\",   JoyS2AxisInv },\n  { \"SLIDER2_POS\",   JoyS2AxisPos },\n  { \"SLIDER2_NEG\",   JoyS2AxisNeg },\n  { \"AXIS8\",         JoyS2Axis },\n  { \"AXIS8_INV\",     JoyS2AxisInv },\n  { \"AXIS8_POS\",     JoyS2AxisPos },\n  { \"AXIS8_NEG\",     JoyS2AxisNeg },"
  "  // Slider 2 (Axis 8)\n  { \"SLIDER2\",       JoyS2Axis },\n  { \"SLIDER2_INV\",   JoyS2AxisInv },\n  { \"SLIDER2_POS\",   JoyS2AxisPos },\n  { \"SLIDER2_NEG\",   JoyS2AxisNeg },\n  { \"AXIS8\",         JoyS2Axis },\n  { \"AXIS8_INV\",     JoyS2AxisInv },\n  { \"AXIS8_POS\",     JoyS2AxisPos },\n  { \"AXIS8_NEG\",     JoyS2AxisNeg },\n\n  // Slider 3 (Axis 9) - ECA\n  { \"SLIDER3\",       JoyS3Axis },\n  { \"SLIDER3_INV\",   JoyS3AxisInv },\n  { \"SLIDER3_POS\",   JoyS3AxisPos },\n  { \"SLIDER3_NEG\",   JoyS3AxisNeg },\n  { \"AXIS9\",         JoyS3Axis },\n  { \"AXIS9_INV\",     JoyS3AxisInv },\n  { \"AXIS9_POS\",     JoyS3AxisPos },\n  { \"AXIS9_NEG\",     JoyS3AxisNeg },\n\n  // Slider 4 (Axis 10) - ECA\n  { \"SLIDER4\",       JoyS4Axis },\n  { \"SLIDER4_INV\",   JoyS4AxisInv },\n  { \"SLIDER4_POS\",   JoyS4AxisPos },\n  { \"SLIDER4_NEG\",   JoyS4AxisNeg },\n  { \"AXIS10\",        JoyS4Axis },\n  { \"AXIS10_INV\",    JoyS4AxisInv },\n  { \"AXIS10_POS\",    JoyS4AxisPos },\n  { \"AXIS10_NEG\",    JoyS4AxisNeg },\n\n  // Slider 5 (Axis 11) - ECA\n  { \"SLIDER5\",       JoyS5Axis },\n  { \"SLIDER5_INV\",   JoyS5AxisInv },\n  { \"SLIDER5_POS\",   JoyS5AxisPos },\n  { \"SLIDER5_NEG\",   JoyS5AxisNeg },\n  { \"AXIS11\",        JoyS5Axis },\n  { \"AXIS11_INV\",    JoyS5AxisInv },\n  { \"AXIS11_POS\",    JoyS5AxisPos },\n  { \"AXIS11_NEG\",    JoyS5AxisNeg },\n\n  // Slider 6 (Axis 12) - ECA\n  { \"SLIDER6\",       JoyS6Axis },\n  { \"SLIDER6_INV\",   JoyS6AxisInv },\n  { \"SLIDER6_POS\",   JoyS6AxisPos },\n  { \"SLIDER6_NEG\",   JoyS6AxisNeg },\n  { \"AXIS12\",        JoyS6Axis },\n  { \"AXIS12_INV\",    JoyS6AxisInv },\n  { \"AXIS12_POS\",    JoyS6AxisPos },\n  { \"AXIS12_NEG\",    JoyS6AxisNeg },"
  "joy part table")
_eca_replace_once(isc
  "const char *CInputSystem::s_axisIds[] = { \"X\", \"Y\", \"Z\", \"RX\", \"RY\", \"RZ\", \"S1\", \"S2\" };"
  "const char *CInputSystem::s_axisIds[] = { \"X\", \"Y\", \"Z\", \"RX\", \"RY\", \"RZ\", \"S1\", \"S2\", \"S3\", \"S4\", \"S5\", \"S6\" };  // ECA"
  "s_axisIds")
_eca_replace_once(isc
  "const char *CInputSystem::s_axisNames[] = { \"X-Axis\", \"Y-Axis\", \"Z-Axis\", \"RX-Axis\", \"RY-Axis\", \"RZ-Axis\", \"Slider1\", \"Slider2\" };"
  "const char *CInputSystem::s_axisNames[] = { \"X-Axis\", \"Y-Axis\", \"Z-Axis\", \"RX-Axis\", \"RY-Axis\", \"RZ-Axis\", \"Slider1\", \"Slider2\", \"Slider3\", \"Slider4\", \"Slider5\", \"Slider6\" };  // ECA"
  "s_axisNames")
  string(REPLACE "\n" "\r\n" isc "${isc}")
  file(WRITE ${_isrc}/InputSystem.cpp "${isc}")
  message(STATUS "patch_supermodel: patched InputSystem.cpp (AXIS9-12 bindings)")
endif()

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${_isrc}/InputSystem.h ${_isrc}/InputSystem.cpp)
