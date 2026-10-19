// HD texture replacement for Supermodel's New3D renderer.
//
// New3D keeps the Real3D's texture RAM on the GPU as raw 16-bit texels and decodes them
// in the fragment shader, so there are no per-texture GL objects to swap. Instead, each
// mesh draw looks up the texture region it samples (page, x, y, width, height, format),
// hashes that region's texels, and if a texture pack holds a replacement for that hash,
// binds it so the patched shader samples it in place of the texture sheet.
//
// The same hook can dump every texture it sees to PNG, which tools/upscale_textures.py
// turns into a pack. A replacement file is named
//     <format>_<width>x<height>_<hash>.png        (hash: 16 hex digits)
// and can be any size; it is sampled with normal mipmapped filtering.
//
// The game loads texture RAM in 256x256 blocks straight from ROM, so the pack can also
// hold whole upscaled blocks, extracted from the ROM by tools/extract_texture_blocks.py:
//     blocks/blk_<format>_<hash>.png              (hash of the block's 256x256 texels)
// A texture without its own replacement is cut out of the upscaled block(s) it lives in.
// Per-texture replacements win because they were upscaled with wrap-aware padding.
//
// The hooks are spliced into copies of New3D.cpp and R3DShader.cpp at build time by
// cmake/patch_supermodel.cmake, so the Supermodel submodule itself stays unmodified.
#pragma once
#include <cstdint>
#include <string>

namespace Util { namespace Config { class Node; } }

namespace ECATex
{
  // From the CNew3D constructor: reads HDTextures / DumpTextures / TexturePackDirectory /
  // TextureDumpDirectory (and the ECA_HD_TEXTURES / ECA_DUMP_TEXTURES overrides)
  void Configure(const Util::Config::Node &config, const std::string &gameName);

  // From CNew3D::AttachMemory: base of the 2 x 2048x1024 16-bit texture sheets
  void AttachTextureRAM(const uint16_t *textureRAM);

  // From CNew3D::UploadTextures (page-local coordinates), and for whole-sheet uploads
  void OnTextureUpload(int page, unsigned level, unsigned x, unsigned y, unsigned width, unsigned height);
  void OnTextureSheetUpload();

  // From R3DShader::SetMeshUniforms, with the mesh's program current. Binds (or unbinds)
  // the replacement texture and sets the ecaHd* uniforms.
  void BindMesh(unsigned program, bool textured, int page, int x, int y, int width, int height, int format,
                int wrapU, int wrapV);
}
