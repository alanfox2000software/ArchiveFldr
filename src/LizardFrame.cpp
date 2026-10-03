// LizardFrame.cpp — see LizardFrame.h
#include "stdafx.h"
#include "LizardFrame.h"
#include "ThirdParty.h"

namespace {

constexpr uint32_t kMagic = 0x184D2206u;
constexpr size_t   kDictionaryBytes = 1u << 24; // LIZARD_DICT_SIZE
constexpr uint32_t kPrime1 = 2654435761u;
constexpr uint32_t kPrime2 = 2246822519u;
constexpr uint32_t kPrime3 = 3266489917u;
constexpr uint32_t kPrime4 =  668265263u;
constexpr uint32_t kPrime5 =  374761393u;

uint32_t Rotl(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

uint32_t Read32(const void* p)
{
    const auto* b = static_cast<const uint8_t*>(p);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint64_t Read64(const void* p)
{
    const auto* b = static_cast<const uint8_t*>(p);
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)b[i] << (i * 8);
    return v;
}

void Append32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 24));
}

void Append64(std::vector<uint8_t>& out, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) out.push_back((uint8_t)(v >> (i * 8)));
}

uint32_t XxhRound(uint32_t acc, uint32_t input)
{
    acc += input * kPrime2;
    acc = Rotl(acc, 13);
    return acc * kPrime1;
}

class Xxh32
{
public:
    Xxh32()
        : m_v1(kPrime1 + kPrime2), m_v2(kPrime2),
          m_v3(0), m_v4(0u - kPrime1) {}

    void Update(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        m_total += size;

        if (m_memSize + size < sizeof(m_mem))
        {
            if (size) memcpy(m_mem + m_memSize, p, size);
            m_memSize += size;
            return;
        }

        if (m_memSize)
        {
            const size_t fill = sizeof(m_mem) - m_memSize;
            memcpy(m_mem + m_memSize, p, fill);
            Process(m_mem);
            p += fill;
            size -= fill;
            m_memSize = 0;
        }

        while (size >= 16)
        {
            Process(p);
            p += 16;
            size -= 16;
        }
        if (size)
        {
            memcpy(m_mem, p, size);
            m_memSize = size;
        }
    }

    uint32_t Digest() const
    {
        uint32_t h = m_total >= 16
            ? Rotl(m_v1, 1) + Rotl(m_v2, 7) +
              Rotl(m_v3, 12) + Rotl(m_v4, 18)
            : kPrime5;
        h += (uint32_t)m_total;

        const uint8_t* p = m_mem;
        size_t left = m_memSize;
        while (left >= 4)
        {
            h += Read32(p) * kPrime3;
            h = Rotl(h, 17) * kPrime4;
            p += 4;
            left -= 4;
        }
        while (left--)
        {
            h += *p++ * kPrime5;
            h = Rotl(h, 11) * kPrime1;
        }
        h ^= h >> 15;
        h *= kPrime2;
        h ^= h >> 13;
        h *= kPrime3;
        h ^= h >> 16;
        return h;
    }

private:
    void Process(const uint8_t* p)
    {
        m_v1 = XxhRound(m_v1, Read32(p));
        m_v2 = XxhRound(m_v2, Read32(p + 4));
        m_v3 = XxhRound(m_v3, Read32(p + 8));
        m_v4 = XxhRound(m_v4, Read32(p + 12));
    }

    uint64_t m_total = 0;
    uint32_t m_v1, m_v2, m_v3, m_v4;
    uint8_t  m_mem[16] = {};
    size_t   m_memSize = 0;
};

uint32_t Xxh32OneShot(const void* data, size_t size)
{
    Xxh32 hash;
    hash.Update(data, size);
    return hash.Digest();
}

struct RawApi
{
    HMODULE module = nullptr;
    std::wstring path;
    int (__cdecl* compress)(const char*, char*, int, int, int) = nullptr;
    int (__cdecl* compressBound)(int) = nullptr;
    int (__cdecl* decompressSafe)(const char*, char*, int, int) = nullptr;
    int (__cdecl* decompressUsingDict)(const char*, char*, int, int,
                                       const char*, int) = nullptr;
};

RawApi g_api;
std::once_flag g_once;

void InitApi()
{
    g_api.module = ThirdParty::LoadComponent(L"lizard", &g_api.path);
    if (!g_api.module) return;
#define BIND_RAW(member, name) \
    g_api.member = reinterpret_cast<decltype(g_api.member)>( \
        GetProcAddress(g_api.module, name))
    BIND_RAW(compress, "Lizard_compress");
    BIND_RAW(compressBound, "Lizard_compressBound");
    BIND_RAW(decompressSafe, "Lizard_decompress_safe");
    BIND_RAW(decompressUsingDict, "Lizard_decompress_safe_usingDict");
#undef BIND_RAW
}

RawApi& Api()
{
    std::call_once(g_once, InitApi);
    return g_api;
}

LizardFrame::Result ReadExact(const LizardFrame::ReadFn& read,
                              void* buffer, size_t size, uint64_t& total)
{
    auto* dst = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < size)
    {
        size_t got = 0;
        if (!read(dst + done, size - done, &got))
            return LizardFrame::Result::ReadError;
        if (!got || got > size - done)
            return LizardFrame::Result::InvalidData;
        done += got;
        total += got;
    }
    return LizardFrame::Result::Ok;
}

LizardFrame::Result WriteAll(const LizardFrame::WriteFn& write,
                             const void* buffer, size_t size, uint64_t& total)
{
    if (size && !write(buffer, size)) return LizardFrame::Result::WriteError;
    total += size;
    return LizardFrame::Result::Ok;
}

struct BlockChoice { uint8_t id; size_t bytes; };

BlockChoice ChooseBlock(uint64_t requested)
{
    if (!requested || requested <= (128ull << 10)) return { 1, 128u << 10 };
    if (requested <= (256ull << 10)) return { 2, 256u << 10 };
    if (requested <= (1ull << 20))   return { 3, 1u << 20 };
    if (requested <= (4ull << 20))   return { 4, 4u << 20 };
    if (requested <= (16ull << 20))  return { 5, 16u << 20 };
    if (requested <= (64ull << 20))  return { 6, 64u << 20 };
    return { 7, 256u << 20 };
}

size_t BlockBytes(uint8_t id)
{
    switch (id)
    {
    case 1: return 128u << 10;
    case 2: return 256u << 10;
    case 3: return 1u << 20;
    case 4: return 4u << 20;
    case 5: return 16u << 20;
    case 6: return 64u << 20;
    case 7: return 256u << 20;
    default: return 0;
    }
}

void UpdateDictionary(std::vector<uint8_t>& dictionary,
                      const uint8_t* data, size_t size)
{
    if (size >= kDictionaryBytes)
    {
        dictionary.assign(data + size - kDictionaryBytes, data + size);
        return;
    }
    if (dictionary.size() + size > kDictionaryBytes)
    {
        const size_t remove = dictionary.size() + size - kDictionaryBytes;
        dictionary.erase(dictionary.begin(), dictionary.begin() + remove);
    }
    dictionary.insert(dictionary.end(), data, data + size);
}

} // anonymous namespace

namespace LizardFrame {

bool EncoderAvailable()
{
    RawApi& api = Api();
    return api.compress && api.compressBound;
}

bool DecoderAvailable()
{
    RawApi& api = Api();
    return api.decompressSafe && api.decompressUsingDict;
}

std::wstring LibraryPath()
{
    return Api().path;
}

Result Encode(const ReadFn& read, const WriteFn& write,
              uint64_t contentSize, int compressionLevel,
              uint64_t blockBytes, const ProgressFn& progress)
{
    RawApi& api = Api();
    if (!api.compress || !api.compressBound) return Result::Unavailable;
    if (!read || !write) return Result::ReadError;
    if (compressionLevel < 10) compressionLevel = 10;
    if (compressionLevel > 49) compressionLevel = 49;

    try
    {
        const BlockChoice block = ChooseBlock(blockBytes);
        std::vector<uint8_t> header;
        Append32(header, kMagic);
        const bool hasContentSize = contentSize != 0;
        header.push_back((uint8_t)(0x40 | 0x20 | (hasContentSize ? 0x08 : 0)));
        header.push_back((uint8_t)(block.id << 4));
        if (hasContentSize) Append64(header, contentSize);
        header.push_back((uint8_t)(Xxh32OneShot(
            header.data() + 4, header.size() - 4) >> 8));

        uint64_t totalIn = 0, totalOut = 0;
        Result result = WriteAll(write, header.data(), header.size(), totalOut);
        if (result != Result::Ok) return result;

        std::vector<uint8_t> input(block.bytes);
        const int bound = api.compressBound((int)block.bytes);
        if (bound <= 0) return Result::InvalidData;
        std::vector<uint8_t> compressed((size_t)bound);

        bool eof = false;
        while (!eof)
        {
            size_t filled = 0;
            while (filled < input.size())
            {
                size_t got = 0;
                if (!read(input.data() + filled, input.size() - filled, &got))
                    return Result::ReadError;
                if (got > input.size() - filled) return Result::ReadError;
                if (!got) { eof = true; break; }
                filled += got;
                totalIn += got;
            }
            if (!filled) break;

            const int made = api.compress(
                reinterpret_cast<const char*>(input.data()),
                reinterpret_cast<char*>(compressed.data()),
                (int)filled, (int)compressed.size(), compressionLevel);

            std::vector<uint8_t> blockHeader;
            const bool store = made <= 0 || (size_t)made >= filled;
            Append32(blockHeader, store
                ? ((uint32_t)filled | 0x80000000u) : (uint32_t)made);
            result = WriteAll(write, blockHeader.data(), blockHeader.size(), totalOut);
            if (result != Result::Ok) return result;
            result = WriteAll(write, store ? input.data() : compressed.data(),
                              store ? filled : (size_t)made, totalOut);
            if (result != Result::Ok) return result;
            if (progress && !progress(totalIn, totalOut)) return Result::Cancelled;
        }

        if (contentSize && totalIn != contentSize) return Result::ReadError;
        const uint8_t endMark[4] = {};
        result = WriteAll(write, endMark, sizeof(endMark), totalOut);
        if (result != Result::Ok) return result;
        if (progress && !progress(totalIn, totalOut)) return Result::Cancelled;
        return Result::Ok;
    }
    catch (const std::bad_alloc&) { return Result::OutOfMemory; }
    catch (...) { return Result::InvalidData; }
}

Result Decode(const ReadFn& read, const WriteFn& write,
              uint64_t outputLimit, const ProgressFn& progress)
{
    RawApi& api = Api();
    if (!api.decompressSafe || !api.decompressUsingDict)
        return Result::Unavailable;
    if (!read || !write) return Result::ReadError;

    try
    {
        uint64_t totalIn = 0, totalOut = 0;
        uint8_t fixed[6] = {};
        Result result = ReadExact(read, fixed, sizeof(fixed), totalIn);
        if (result != Result::Ok) return result;
        if (Read32(fixed) != kMagic) return Result::InvalidData;

        const uint8_t flg = fixed[4], bd = fixed[5];
        const uint8_t version = (flg >> 6) & 3;
        const bool independent = ((flg >> 5) & 1) != 0;
        const bool blockChecksum = ((flg >> 4) & 1) != 0;
        const bool hasContentSize = ((flg >> 3) & 1) != 0;
        const bool contentChecksum = ((flg >> 2) & 1) != 0;
        const uint8_t blockId = (bd >> 4) & 7;
        const size_t maxBlock = BlockBytes(blockId);
        if (version != 1 || blockChecksum || (flg & 3) ||
            (bd & 0x8F) || !maxBlock)
            return Result::InvalidData;

        std::vector<uint8_t> descriptor{ flg, bd };
        uint64_t expectedSize = 0;
        if (hasContentSize)
        {
            uint8_t sizeBytes[8] = {};
            result = ReadExact(read, sizeBytes, sizeof(sizeBytes), totalIn);
            if (result != Result::Ok) return result;
            descriptor.insert(descriptor.end(), sizeBytes, sizeBytes + 8);
            expectedSize = Read64(sizeBytes);
        }
        uint8_t headerChecksum = 0;
        result = ReadExact(read, &headerChecksum, 1, totalIn);
        if (result != Result::Ok) return result;
        if (headerChecksum != (uint8_t)(Xxh32OneShot(
                descriptor.data(), descriptor.size()) >> 8))
            return Result::InvalidData;

        std::vector<uint8_t> compressed(maxBlock), output(maxBlock), dictionary;
        if (!independent) dictionary.reserve(kDictionaryBytes);
        Xxh32 contentHash;

        for (;;)
        {
            uint8_t sizeBytes[4] = {};
            result = ReadExact(read, sizeBytes, sizeof(sizeBytes), totalIn);
            if (result != Result::Ok) return result;
            const uint32_t field = Read32(sizeBytes);
            if (field == 0) break;

            const bool uncompressed = (field & 0x80000000u) != 0;
            const size_t storedSize = field & 0x7FFFFFFFu;
            if (!storedSize || storedSize > maxBlock) return Result::InvalidData;
            result = ReadExact(read, compressed.data(), storedSize, totalIn);
            if (result != Result::Ok) return result;

            size_t made = storedSize;
            if (uncompressed)
            {
                memcpy(output.data(), compressed.data(), storedSize);
            }
            else
            {
                const int decoded = independent
                    ? api.decompressSafe(
                        reinterpret_cast<const char*>(compressed.data()),
                        reinterpret_cast<char*>(output.data()),
                        (int)storedSize, (int)maxBlock)
                    : api.decompressUsingDict(
                        reinterpret_cast<const char*>(compressed.data()),
                        reinterpret_cast<char*>(output.data()),
                        (int)storedSize, (int)maxBlock,
                        dictionary.empty()
                            ? reinterpret_cast<const char*>(output.data())
                            : reinterpret_cast<const char*>(dictionary.data()),
                        (int)dictionary.size());
                if (decoded <= 0 || (size_t)decoded > maxBlock)
                    return Result::InvalidData;
                made = (size_t)decoded;
            }

            if (outputLimit && totalOut + made > outputLimit)
                return Result::WriteError;
            if (hasContentSize && totalOut + made > expectedSize)
                return Result::InvalidData;
            if (contentChecksum) contentHash.Update(output.data(), made);
            result = WriteAll(write, output.data(), made, totalOut);
            if (result != Result::Ok) return result;
            if (!independent) UpdateDictionary(dictionary, output.data(), made);
            if (progress && !progress(totalIn, totalOut)) return Result::Cancelled;
        }

        if (contentChecksum)
        {
            uint8_t checksum[4] = {};
            result = ReadExact(read, checksum, sizeof(checksum), totalIn);
            if (result != Result::Ok) return result;
            if (Read32(checksum) != contentHash.Digest())
                return Result::InvalidData;
        }
        if (hasContentSize && totalOut != expectedSize)
            return Result::InvalidData;
        if (progress && !progress(totalIn, totalOut)) return Result::Cancelled;
        return Result::Ok;
    }
    catch (const std::bad_alloc&) { return Result::OutOfMemory; }
    catch (...) { return Result::InvalidData; }
}

const wchar_t* ResultMessage(Result result)
{
    switch (result)
    {
    case Result::Ok:          return L"";
    case Result::Unavailable: return L"The Lizard DLL does not export a usable frame or raw-block API.";
    case Result::ReadError:   return L"The Lizard input stream could not be read completely.";
    case Result::WriteError:  return L"The Lizard output stream could not be written or exceeded its size limit.";
    case Result::OutOfMemory: return L"There was not enough memory for the selected Lizard block size.";
    case Result::Cancelled:   return L"The Lizard operation was cancelled.";
    default:                  return L"The stream is damaged or is not a valid Lizard frame.";
    }
}

} // namespace LizardFrame
