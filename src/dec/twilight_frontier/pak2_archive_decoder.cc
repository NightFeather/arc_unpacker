#include "dec/twilight_frontier/pak2_archive_decoder.h"
#include "algo/binary.h"
#include "algo/crypt/mt.h"
#include "algo/locale.h"
#include "algo/range.h"
#include "dec/twilight_frontier/pak2_image_decoder.h"
#include "err.h"
#include "io/file_system.h"
#include "io/memory_stream.h"
#include "util/file_from_image.h"

using namespace au;
using namespace au::dec::twilight_frontier;

namespace
{
    struct ArchiveEntryImpl final : dec::ArchiveEntry
    {
        size_t offset;
        size_t size;
        bool already_unpacked;
    };
}

static void decrypt(bstr &buffer, u32 mt_seed, u8 a, u8 b, u8 delta)
{
    auto mt = algo::crypt::MersenneTwister::Improved(mt_seed);
    for (const auto i : algo::range(buffer.size()))
    {
        buffer[i] ^= mt->next_u32();
        buffer[i] ^= a;
        a += b;
        b += delta;
    }
}

bool Pak2ArchiveDecoder::is_recognized_impl(io::File &input_file) const
{
    Logger dummy_logger;
    dummy_logger.mute();
    try
    {
        read_meta(dummy_logger, input_file);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::unique_ptr<dec::ArchiveMeta> Pak2ArchiveDecoder::read_meta_impl(
    const Logger &logger, io::File &input_file) const
{
    input_file.stream.seek(0);
    const auto file_count = input_file.stream.read_u16_le();
    if (!file_count && input_file.stream.size() != 6)
        throw err::RecognitionError();

    const auto table_size = input_file.stream.read_u32_le();
    if (table_size > input_file.stream.size() - input_file.stream.tell())
        throw err::RecognitionError();
    if (table_size > file_count * (4 + 4 + 256 + 1))
        throw err::RecognitionError();
    auto table_data = input_file.stream.read(table_size);
    decrypt(table_data, table_size + 6, 0xC5, 0x83, 0x53);
    io::MemoryStream table_stream(table_data);

    auto meta = std::make_unique<ArchiveMeta>();
    for (const auto i : algo::range(file_count))
    {
        auto entry = std::make_unique<ArchiveEntryImpl>();
        entry->already_unpacked = false;
        entry->offset = table_stream.read_u32_le();
        entry->size = table_stream.read_u32_le();
        const auto name_size = table_stream.read_u8();
        entry->path = algo::sjis_to_utf8(table_stream.read(name_size)).str();
        if (entry->offset + entry->size > input_file.stream.size())
            throw err::BadDataOffsetError();
        meta->entries.push_back(std::move(entry));
    }
    return meta;
}

std::unique_ptr<io::File> Pak2ArchiveDecoder::read_file_impl(
    const Logger &logger,
    io::File &input_file,
    const dec::ArchiveMeta &m,
    const dec::ArchiveEntry &e) const
{
    const auto entry = static_cast<const ArchiveEntryImpl*>(&e);
    if (entry->already_unpacked)
        return nullptr;
    const auto data = algo::unxor(
        input_file.stream.seek(entry->offset).read(entry->size),
        (entry->offset >> 1) | 0x23);
    return std::make_unique<io::File>(entry->path, data);
}

std::vector<std::string> Pak2ArchiveDecoder::get_linked_formats() const
{
    return {"twilight-frontier/pak2-sfx", "twilight-frontier/pak2-gfx"};
}

static auto _ = dec::register_decoder<Pak2ArchiveDecoder>(
    "twilight-frontier/pak2");