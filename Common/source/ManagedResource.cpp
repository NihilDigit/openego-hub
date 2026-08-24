#include "ManagedResource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace ManagedRes {
namespace {

// .resources 文件头的魔数，见 System.Resources.ResourceManager.MagicNumber。
constexpr uint32_t kResourcesMagic = 0xBEEFCACEu;

// System.Resources.ResourceTypeCode
constexpr uint32_t kTypeCodeByteArray = 0x20;
constexpr uint32_t kTypeCodeStream    = 0x21;

// 一个只前进、越界即失败的读取器。所有偏移都来自文件内容，必须假定它可能是坏的。
class Cursor {
public:
    Cursor(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    bool Seek(size_t pos) {
        if (pos > m_size) return false;
        m_pos = pos;
        return true;
    }
    [[nodiscard]] size_t Pos() const { return m_pos; }
    bool Skip(size_t n) { return Seek(m_pos + n); }

    bool U8(uint8_t& v) {
        if (m_pos + 1 > m_size) return false;
        v = m_data[m_pos++];
        return true;
    }
    bool U32(uint32_t& v) {
        if (m_pos + 4 > m_size) return false;
        std::memcpy(&v, m_data + m_pos, 4);
        m_pos += 4;
        return true;
    }
    bool I32(int32_t& v) {
        uint32_t u = 0;
        if (!U32(u)) return false;
        v = static_cast<int32_t>(u);
        return true;
    }
    bool Bytes(void* dst, size_t n) {
        if (m_pos + n > m_size) return false;
        std::memcpy(dst, m_data + m_pos, n);
        m_pos += n;
        return true;
    }

    // BinaryWriter 的变长整数：每字节 7 位有效，最高位表示还有后续。
    bool Packed7Bit(uint32_t& v) {
        uint32_t result = 0;
        int shift = 0;
        for (int i = 0; i < 5; ++i) {
            uint8_t b = 0;
            if (!U8(b)) return false;
            result |= static_cast<uint32_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                v = result;
                return true;
            }
            shift += 7;
        }
        return false;   // 超过 5 字节说明数据是坏的
    }

    // 7 位变长长度 + UTF-8，用于类型名。这里只需要跳过。
    bool SkipPackedString() {
        uint32_t len = 0;
        return Packed7Bit(len) && Skip(len);
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_pos = 0;
};

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != FALSE && size.QuadPart > 0 &&
              size.QuadPart < (64ll << 20);   // 插件 DLL 只有几百 KB，超过就不是我们要的东西
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != FALSE &&
             read == out.size();
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

// CLI 头（IMAGE_COR20_HEADER）里我们只关心资源区的位置。corhdr.h 不一定可用，按偏移取。
struct CliResources {
    uint32_t rva = 0;
    uint32_t size = 0;
};

// PE 的 RVA 要经节表换算成文件偏移，两者一般不相等。
bool RvaToOffset(const std::vector<uint8_t>& img, const IMAGE_SECTION_HEADER* sections,
                 unsigned count, uint32_t rva, size_t& out) {
    for (unsigned i = 0; i < count; ++i) {
        const auto& s = sections[i];
        const uint32_t va = s.VirtualAddress;
        const uint32_t vsize = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
        if (rva >= va && rva < va + vsize) {
            const uint64_t off = uint64_t(s.PointerToRawData) + (rva - va);
            if (off >= img.size()) return false;
            out = static_cast<size_t>(off);
            return true;
        }
    }
    return false;
}

bool FindCliResources(const std::vector<uint8_t>& img, CliResources& out) {
    if (img.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew < 0 || size_t(dos->e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) > img.size()) {
        return false;
    }

    const uint8_t* nt = img.data() + dos->e_lfanew;
    uint32_t sig = 0;
    std::memcpy(&sig, nt, 4);
    if (sig != IMAGE_NT_SIGNATURE) return false;

    const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(nt + 4);
    const uint8_t* opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    if (size_t(opt - img.data()) + 2 > img.size()) return false;

    uint16_t optMagic = 0;
    std::memcpy(&optMagic, opt, 2);

    // 数据目录第 15 项（索引 14）是 CLI 头。PE32 与 PE32+ 的可选头长度不同。
    const IMAGE_DATA_DIRECTORY* dirs = nullptr;
    uint32_t dirCount = 0;
    if (optMagic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const auto* o = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(opt);
        dirs = o->DataDirectory;
        dirCount = o->NumberOfRvaAndSizes;
    } else if (optMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const auto* o = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(opt);
        dirs = o->DataDirectory;
        dirCount = o->NumberOfRvaAndSizes;
    } else {
        return false;
    }
    if (dirCount <= IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR) return false;

    const auto& cliDir = dirs[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
    if (cliDir.VirtualAddress == 0 || cliDir.Size < 32) return false;   // 不是托管程序集

    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        opt + fileHeader->SizeOfOptionalHeader);
    if (size_t(reinterpret_cast<const uint8_t*>(sections) - img.data()) +
            sizeof(IMAGE_SECTION_HEADER) * fileHeader->NumberOfSections > img.size()) {
        return false;
    }

    size_t cliOff = 0;
    if (!RvaToOffset(img, sections, fileHeader->NumberOfSections, cliDir.VirtualAddress, cliOff)) {
        return false;
    }
    if (cliOff + 32 > img.size()) return false;

    uint32_t resRva = 0, resSize = 0;
    std::memcpy(&resRva, img.data() + cliOff + 24, 4);
    std::memcpy(&resSize, img.data() + cliOff + 28, 4);
    if (resRva == 0 || resSize == 0) return false;

    size_t resOff = 0;
    if (!RvaToOffset(img, sections, fileHeader->NumberOfSections, resRva, resOff)) return false;
    if (resOff + resSize > img.size()) return false;

    out.rva = static_cast<uint32_t>(resOff);   // 这里存的是文件偏移，调用方只用它定位
    out.size = resSize;
    return true;
}

// 在一段 .resources 内容里按名字找项。blob 指向 0xBEEFCACE 开头的那一块。
bool FindInResourceSet(const uint8_t* blob, size_t blobSize,
                       const std::wstring& name, std::vector<uint8_t>& out) {
    Cursor c(blob, blobSize);

    uint32_t magic = 0;
    if (!c.U32(magic) || magic != kResourcesMagic) return false;

    int32_t headerVersion = 0, bytesToSkip = 0;
    if (!c.I32(headerVersion) || !c.I32(bytesToSkip) || bytesToSkip < 0) return false;
    // 这段是 reader/set 的类型名。版本 1 的读取器会解析它，我们一律跳过——长度就是为此写的。
    if (!c.Skip(static_cast<size_t>(bytesToSkip))) return false;

    int32_t setVersion = 0, numResources = 0, numTypes = 0;
    if (!c.I32(setVersion) || !c.I32(numResources) || !c.I32(numTypes)) return false;
    if (setVersion < 1 || setVersion > 2) return false;
    if (numResources < 0 || numTypes < 0 || numResources > 100000) return false;

    for (int32_t i = 0; i < numTypes; ++i) {
        if (!c.SkipPackedString()) return false;
    }

    // 类型表之后按 8 字节对齐，对齐基准是流起点，也就是 blob 起点。
    if (const size_t misalign = c.Pos() & 7u; misalign != 0) {
        if (!c.Skip(8 - misalign)) return false;
    }

    // 名字哈希表，用于快速查找；我们线性扫，跳过。
    if (!c.Skip(size_t(numResources) * 4)) return false;

    std::vector<int32_t> namePositions(static_cast<size_t>(numResources));
    for (int32_t i = 0; i < numResources; ++i) {
        if (!c.I32(namePositions[static_cast<size_t>(i)])) return false;
    }

    int32_t dataSectionOffset = 0;
    if (!c.I32(dataSectionOffset) || dataSectionOffset < 0) return false;
    const size_t nameSectionStart = c.Pos();

    for (int32_t i = 0; i < numResources; ++i) {
        const int32_t np = namePositions[static_cast<size_t>(i)];
        if (np < 0) continue;
        if (!c.Seek(nameSectionStart + static_cast<size_t>(np))) continue;

        uint32_t nameBytes = 0;
        if (!c.Packed7Bit(nameBytes)) continue;
        if (nameBytes == 0 || (nameBytes & 1u) != 0 || nameBytes > 4096) continue;

        std::wstring candidate(nameBytes / 2, L'\0');
        if (!c.Bytes(candidate.data(), nameBytes)) continue;   // 名字是 UTF-16LE

        int32_t dataOffset = 0;
        if (!c.I32(dataOffset) || dataOffset < 0) continue;
        if (candidate != name) continue;

        if (!c.Seek(static_cast<size_t>(dataSectionOffset) + static_cast<size_t>(dataOffset))) {
            return false;
        }
        uint32_t typeCode = 0;
        if (!c.Packed7Bit(typeCode)) return false;
        if (typeCode != kTypeCodeByteArray && typeCode != kTypeCodeStream) return false;

        int32_t length = 0;
        if (!c.I32(length) || length <= 0) return false;
        out.resize(static_cast<size_t>(length));
        return c.Bytes(out.data(), out.size());
    }
    return false;
}

} // namespace

bool ReadEmbeddedResource(const std::wstring& assemblyPath,
                          const std::wstring& resourceName,
                          std::vector<uint8_t>& out) {
    out.clear();

    std::vector<uint8_t> img;
    if (!ReadWholeFile(assemblyPath, img)) return false;

    CliResources res{};
    if (!FindCliResources(img, res)) return false;

    // 资源区里每一项是「uint32 长度 + 内容」，各项之间还有对齐填充。与其去元数据表里查每项的
    // 偏移（那要按 schema 跳过四十来张表），不如直接扫魔数——每个 .resources 项都以它开头，
    // 长度就写在它前面 4 字节。
    const size_t begin = res.rva;
    const size_t end = begin + res.size;
    for (size_t p = begin + 4; p + 4 <= end; ++p) {
        uint32_t magic = 0;
        std::memcpy(&magic, img.data() + p, 4);
        if (magic != kResourcesMagic) continue;

        uint32_t declared = 0;
        std::memcpy(&declared, img.data() + p - 4, 4);
        if (declared < 8 || p + declared > end) continue;

        if (FindInResourceSet(img.data() + p, declared, resourceName, out)) {
            return true;
        }
    }
    return false;
}

} // namespace ManagedRes
