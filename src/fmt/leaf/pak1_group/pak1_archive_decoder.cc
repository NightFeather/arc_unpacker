#include "fmt/leaf/pak1_group/pak1_archive_decoder.h"
#include <boost/optional.hpp>
#include <map>
#include "algo/locale.h"
#include "algo/range.h"
#include "algo/str.h"
#include "err.h"
#include "fmt/leaf/pak1_group/grp_image_decoder.h"
#include "util/file_from_image.h"

using namespace au;
using namespace au::fmt::leaf;

namespace
{
    struct ArchiveEntryImpl final : fmt::ArchiveEntry
    {
        size_t offset;
        size_t size;
        bool compressed;
        bool already_unpacked;
    };
}

// Modified LZSS routine
// - starting position at 0 rather than 0xFEE
// - optionally, additional byte for repetition count
// - dictionary writing in two passes
static bstr custom_lzss_decompress(
    const bstr &input, size_t output_size, const size_t dict_capacity)
{
    std::vector<u8> dict(dict_capacity);
    size_t dict_size = 0;
    size_t dict_pos = 0;

    bstr output(output_size);
    auto output_ptr = output.get<u8>();
    auto output_end = output.end<const u8>();
    auto input_ptr = input.get<const u8>();
    auto input_end = input.end<const u8>();

    u16 control = 0;
    while (output_ptr < output_end)
    {
        control >>= 1;
        if (!(control & 0x100))
            control = *input_ptr++ | 0xFF00;

        if (control & 1)
        {
            dict[dict_pos++] = *output_ptr++ = *input_ptr++;
            dict_pos %= dict_capacity;
            if (dict_size < dict_capacity)
                dict_size++;
        }
        else
        {
            auto tmp = *reinterpret_cast<const u16*>(input_ptr);
            input_ptr += 2;

            auto look_behind_pos = tmp >> 4;
            auto repetitions = tmp & 0xF;
            if (repetitions == 0xF)
                repetitions += *input_ptr++;
            repetitions += 3;

            auto i = repetitions;
            while (i-- && output_ptr < output_end)
            {
                *output_ptr++ = dict[look_behind_pos++];
                look_behind_pos %= dict_size;
            }

            auto source = &output_ptr[-repetitions];
            while (source < output_ptr)
            {
                dict[dict_pos++] = *source++;
                dict_pos %= dict_capacity;
                if (dict_size < dict_capacity)
                    dict_size++;
            }
        }
    }

    return output;
}

struct Pak1ArchiveDecoder::Priv final
{
    boost::optional<int> version;
};

Pak1ArchiveDecoder::Pak1ArchiveDecoder() : p(new Priv())
{
}

Pak1ArchiveDecoder::~Pak1ArchiveDecoder()
{
}

void Pak1ArchiveDecoder::register_cli_options(ArgParser &arg_parser) const
{
    arg_parser.register_switch({"--pak-version"})
        ->set_value_name("NUMBER")
        ->set_description("File version (1 or 2)");
}

void Pak1ArchiveDecoder::parse_cli_options(const ArgParser &arg_parser)
{
    if (arg_parser.has_switch("pak-version"))
    {
        set_version(algo::from_string<int>(
            arg_parser.get_switch("pak-version")));
    }
}

void Pak1ArchiveDecoder::set_version(const int version)
{
    if (version != 1 && version != 2)
        throw err::UsageError("PAK version can be either '1' or '2'");
    p->version = version;
}

bool Pak1ArchiveDecoder::is_recognized_impl(io::File &input_file) const
{
    auto meta = read_meta(input_file);
    if (!meta->entries.size())
        return false;
    auto last_entry = static_cast<const ArchiveEntryImpl*>(
        meta->entries[meta->entries.size() - 1].get());
    return last_entry->offset + last_entry->size == input_file.stream.size();
}

std::unique_ptr<fmt::ArchiveMeta>
    Pak1ArchiveDecoder::read_meta_impl(io::File &input_file) const
{
    auto file_count = input_file.stream.read_u32_le();
    auto meta = std::make_unique<ArchiveMeta>();
    for (auto i : algo::range(file_count))
    {
        auto entry = std::make_unique<ArchiveEntryImpl>();
        entry->already_unpacked = false;
        entry->path = algo::sjis_to_utf8(
            input_file.stream.read_to_zero(16)).str();
        entry->size = input_file.stream.read_u32_le();
        entry->compressed = input_file.stream.read_u32_le() > 0;
        entry->offset = input_file.stream.read_u32_le();
        if (entry->size)
            meta->entries.push_back(std::move(entry));
    }
    return meta;
}

std::unique_ptr<io::File> Pak1ArchiveDecoder::read_file_impl(
    io::File &input_file, const ArchiveMeta &m, const ArchiveEntry &e) const
{
    if (!p->version)
    {
        throw err::UsageError(
            "Please choose PAK version with --pak-version switch.");
    }

    auto entry = static_cast<const ArchiveEntryImpl*>(&e);
    if (entry->already_unpacked)
        return nullptr;

    input_file.stream.seek(entry->offset);
    bstr data;
    if (entry->compressed)
    {
        auto size_comp = input_file.stream.read_u32_le();
        auto size_orig = input_file.stream.read_u32_le();
        data = input_file.stream.read(size_comp - 8);
        data = custom_lzss_decompress(
            data, size_orig, p->version == 1 ? 0x1000 : 0x800);
    }
    else
    {
        data = input_file.stream.read(entry->size);
    }

    return std::make_unique<io::File>(entry->path, data);
}

void Pak1ArchiveDecoder::preprocess(
    io::File &input_file, fmt::ArchiveMeta &meta, const FileSaver &saver) const
{
    std::map<std::string, ArchiveEntryImpl*>
        palette_entries, sprite_entries, mask_entries;
    for (auto &entry : meta.entries)
    {
        const auto fn = entry->path.stem();
        if (entry->path.has_extension("c16"))
            palette_entries[fn] = static_cast<ArchiveEntryImpl*>(entry.get());
        else if (entry->path.has_extension("grp"))
            sprite_entries[fn] = static_cast<ArchiveEntryImpl*>(entry.get());
        else if (entry->path.has_extension("msk"))
            mask_entries[fn] = static_cast<ArchiveEntryImpl*>(entry.get());
    }

    GrpImageDecoder grp_image_decoder;
    for (auto it : sprite_entries)
    {
        try
        {
            ArchiveEntryImpl *sprite_entry = it.second;
            ArchiveEntryImpl *palette_entry = nullptr;
            ArchiveEntryImpl *mask_entry = nullptr;
            std::shared_ptr<io::File> palette_file;
            std::shared_ptr<io::File> mask_file;

            auto sprite_file = read_file(input_file, meta, *sprite_entry);
            if (palette_entries.find(it.first) != palette_entries.end())
            {
                palette_entry = palette_entries[it.first];
                palette_file = read_file(input_file, meta, *palette_entry);
            }
            if (mask_entries.find(it.first) != mask_entries.end())
            {
                mask_entry = mask_entries[it.first];
                mask_file = read_file(input_file, meta, *mask_entry);
            }

            auto sprite = grp_image_decoder.decode(
                *sprite_file, palette_file, mask_file);
            saver.save(util::file_from_image(sprite, sprite_entry->path));

            sprite_entry->already_unpacked = true;
            if (mask_entry)
                mask_entry->already_unpacked = true;
            if (palette_entry)
                palette_entry->already_unpacked = true;
        }
        catch (...)
        {
            continue;
        }
    }
}

std::vector<std::string> Pak1ArchiveDecoder::get_linked_formats() const
{
    return {"leaf/grp"};
}

static auto dummy = fmt::register_fmt<Pak1ArchiveDecoder>("leaf/pak1");
