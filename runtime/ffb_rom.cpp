// See ffb_rom.h.
#include "ffb_rom.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Pkgs/unzip.h"
#include <zlib.h>

namespace EcaFfb
{
namespace
{
namespace fs = std::filesystem;

std::string Lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

void Put16(std::vector<uint8_t> &o, uint32_t v)
{
  o.push_back((uint8_t)(v & 0xFF));
  o.push_back((uint8_t)((v >> 8) & 0xFF));
}

void Put32(std::vector<uint8_t> &o, uint32_t v)
{
  o.push_back((uint8_t)(v & 0xFF));
  o.push_back((uint8_t)((v >> 8) & 0xFF));
  o.push_back((uint8_t)((v >> 16) & 0xFF));
  o.push_back((uint8_t)((v >> 24) & 0xFF));
}

void Put16(std::ostream &o, uint32_t v)
{
  uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF)};
  o.write((char *)b, 2);
}

void Put32(std::ostream &o, uint32_t v)
{
  uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF),
                   (uint8_t)((v >> 24) & 0xFF)};
  o.write((char *)b, 4);
}

// Central-directory entry of an existing zip member, with its compressed
// bytes so the rebuilt zip can preserve the original compression.
struct KeptEntry
{
  std::string name;
  uint32_t method = 0;
  uint32_t flag = 0;
  uint32_t dosDate = 0;
  uint32_t crc = 0;
  uint32_t compSize = 0;
  uint32_t uncompSize = 0;
  uint32_t internalFa = 0;
  uint32_t externalFa = 0;
  std::vector<uint8_t> compData;
};

bool ReadEntryRaw(unzFile zf, const unz_file_info &info, const std::string &name, KeptEntry &out,
                  std::string &error)
{
  if (info.flag & 0x01)
  {
    error = "encrypted entries are not supported (" + name + ")";
    return false;
  }
  if (info.compression_method != 0 && info.compression_method != 8)
  {
    error = "unsupported compression method in " + name;
    return false;
  }
  if (info.disk_num_start != 0)
  {
    error = "multi-disk zips are not supported (" + name + ")";
    return false;
  }
  int method = 0, level = 0;
  if (unzOpenCurrentFile2(zf, &method, &level, 1) != UNZ_OK)
  {
    error = "cannot open " + name;
    return false;
  }
  std::vector<uint8_t> data(info.compressed_size);
  size_t got = 0;
  while (got < data.size())
  {
    int n = unzReadCurrentFile(zf, data.data() + got, (unsigned)(data.size() - got));
    if (n <= 0)
    {
      unzCloseCurrentFile(zf);
      error = "cannot read " + name;
      return false;
    }
    got += (size_t)n;
  }
  unzCloseCurrentFile(zf);
  out.name = name;
  out.method = info.compression_method;
  out.flag = info.flag & ~0x08u & ~0x01u;  // we write sizes up front: no descriptor, no encryption
  out.dosDate = info.dosDate;
  out.crc = info.crc;
  out.compSize = info.compressed_size;
  out.uncompSize = info.uncompressed_size;
  out.internalFa = info.internal_fa;
  out.externalFa = info.external_fa;
  out.compData = std::move(data);
  return true;
}

bool ReadAllEntries(const std::string &zipPath, std::vector<KeptEntry> &entries, std::string &error)
{
  unzFile zf = unzOpen(zipPath.c_str());
  if (!zf)
  {
    error = "cannot open " + zipPath;
    return false;
  }
  bool ok = true;
  int rc = unzGoToFirstFile(zf);
  while (rc == UNZ_OK)
  {
    unz_file_info info{};
    char name[1024] = {};
    if (unzGetCurrentFileInfo(zf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      error = "cannot list " + zipPath;
      ok = false;
      break;
    }
    // Long names: re-query with a bigger buffer when truncated.
    std::string fname(name);
    if (info.size_filename >= sizeof(name) - 1)
    {
      std::vector<char> big(info.size_filename + 1, 0);
      if (unzGetCurrentFileInfo(zf, &info, big.data(), (uLong)big.size() - 1, nullptr, 0, nullptr, 0) !=
          UNZ_OK)
      {
        error = "cannot list " + zipPath;
        ok = false;
        break;
      }
      fname.assign(big.data(), info.size_filename);
    }
    KeptEntry e;
    if (!ReadEntryRaw(zf, info, fname, e, error))
    {
      ok = false;
      break;
    }
    entries.push_back(std::move(e));
    rc = unzGoToNextFile(zf);
  }
  unzClose(zf);
  if (!ok)
    return false;
  if (rc != UNZ_END_OF_LIST_OF_FILE)
  {
    error = "cannot read " + zipPath;
    return false;
  }
  return true;
}

}  // namespace

bool HasDriveboardRom(const std::string &ecaZipPath)
{
  unzFile zf = unzOpen(ecaZipPath.c_str());
  if (!zf)
    return false;
  bool found = false;
  for (int rc = unzGoToFirstFile(zf); rc == UNZ_OK; rc = unzGoToNextFile(zf))
  {
    unz_file_info info{};
    char name[512] = {};
    if (unzGetCurrentFileInfo(zf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
      break;
    if (info.crc == kFileCrc || Lower(name) == kFileName)
    {
      found = true;
      break;
    }
  }
  unzClose(zf);
  return found;
}

bool ExtractFromScud(const std::string &scudZipPath, std::vector<uint8_t> &outData, std::string &error)
{
  unzFile zf = unzOpen(scudZipPath.c_str());
  if (!zf)
  {
    error = "cannot open " + scudZipPath;
    return false;
  }
  // Prefer a CRC match, fall back to the file name (MAME names are stable).
  std::string candidate;
  bool byCrc = false;
  for (int rc = unzGoToFirstFile(zf); rc == UNZ_OK; rc = unzGoToNextFile(zf))
  {
    unz_file_info info{};
    char name[512] = {};
    if (unzGetCurrentFileInfo(zf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
      break;
    if (info.crc == kFileCrc)
    {
      candidate = name;
      byCrc = true;
      break;
    }
    if (candidate.empty() && Lower(name) == kFileName)
      candidate = name;
  }
  if (candidate.empty())
  {
    unzClose(zf);
    error = kFileName + std::string(" not found in ") + scudZipPath;
    return false;
  }
  if (unzLocateFile(zf, candidate.c_str(), 1) != UNZ_OK &&
      unzLocateFile(zf, candidate.c_str(), 2) != UNZ_OK)
  {
    unzClose(zf);
    error = "cannot locate " + candidate + " in " + scudZipPath;
    return false;
  }
  unz_file_info info{};
  if (unzGetCurrentFileInfo(zf, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
  {
    unzClose(zf);
    error = "cannot stat " + candidate;
    return false;
  }
  if (unzOpenCurrentFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    error = "cannot open " + candidate;
    return false;
  }
  std::vector<uint8_t> data(info.uncompressed_size);
  size_t got = 0;
  while (got < data.size())
  {
    int n = unzReadCurrentFile(zf, data.data() + got, (unsigned)(data.size() - got));
    if (n <= 0)
      break;
    got += (size_t)n;
  }
  int closeRc = unzCloseCurrentFile(zf);
  unzClose(zf);
  if (got != data.size() || closeRc == UNZ_CRCERROR)
  {
    error = "cannot read " + candidate + " (corrupt zip?)";
    return false;
  }
  if (data.size() != kFileSize)
  {
    error = std::string("unexpected size for ") + candidate + ": " + std::to_string(data.size()) +
            " bytes (expected " + std::to_string(kFileSize) + ")";
    return false;
  }
  if (crc32(0, data.data(), (uInt)data.size()) != kFileCrc && !byCrc)
  {
    // Named match but content differs: refuse rather than writing a bad ROM.
    error = std::string("CRC mismatch for ") + candidate + " (expected C9FAC464)";
    return false;
  }
  outData = std::move(data);
  return true;
}

bool ImportFromScud(const std::string &ecaZipPath, const std::string &scudZipPath, std::string &error)
{
  std::vector<uint8_t> drive;
  if (!ExtractFromScud(scudZipPath, drive, error))
    return false;

  std::vector<KeptEntry> entries;
  if (!ReadAllEntries(ecaZipPath, entries, error))
    return false;

  // Replace any existing drive-board entry, otherwise append.
  bool replaced = false;
  for (auto &e : entries)
  {
    if (e.crc == kFileCrc || Lower(e.name) == kFileName)
    {
      e.name = kFileName;
      e.method = 0;
      e.flag = 0;
      e.dosDate = 0;
      e.crc = kFileCrc;
      e.compSize = (uint32_t)drive.size();
      e.uncompSize = (uint32_t)drive.size();
      e.internalFa = 0;
      e.externalFa = 0;
      e.compData = drive;
      replaced = true;
    }
  }
  if (!replaced)
  {
    KeptEntry e;
    e.name = kFileName;
    e.method = 0;  // stored: 64 KB is tiny, keeps the writer dependency-free
    e.flag = 0;
    e.dosDate = 0;
    e.crc = (uint32_t)crc32(0, drive.data(), (uInt)drive.size());
    e.compSize = (uint32_t)drive.size();
    e.uncompSize = (uint32_t)drive.size();
    e.compData = drive;
    entries.push_back(std::move(e));
  }

  const std::string tmpPath = ecaZipPath + ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
      error = "cannot write " + tmpPath;
      return false;
    }
    std::vector<uint32_t> localOff;
    localOff.reserve(entries.size());
    for (const auto &e : entries)
    {
      if (e.name.size() > 0xFFFF || e.compData.size() != e.compSize)
      {
        error = "entry too large: " + e.name;
        return false;
      }
      localOff.push_back((uint32_t)out.tellp());
      std::vector<uint8_t> h;
      Put32(h, 0x04034B50u);  // local file header
      Put16(h, e.method == 8 ? 20 : 10);
      Put16(h, e.flag);
      Put16(h, e.method);
      Put16(h, e.dosDate & 0xFFFFu);          // time
      Put16(h, (e.dosDate >> 16) & 0xFFFFu);  // date
      Put32(h, e.crc);
      Put32(h, e.compSize);
      Put32(h, e.uncompSize);
      Put16(h, (uint32_t)e.name.size());
      Put16(h, 0);  // no extra field
      out.write((char *)h.data(), (std::streamsize)h.size());
      out.write(e.name.data(), (std::streamsize)e.name.size());
      if (!e.compData.empty())
        out.write((char *)e.compData.data(), (std::streamsize)e.compData.size());
      if (!out)
      {
        error = "cannot write " + tmpPath;
        return false;
      }
    }
    const uint32_t centralOff = (uint32_t)out.tellp();
    uint32_t centralSize = 0;
    for (size_t i = 0; i < entries.size(); i++)
    {
      const auto &e = entries[i];
      std::vector<uint8_t> h;
      Put32(h, 0x02014B50u);  // central directory header
      Put16(h, 20);           // version made by (2.0, FAT)
      Put16(h, e.method == 8 ? 20 : 10);
      Put16(h, e.flag);
      Put16(h, e.method);
      Put16(h, e.dosDate & 0xFFFFu);
      Put16(h, (e.dosDate >> 16) & 0xFFFFu);
      Put32(h, e.crc);
      Put32(h, e.compSize);
      Put32(h, e.uncompSize);
      Put16(h, (uint32_t)e.name.size());
      Put16(h, 0);  // extra
      Put16(h, 0);  // comment
      Put16(h, 0);  // disk number start
      Put16(h, e.internalFa);
      Put32(h, e.externalFa);
      Put32(h, localOff[i]);
      out.write((char *)h.data(), (std::streamsize)h.size());
      out.write(e.name.data(), (std::streamsize)e.name.size());
      centralSize += (uint32_t)h.size() + (uint32_t)e.name.size();
      if (!out)
      {
        error = "cannot write " + tmpPath;
        return false;
      }
    }
    Put32(out, 0x06054B50u);  // end of central directory
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, (uint32_t)entries.size());
    Put16(out, (uint32_t)entries.size());
    Put32(out, centralSize);
    Put32(out, centralOff);
    Put16(out, 0);  // comment length
    out.flush();
    if (!out)
    {
      error = "cannot write " + tmpPath;
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmpPath, ecaZipPath, ec);
  if (ec)
  {
    fs::remove(tmpPath, ec);
    error = "cannot replace " + ecaZipPath + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace EcaFfb
