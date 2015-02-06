#include <cassert>
#include "fs.h"
#include "logger.h"
#include "output_files.h"
#include "string_ex.h"

struct OutputFilesHdd::Internals
{
    std::string output_dir;

    std::string get_full_path(
        const std::string output_dir, const std::string file_name)
    {
        assert(file_name != "");
        if (output_dir == "")
            return file_name;
        return output_dir + "/" + file_name;
    }
};

OutputFilesHdd::OutputFilesHdd(std::string output_dir)
{
    internals = new Internals();
    internals->output_dir = output_dir;
}

OutputFilesHdd::~OutputFilesHdd()
{
    delete internals;
}

bool OutputFilesHdd::save(
    std::unique_ptr<VirtualFile>(*save_proc)(void *),
    void *context) const
{
    assert(save_proc != nullptr);

    log_info("Reading file...");

    std::unique_ptr<VirtualFile> file = save_proc(context);
    if (file == nullptr)
    {
        log_error("An error occured while reading file, saving skipped.");
        log_info("");
        return false;
    }

    std::string full_path = internals->get_full_path(
        internals->output_dir, file->name);
    assert(full_path != "");

    log_info("Saving to %s... ", full_path.c_str());

    if (!mkpath(dirname(full_path)))
        assert(0);

    IO *output_io = io_create_from_file(full_path.c_str(), "wb");
    if (!output_io)
    {
        log_warning("Failed to open file %s", full_path.c_str());
        log_info("");
        return false;
    }

    io_seek(&file->io, 0);
    io_write_string_from_io(output_io, &file->io, io_size(&file->io));
    io_destroy(output_io);
    log_info("Saved successfully");
    log_info("");
    return true;
}



struct OutputFilesMemory::Internals
{
    std::vector<std::unique_ptr<VirtualFile>> files;
};

OutputFilesMemory::OutputFilesMemory()
{
    internals = new Internals();
}

OutputFilesMemory::~OutputFilesMemory()
{
    delete internals;
}

const std::vector<VirtualFile*> OutputFilesMemory::get_saved() const
{
    std::vector<VirtualFile*> files;
    for (std::unique_ptr<VirtualFile>& f : internals->files)
        files.push_back(f.get());
    return files;
}

bool OutputFilesMemory::save(
    std::unique_ptr<VirtualFile>(*save_proc)(void *),
    void *context) const
{
    assert(save_proc != nullptr);
    std::unique_ptr<VirtualFile> file(save_proc(context));
    if (file == nullptr)
        return false;
    internals->files.push_back(std::move(file));
    return true;
}
