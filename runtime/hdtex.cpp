// HD texture replacement for New3D. See hdtex.h for how it fits together.
#include "hdtex.h"

#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Util/NewConfig.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace
{
  constexpr int kHdUnit = 4;                       // texture unit for replacements (New3D uses 0 and 1)
  constexpr int kSheetW = 2048, kSheetH = 1024;    // one texture page
  constexpr int kBlock = 256;                      // the game uploads texture RAM in 256x256 blocks
  constexpr int kBlocksX = kSheetW / kBlock, kBlocksY = kSheetH / kBlock;
  constexpr size_t kMaxDecodedBlocks = 160;        // ~640 MB of RGBA at 4x

  bool g_verbose = false;     // ECA_HDTEX_VERBOSE: report block matches
  bool g_enabled = false;     // sample replacements
  bool g_dump = false;        // write every texture seen to the dump directory
  std::string g_packDir, g_dumpDir;
  const uint16_t *g_ram = nullptr;

  std::mutex g_mutex;         // everything below except the decoder's own state

  struct Region
  {
    uint64_t hash = 0;
    std::string name;          // <format>_<w>x<h>_<hash>
    GLuint tex = 0;            // replacement (own file or cut from blocks), or 0
    bool cut = false;          // tex was cut from blocks and belongs to this region
    bool pending = false;      // waiting for block decodes; retry on the next draw
  };
  std::unordered_map<uint64_t, Region> g_regions;          // keyed by PackRegion()
  std::unordered_set<std::string> g_packFiles;             // per-texture replacements available
  std::unordered_map<uint64_t, unsigned> g_packBlocks;     // block hash -> bitmask of formats available
  std::unordered_map<std::string, GLuint> g_loaded;        // per-texture file -> GL texture
  std::unordered_map<GLuint, int> g_texWrap;               // last wrap state set on each texture
  std::unordered_map<GLuint, int> g_texWidth;              // width of each replacement texture
  std::unordered_set<std::string> g_dumped;                // names already in the dump directory
  uint64_t g_blockHash[2][kBlocksY][kBlocksX];             // content of each resident block, 0 = unknown
  float g_maxAniso = 0;

  struct ProgramState
  {
    GLint locEnabled = -1;
    GLint locLodBias = -1;
    int enabled = -1;
    float lodBias = -1000;
  };
  std::unordered_map<unsigned, ProgramState> g_programs;

  // --- Background decoding of block PNGs ------------------------------------------

  struct Decoded
  {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
  };
  using DecodedPtr = std::shared_ptr<const Decoded>;

  class BlockDecoder
  {
  public:
    // Returns the decoded block, or nullptr after queueing it if it is not ready yet
    DecodedPtr Get(uint64_t hash, int format)
    {
      uint64_t key = hash ^ ((uint64_t)format << 60);
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = m_done.find(key);
      if (it != m_done.end())
      {
        m_lru.splice(m_lru.begin(), m_lru, it->second.second);
        return it->second.first;
      }
      if (m_queued.insert(key).second)
      {
        char name[64];
        snprintf(name, sizeof(name), "blk_%d_%016llx.png", format, (unsigned long long)hash);
        m_queue.push_back({key, (fs::path(m_dir) / name).string()});
        m_cv.notify_one();
        if (!m_thread.joinable())
          m_thread = std::thread([this] { Run(); });
      }
      return nullptr;
    }

    void SetDirectory(const std::string &dir)
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_dir = dir;
      m_done.clear();
      m_lru.clear();
      m_queued.clear();
      m_queue.clear();
    }

  private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    std::string m_dir;
    std::deque<std::pair<uint64_t, std::string>> m_queue;
    std::unordered_set<uint64_t> m_queued;
    std::list<uint64_t> m_lru;
    std::unordered_map<uint64_t, std::pair<DecodedPtr, std::list<uint64_t>::iterator>> m_done;

    void Run()
    {
      while (true)
      {
        std::pair<uint64_t, std::string> job;
        {
          std::unique_lock<std::mutex> lock(m_mutex);
          m_cv.wait(lock, [this] { return !m_queue.empty(); });
          job = m_queue.front();
          m_queue.pop_front();
        }
        auto d = std::make_shared<Decoded>();
        int n;
        if (stbi_uc *px = stbi_load(job.second.c_str(), &d->w, &d->h, &n, 4))
        {
          d->rgba.assign(px, px + (size_t)d->w * d->h * 4);
          stbi_image_free(px);
        }
        else
          fprintf(stderr, "[hdtex] cannot load %s\n", job.second.c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_queued.erase(job.first))
          continue;   // directory changed meanwhile
        m_lru.push_front(job.first);
        m_done[job.first] = {d, m_lru.begin()};
        while (m_lru.size() > kMaxDecodedBlocks)
        {
          m_done.erase(m_lru.back());
          m_lru.pop_back();
        }
      }
    }
  };
  // Leaked on purpose: the worker thread must outlive static destruction at exit
  BlockDecoder &Decoder()
  {
    static BlockDecoder *d = new BlockDecoder;
    return *d;
  }

  // --- Helpers ------------------------------------------------------------------

  uint64_t PackRegion(int page, int x, int y, int w, int h, int format)
  {
    return (uint64_t)(page & 1) | (uint64_t)(x & 0x7FF) << 1 | (uint64_t)(y & 0x3FF) << 12 |
           (uint64_t)(w & 0xFFF) << 22 | (uint64_t)(h & 0xFFF) << 34 | (uint64_t)(format & 0xF) << 46;
  }

  const uint16_t *Texel(int page, int x, int y)
  {
    return &g_ram[(size_t)page * kSheetW * kSheetH + (size_t)(y & (kSheetH - 1)) * kSheetW + (x & (kSheetW - 1))];
  }

  // 64-bit hash of a texture's texels (rows wrap around the sheet like the shader does).
  // tools/extract_texture_blocks.py implements the same function; keep them in sync.
  uint64_t HashRegion(int page, int x, int y, int w, int h)
  {
    uint64_t hsh = 0x9E3779B97F4A7C15ull ^ ((uint64_t)w << 32 | (uint64_t)h);
    for (int row = 0; row < h; row++)
    {
      for (int col = 0; col < w; col++)
      {
        hsh ^= *Texel(page, x + col, y + row);
        hsh *= 0x100000001B3ull;                // FNV-1a step per texel
      }
      hsh = (hsh ^ (hsh >> 29)) * 0xBF58476D1CE4E5B9ull;   // mix per row
    }
    return hsh ^ (hsh >> 32);
  }

  // CPU copy of ExtractColour() in R3DShaderCommon.h (also in extract_texture_blocks.py)
  void Decode(int type, uint16_t v, uint8_t *rgba)
  {
    auto n4 = [](unsigned x) { return (uint8_t)((x & 0xF) * 17); };
    auto n5 = [](unsigned x) { return (uint8_t)(((x & 0x1F) * 255 + 15) / 31); };
    auto lumKey = [&](unsigned l, unsigned max) {
      uint8_t c = (uint8_t)(l * 255 / max);
      rgba[0] = rgba[1] = rgba[2] = c;
      rgba[3] = (l == max) ? 0 : 255;    // full white is the transparent key
    };
    switch (type)
    {
    case 0: rgba[0] = n5(v >> 10); rgba[1] = n5(v >> 5); rgba[2] = n5(v); rgba[3] = (v & 0x8000) ? 0 : 255; break;
    case 1: rgba[0] = rgba[1] = rgba[2] = n4(v); rgba[3] = n4(v >> 4); break;
    case 2: rgba[3] = n4(v); rgba[0] = rgba[1] = rgba[2] = n4(v >> 4); break;
    case 3: rgba[0] = rgba[1] = rgba[2] = n4(v >> 8); rgba[3] = n4(v >> 12); break;
    case 4: rgba[3] = n4(v >> 8); rgba[0] = rgba[1] = rgba[2] = n4(v >> 12); break;
    case 5: lumKey(v & 0xFF, 255); break;
    case 6: lumKey((v >> 8) & 0xFF, 255); break;
    case 7: rgba[0] = n4(v >> 12); rgba[1] = n4(v >> 8); rgba[2] = n4(v >> 4); rgba[3] = n4(v); break;
    case 8: lumKey(v & 0xF, 15); break;
    case 9: lumKey((v >> 4) & 0xF, 15); break;
    case 10: lumKey((v >> 8) & 0xF, 15); break;
    case 11: lumKey((v >> 12) & 0xF, 15); break;
    default: rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; break;
    }
  }

  void Dump(const Region &r, int page, int x, int y, int w, int h, int format, int wrapU, int wrapV)
  {
    if (g_dumped.count(r.name))
      return;
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int row = 0; row < h; row++)
      for (int col = 0; col < w; col++)
        Decode(format, *Texel(page, x + col, y + row), &px[((size_t)row * w + col) * 4]);
    std::error_code ec;
    fs::create_directories(g_dumpDir, ec);
    std::string path = (fs::path(g_dumpDir) / (r.name + ".png")).string();
    if (stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4))
    {
      // Wrap modes tell the upscaler how to pad the edges so tiling stays seamless
      std::ofstream idx(fs::path(g_dumpDir) / "wrap.txt", std::ios::app);
      idx << r.name << ' ' << wrapU << ' ' << wrapV << '\n';
    }
    g_dumped.insert(r.name);
  }

  // Fully transparent texels keep whatever colour the source had (often the white key).
  // Hardware bilinear filtering and generated mipmaps would blend that into visible edges,
  // so copy colour in from opaque neighbours first. Alpha is untouched.
  void BleedTransparent(std::vector<uint8_t> &px, int w, int h)
  {
    std::vector<uint8_t> known((size_t)w * h);
    bool any = false, all = true;
    for (size_t i = 0; i < known.size(); i++)
    {
      known[i] = px[i * 4 + 3] != 0;
      any |= known[i] != 0;
      all &= known[i] != 0;
    }
    if (!any || all)
      return;
    std::vector<uint8_t> next;
    for (int pass = 0; pass < 8; pass++)
    {
      next = known;
      bool changed = false;
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
          size_t i = (size_t)y * w + x;
          if (known[i])
            continue;
          int sum[3] = {0, 0, 0}, n = 0;
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
            {
              int xx = x + dx, yy = y + dy;
              if ((dx || dy) && xx >= 0 && yy >= 0 && xx < w && yy < h && known[(size_t)yy * w + xx])
              {
                const uint8_t *q = &px[((size_t)yy * w + xx) * 4];
                sum[0] += q[0], sum[1] += q[1], sum[2] += q[2], n++;
              }
            }
          if (n)
          {
            for (int c = 0; c < 3; c++)
              px[i * 4 + c] = (uint8_t)(sum[c] / n);
            next[i] = 1;
            changed = true;
          }
        }
      known.swap(next);
      if (!changed)
        break;
    }
  }

  GLuint CreateTexture(int w, int h, const uint8_t *src)
  {
    std::vector<uint8_t> px(src, src + (size_t)w * h * 4);
    BleedTransparent(px, w, h);
    const uint8_t *rgba = px.data();
    GLint prevUnit, prevTex;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0 + kHdUnit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (g_maxAniso > 1)
      glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, std::min(16.0f, g_maxAniso));
    glBindTexture(GL_TEXTURE_2D, prevTex);
    glActiveTexture(prevUnit);
    g_texWidth[tex] = w;
    return tex;
  }

  GLuint LoadFile(const std::string &name)
  {
    auto it = g_loaded.find(name);
    if (it != g_loaded.end())
      return it->second;
    GLuint tex = 0;
    int w, h, n;
    std::string path = (fs::path(g_packDir) / (name + ".png")).string();
    if (stbi_uc *px = stbi_load(path.c_str(), &w, &h, &n, 4))
    {
      tex = CreateTexture(w, h, px);
      stbi_image_free(px);
    }
    else
      fprintf(stderr, "[hdtex] cannot load %s\n", path.c_str());
    g_loaded[name] = tex;
    return tex;
  }

  // Cuts a texture out of the upscaled block(s) it lies in. Returns 0 and sets pending
  // while any block is still being decoded; 0 without pending if blocks don't cover it.
  GLuint CutFromBlocks(int page, int x, int y, int w, int h, int format, bool &pending)
  {
    pending = false;
    if (g_packBlocks.empty() || x < 0 || y < 0 || x + w > kSheetW || y + h > kSheetH)
      return 0;
    int bx0 = x / kBlock, by0 = y / kBlock, bx1 = (x + w - 1) / kBlock, by1 = (y + h - 1) / kBlock;
    std::vector<DecodedPtr> blocks;
    int scale = 0;
    for (int by = by0; by <= by1; by++)
      for (int bx = bx0; bx <= bx1; bx++)
      {
        uint64_t hash = g_blockHash[page][by][bx];
        auto avail = g_packBlocks.find(hash);
        if (!hash || avail == g_packBlocks.end() || !(avail->second & (1u << format)))
          return 0;
        DecodedPtr d = Decoder().Get(hash, format);
        if (!d)
        {
          pending = true;
          continue;
        }
        if (d->rgba.empty() || d->w != d->h || d->w % kBlock)
          return 0;
        if (scale && d->w / kBlock != scale)
          return 0;
        scale = d->w / kBlock;
        blocks.push_back(d);
      }
    if (pending)
      return 0;

    int W = w * scale, H = h * scale;
    std::vector<uint8_t> out((size_t)W * H * 4);
    size_t i = 0;
    for (int by = by0; by <= by1; by++)
      for (int bx = bx0; bx <= bx1; bx++, i++)
      {
        const Decoded &d = *blocks[i];
        // Part of this block inside the texture, in texels
        int sx0 = std::max(x, bx * kBlock), sx1 = std::min(x + w, (bx + 1) * kBlock);
        int sy0 = std::max(y, by * kBlock), sy1 = std::min(y + h, (by + 1) * kBlock);
        for (int yy = sy0 * scale; yy < sy1 * scale; yy++)
        {
          const uint8_t *src = &d.rgba[((size_t)(yy - by * kBlock * scale) * d.w + (sx0 - bx * kBlock) * scale) * 4];
          uint8_t *dst = &out[((size_t)(yy - y * scale) * W + (sx0 - x) * scale) * 4];
          memcpy(dst, src, (size_t)(sx1 - sx0) * scale * 4);
        }
      }
    return CreateTexture(W, H, out.data());
  }

  GLenum WrapToGL(int mode)
  {
    // New3D: 0 repeat, 1 repeat without filtering across the edge (the shader repeats the
    // coordinate, the sampler clamps), 2/3 mirror
    return mode >= 2 ? GL_MIRRORED_REPEAT : mode == 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT;
  }

  bool Overlaps(uint64_t key, int page, unsigned x, unsigned y, unsigned w, unsigned h)
  {
    int rp = (int)(key & 1);
    unsigned rx = (key >> 1) & 0x7FF, ry = (key >> 12) & 0x3FF, rw = (key >> 22) & 0xFFF, rh = (key >> 34) & 0xFFF;
    return rp == page && rx < x + w && x < rx + rw && ry < y + h && y < ry + rh;
  }

  bool EnvFlag(const char *name, bool fallback)
  {
    const char *v = getenv(name);
    if (!v)
      return fallback;
    return *v == '1' || *v == 't' || *v == 'T' || *v == 'y' || *v == 'Y';
  }

  void ForgetBlocks()
  {
    memset(g_blockHash, 0, sizeof(g_blockHash));
  }

  // Cut textures are owned by their region; per-texture files are shared via g_loaded
  void ReleaseRegion(Region &r)
  {
    if (r.cut && r.tex)
    {
      glDeleteTextures(1, &r.tex);
      g_texWrap.erase(r.tex);
      g_texWidth.erase(r.tex);
    }
    r.tex = 0;
    r.cut = false;
  }

  void ClearRegions()
  {
    for (auto &kv : g_regions)
      ReleaseRegion(kv.second);
    g_regions.clear();
  }
}

namespace ECATex
{
  void Configure(const Util::Config::Node &config, const std::string &gameName)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto get = [&](const char *key, const std::string &fallback) {
      const Util::Config::Node *n = config.TryGet(key);
      return (n && n->Exists()) ? n->ValueAs<std::string>() : fallback;
    };
    auto flag = [&](const char *key) {
      std::string v = get(key, "false");
      return v == "true" || v == "1";
    };
    g_enabled = EnvFlag("ECA_HD_TEXTURES", flag("HDTextures"));
    g_dump = EnvFlag("ECA_DUMP_TEXTURES", flag("DumpTextures"));
    g_verbose = EnvFlag("ECA_HDTEX_VERBOSE", false);
    g_packDir = get("TexturePackDirectory", "Textures/" + gameName);
    g_dumpDir = get("TextureDumpDirectory", "Textures/" + gameName + "_dump");

    // A new renderer means a new GL context: earlier texture names are gone
    g_regions.clear();
    g_loaded.clear();
    g_texWrap.clear();
    g_texWidth.clear();
    g_packFiles.clear();
    g_packBlocks.clear();
    g_dumped.clear();
    g_programs.clear();
    ForgetBlocks();

    std::error_code ec;
    if (g_enabled)
    {
      for (auto &e : fs::directory_iterator(g_packDir, ec))
        if (e.path().extension() == ".png")
          g_packFiles.insert(e.path().stem().string());
      std::string blockDir = (fs::path(g_packDir) / "blocks").string();
      for (auto &e : fs::directory_iterator(blockDir, ec))
      {
        int fmt;
        unsigned long long hash;
        if (e.path().extension() == ".png" && sscanf(e.path().stem().string().c_str(), "blk_%d_%llx", &fmt, &hash) == 2 &&
            fmt >= 0 && fmt < 16)
          g_packBlocks[hash] |= 1u << fmt;
      }
      Decoder().SetDirectory(blockDir);
    }
    if (g_dump)
      for (auto &e : fs::directory_iterator(g_dumpDir, ec))
        if (e.path().extension() == ".png")
          g_dumped.insert(e.path().stem().string());

    if (glewIsSupported("GL_EXT_texture_filter_anisotropic") || glewIsSupported("GL_ARB_texture_filter_anisotropic"))
      glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &g_maxAniso);

    if (g_enabled || g_dump)
      fprintf(stderr, "[hdtex] HD textures %s (%zu textures, %zu blocks in %s), dumping %s%s\n", g_enabled ? "on" : "off",
              g_packFiles.size(), g_packBlocks.size(), g_packDir.c_str(), g_dump ? "to " : "off",
              g_dump ? g_dumpDir.c_str() : "");
  }

  void AttachTextureRAM(const uint16_t *textureRAM)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ram = textureRAM;
    ClearRegions();
    ForgetBlocks();
  }

  void OnTextureUpload(int page, unsigned level, unsigned x, unsigned y, unsigned width, unsigned height)
  {
    if (!g_enabled && !g_dump)
      return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_regions.begin(); it != g_regions.end();)
    {
      if (Overlaps(it->first, page, x, y, width, height))
      {
        ReleaseRegion(it->second);
        it = g_regions.erase(it);
      }
      else
        ++it;
    }

    if (!g_enabled || !g_ram || level != 0)
      return;
    // Remember which ROM block now occupies each 256x256 cell this upload touched
    for (unsigned by = y / kBlock; by <= (y + height - 1) / kBlock && by < (unsigned)kBlocksY; by++)
      for (unsigned bx = x / kBlock; bx <= (x + width - 1) / kBlock && bx < (unsigned)kBlocksX; bx++)
      {
        bool whole = x == bx * kBlock && y == by * kBlock && width == kBlock && height == kBlock;
        uint64_t hash = whole ? HashRegion(page, x, y, kBlock, kBlock) : 0;
        g_blockHash[page][by][bx] = hash;
        if (g_verbose && whole)
          fprintf(stderr, "[hdtex] block page %d (%4u,%4u) %016llx %s\n", page, x, y, (unsigned long long)hash,
                  g_packBlocks.count(hash) ? "in pack" : "NOT in pack");
      }
  }

  void OnTextureSheetUpload()
  {
    if (!g_enabled && !g_dump)
      return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ClearRegions();
    ForgetBlocks();
  }

  void BindMesh(unsigned program, bool textured, int page, int x, int y, int width, int height, int format,
                int wrapU, int wrapV)
  {
    if (!g_enabled && !g_dump)
      return;
    std::lock_guard<std::mutex> lock(g_mutex);

    ProgramState &ps = g_programs[program];
    if (ps.enabled < 0)
    {
      ps.locEnabled = glGetUniformLocation(program, "ecaHdEnabled");
      ps.locLodBias = glGetUniformLocation(program, "ecaHdLodBias");
      glUniform1i(glGetUniformLocation(program, "ecaHdTex"), kHdUnit);
    }

    GLuint tex = 0;
    if (textured && g_ram && width > 0 && height > 0)
    {
      uint64_t key = PackRegion(page, x, y, width, height, format);
      auto it = g_regions.find(key);
      if (it == g_regions.end())
      {
        Region r;
        r.hash = HashRegion(page, x, y, width, height);
        char name[64];
        snprintf(name, sizeof(name), "%d_%dx%d_%016llx", format, width, height, (unsigned long long)r.hash);
        r.name = name;
        if (g_enabled && g_packFiles.count(r.name))
          r.tex = LoadFile(r.name);
        else if (g_enabled)
        {
          r.tex = CutFromBlocks(page, x, y, width, height, format, r.pending);
          r.cut = r.tex != 0;
        }
        if (g_dump)
          Dump(r, page, x, y, width, height, format, wrapU, wrapV);
        it = g_regions.emplace(key, std::move(r)).first;
      }
      else if (it->second.pending)
      {
        it->second.tex = CutFromBlocks(page, x, y, width, height, format, it->second.pending);
        it->second.cut = it->second.tex != 0;
      }
      tex = g_enabled ? it->second.tex : 0;
    }

    if (tex)
    {
      GLint prevUnit;
      glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
      glActiveTexture(GL_TEXTURE0 + kHdUnit);
      glBindTexture(GL_TEXTURE_2D, tex);
      int wrap = wrapU | wrapV << 4;
      auto w = g_texWrap.find(tex);
      if (w == g_texWrap.end() || w->second != wrap)
      {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, WrapToGL(wrapU));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, WrapToGL(wrapV));
        g_texWrap[tex] = wrap;
      }
      glActiveTexture(prevUnit);
      float bias = std::log2((float)g_texWidth[tex] / (float)width);
      if (bias != ps.lodBias)
      {
        glUniform1f(ps.locLodBias, bias);
        ps.lodBias = bias;
      }
    }
    int enabled = tex ? 1 : 0;
    if (enabled != ps.enabled)
    {
      glUniform1i(ps.locEnabled, enabled);
      ps.enabled = enabled;
    }
  }
}
