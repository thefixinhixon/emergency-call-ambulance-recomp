// Force feedback ROM helper: the optional drive-board program for ECA.
//
// ECA's Games.xml lists an optional region:
//
//   <region name="driveboard_program" ... required="false">
//     <file offset="0" name="epr-19338a.bin" crc32="0xC9FAC464" />
//
// The file is not part of the eca set; the same ROM ships with Scud Race
// (scud.zip). Without it Supermodel logs
// "'epr-19338a.bin' ... not found" / "Optional ROM region
// 'driveboard_program' ... could not be loaded" and runs without force
// feedback. These helpers let the launcher detect the missing file and copy
// it into the user's eca.zip from a scud.zip they provide.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace EcaFfb
{
inline constexpr char kFileName[] = "epr-19338a.bin";
inline constexpr uint32_t kFileCrc = 0xC9FAC464u;
inline constexpr size_t kFileSize = 64u * 1024u;

// True if the zip at ecaZipPath already contains the drive-board ROM
// (matched by CRC32, falling back to file name). Missing/unreadable zips
// return false.
bool HasDriveboardRom(const std::string &ecaZipPath);

// Read and validate the drive-board ROM out of a scud.zip.
// On success fills outData and returns true. On failure returns false and
// sets error.
bool ExtractFromScud(const std::string &scudZipPath, std::vector<uint8_t> &outData, std::string &error);

// Copy the drive-board ROM from scudZipPath into ecaZipPath (replacing any
// existing entry). Existing entries are copied with their compressed bytes
// preserved; the new entry is stored uncompressed. The destination is
// written to a .tmp file and renamed over the original. Returns true on
// success, false with error set on failure.
bool ImportFromScud(const std::string &ecaZipPath, const std::string &scudZipPath, std::string &error);

}  // namespace EcaFfb
