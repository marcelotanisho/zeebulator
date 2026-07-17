#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/brew/hle_runtime.h"
#include "core/brew/virtual_filesystem.h"
#include "core/memory/memory.h"

namespace zeebulator {

// IFile/IFileMgr HLE backed by a VirtualFilesystem (itself backed by a
// loaded GGZ archive's contents). Vtable slot orders verified directly
// against real Qualcomm AEEFile.h -- see TASKS.md Phase 4:
//   IFile inherits IAStream: AddRef, Release, Readable, Read, Cancel,
//   then Write, GetInfo, Seek, Truncate.
//   IFileMgr: AddRef, Release, OpenFile, GetInfo, Remove, MkDir, RmDir,
//   Test, GetFreeSpace, GetLastError, EnumInit, EnumNext, Rename.
//
// The GGZ-backed content is read-only (Remove/MkDir/RmDir/Rename all
// return an error rather than silently succeeding), but OpenFile
// supports real save-game style read/write/create against a separate,
// in-memory "user data" store (see writable_files_) -- confirmed real
// disassembly of Double Dragon's own save/load routine (`ddragonz.mod`
// offset `0x9f3c`, called from `0x237c4`, see PHASE8_LOG.md) does
// exactly `IFILEMGR_Test("./udata/ddz.sav")`, then on failure
// `IFILEMGR_GetFreeSpace()` (checked against a minimum), then
// `IFILEMGR_OpenFile(..., _OFM_CREATE)` -- a real "load save, or create
// a fresh one" pattern this class needs to actually satisfy, not just
// avoid crashing on. The filesystem is flat (GGZ has no directory
// structure), so EnumInit ignores its directory argument and always
// enumerates everything (only the read-only GGZ-backed files, not
// writable ones -- nothing exercised so far enumerates a save
// directory).
//
// IFile and IFileMgr are implemented together here (not one-file-per-
// interface like IShell/IDisplay) because they're a tightly coupled
// pair in practice: IFileMgr::OpenFile is what creates IFile instances,
// and both need access to the same open-file state.
class FileHle {
 public:
  // `file_object_region_start` is a bump-allocated address range this
  // class owns for newly-opened IFile object headers (4 bytes each) --
  // must not overlap any other memory region the caller is using.
  FileHle(Memory& memory, HleRuntime& hle, const VirtualFilesystem& vfs,
          uint32_t file_object_region_start);

  // Builds the shared IFile vtable (used by every opened file) and the
  // IFileMgr object. Returns the IFileMgr* value the app should receive.
  uint32_t Build(uint32_t file_mgr_vtable_address, uint32_t file_mgr_object_address,
                 uint32_t file_vtable_address);

 private:
  struct OpenFile {
    std::string name;
    const std::vector<uint8_t>* data;  // current bytes, for Read/Seek/GetInfo/size
    std::vector<uint8_t>* mutable_data = nullptr;  // non-null only for writable_files_ entries
    uint32_t position = 0;
  };

  // IFileMgr methods.
  void OpenFileImpl(IArmCore& core);
  void FileMgrGetInfoImpl(IArmCore& core);
  void TestImpl(IArmCore& core);
  void GetFreeSpaceImpl(IArmCore& core);
  void EnumInitImpl(IArmCore& core);
  void EnumNextImpl(IArmCore& core);

  // IFile methods (shared vtable; instance looked up by "po", i.e. R0).
  void ReadImpl(IArmCore& core);
  void WriteImpl(IArmCore& core);
  void FileGetInfoImpl(IArmCore& core);
  void SeekImpl(IArmCore& core);

  void WriteFileInfo(uint32_t dest_addr, const std::string& name, uint32_t size);
  uint32_t AllocateFileObject(const std::string& name, const std::vector<uint8_t>* data,
                               std::vector<uint8_t>* mutable_data = nullptr);

  Memory& memory_;
  HleRuntime& hle_;
  const VirtualFilesystem& vfs_;
  uint32_t file_vtable_address_ = 0;
  uint32_t next_object_address_;
  size_t enum_cursor_ = 0;
  std::unordered_map<uint32_t, OpenFile> open_files_;
  // Runtime-created/written files (e.g. save games) -- separate from
  // the read-only GGZ-backed vfs_, never persisted past this process.
  std::unordered_map<std::string, std::vector<uint8_t>> writable_files_;
};

}  // namespace zeebulator
