// romtool: load a Model 3 ROM set through Supermodel's GameLoader and write the
// regions the recompiler and Ghidra need.
//
//   romtool <Games.xml> <romset.zip> <game> <out_dir>
//   romtool <Games.xml> - <game> <out_dir>      (only write the filtered Games.xml)
//
// GameLoader always picks the first game it finds in a merged set (e.g. "ecaj"
// from a set that also holds eca/ecap/ecau), so we first write a Games.xml that
// only contains <game> to out_dir and load through that. The runtime uses the
// same filtered file.
//
// Output (all CROM images are true big-endian, i.e. byte order as the PPC sees it):
//   crom.bin         8 MB fixed CROM, maps to 0xFF800000-0xFFFFFFFF (patches applied)
//   banked_crom.bin  banked CROM, maps through the 0xFF000000-0xFF7FFFFF window
//   <region>.bin     every other region exactly as Supermodel loads it
//   manifest.txt     region sizes and CRC32s
//   Games.xml        Supermodel game definitions filtered down to <game>
//   <game>.zip       the ROM set the runtime should load. If <romset.zip> is a
//                    wrapper holding <game>.zip (a common distribution format), the
//                    inner zip is extracted; otherwise the input is copied.
#include "GameLoader.h"
#include "ROMSet.h"
#include "Game.h"
#include "OSD/Logger.h"
#include "Pkgs/tinyxml2.h"
#include "Pkgs/unzip.h"
#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

static bool WriteFile(const std::filesystem::path &path, const std::vector<uint8_t> &data)
{
  FILE *fp = fopen(path.string().c_str(), "wb");
  if (!fp)
    return false;
  bool ok = fwrite(data.data(), 1, data.size(), fp) == data.size();
  fclose(fp);
  return ok;
}

// Writes <game>.zip from inside a wrapper zip. Returns false if there is no such entry.
static bool ExtractNestedZip(const std::string &outer, const std::string &entry, const std::filesystem::path &dest)
{
  unzFile zf = unzOpen(outer.c_str());
  if (!zf)
    return false;
  bool ok = false;
  if (unzLocateFile(zf, entry.c_str(), 2) == UNZ_OK && unzOpenCurrentFile(zf) == UNZ_OK)
  {
    unz_file_info info;
    unzGetCurrentFileInfo(zf, &info, nullptr, 0, nullptr, 0, nullptr, 0);
    std::vector<uint8_t> buf(info.uncompressed_size);
    ok = unzReadCurrentFile(zf, buf.data(), (unsigned)buf.size()) == (int)buf.size() && WriteFile(dest, buf);
    unzCloseCurrentFile(zf);
  }
  unzClose(zf);
  return ok;
}

int main(int argc, char **argv)
{
  if (argc != 5)
  {
    fprintf(stderr, "usage: %s <Games.xml> <romset.zip> <game> <out_dir>\n", argv[0]);
    return 1;
  }
  SetLogger(std::make_shared<CConsoleErrorLogger>());
  const std::string wanted = argv[3];
  std::filesystem::path out_dir(argv[4]);
  std::filesystem::create_directories(out_dir);

  // Filter the game list down to the requested game
  tinyxml2::XMLDocument xml;
  if (xml.LoadFile(argv[1]) != tinyxml2::XML_SUCCESS || !xml.FirstChildElement("games"))
  {
    fprintf(stderr, "romtool: cannot parse %s\n", argv[1]);
    return 1;
  }
  auto *games = xml.FirstChildElement("games");
  bool found = false;
  for (auto *g = games->FirstChildElement("game"); g; )
  {
    auto *next = g->NextSiblingElement("game");
    const char *name = g->Attribute("name");
    if (name && wanted == name)
      found = true;
    else
      games->DeleteChild(g);
    g = next;
  }
  if (!found)
  {
    fprintf(stderr, "romtool: game '%s' not in %s\n", wanted.c_str(), argv[1]);
    return 1;
  }
  auto filtered_xml = (out_dir / "Games.xml").string();
  xml.SaveFile(filtered_xml.c_str());
  if (std::string(argv[2]) == "-")
    return 0;

  auto game_zip = out_dir / (wanted + ".zip");
  if (ExtractNestedZip(argv[2], wanted + ".zip", game_zip))
    printf("romtool: using %s.zip from inside %s\n", wanted.c_str(), argv[2]);
  else
    std::filesystem::copy_file(argv[2], game_zip, std::filesystem::copy_options::overwrite_existing);

  GameLoader loader(filtered_xml);
  Game game;
  ROMSet rom_set;
  if (loader.Load(&game, &rom_set, game_zip.string()))
  {
    fprintf(stderr, "romtool: failed to load %s\n", argv[2]);
    return 1;
  }

  FILE *manifest = fopen((out_dir / "manifest.txt").string().c_str(), "w");
  if (!manifest)
    return 1;
  fprintf(manifest, "game %s (%s)\n", game.name.c_str(), game.title.c_str());

  for (auto &[name, rom] : rom_set.rom_by_region)
  {
    std::vector<uint8_t> raw(rom.size);
    rom.CopyTo(raw.data(), raw.size(), true);
    std::vector<uint8_t> out;
    if (name == "crom")
    {
      // Fixed CROM sits at the top of the 8 MB window, exactly as CModel3::LoadGame places it
      const size_t window = 8 * 0x100000;
      std::vector<uint8_t> full(window, 0);
      memcpy(&full[window - raw.size()], raw.data(), raw.size());
      out = std::move(full);
    }
    else
      out = std::move(raw);   // the loader returns ROM bytes unswapped (big-endian for CROM)

    auto path = out_dir / (name + ".bin");
    if (!WriteFile(path, out))
    {
      fprintf(stderr, "romtool: cannot write %s\n", path.string().c_str());
      return 1;
    }
    uint32_t crc = crc32(0, out.data(), (uInt)out.size());
    fprintf(manifest, "%-20s size=0x%08zX crc32=%08X\n", name.c_str(), out.size(), crc);
    printf("%-20s size=0x%08zX crc32=%08X\n", name.c_str(), out.size(), crc);
  }
  fclose(manifest);
  return 0;
}
