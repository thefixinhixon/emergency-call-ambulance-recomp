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
