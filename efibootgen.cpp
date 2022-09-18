
#include "platform.h"
#include "disktools.h"
#include "status.h"
#include "jopts.h"

#define CHECK_REPORT_ABORT_ERROR(result)\
if (!result)\
{\
    std::cerr << "*** error: \"" << result.error_code() << "\"" << std::endl;\
    return -1;\
}


int main(int argc, char** argv)
{
    // use 7-zip manager to open the image file and examine the contents.
    //
    // On Linux: for raw FAT partitions you can also use
    //
    //  dosfsck -l -v -V boot.dd
    //
    // or
    //
    //  mdir -/ -i boot.dd
    //
    // to inspect and validate the partition
    //
    //
    //

    std::cout << "------------------------------------\n";
    std::cout << "efibootgen EFI boot disk creator\n";
    std::cout << "by jarl.ostensen\n\n";

    using namespace jopts;
    option_parser_t opts;
    const auto bootimage_option = opts.add(option_constraint_t::kOptional, option_type_t::kText, "b,bootimage", "source kernel binary, must be BOOTX64.EFI. This creates a standard EFI/BOOOT/BOOTX64.EFI layout.", option_default_t::kNotPresent);
    const auto verbose_option = opts.add(option_constraint_t::kOptional, option_type_t::kFlag, "v,verbose", "output more information about the build process", option_default_t::kNotPresent);
    const auto case_option = opts.add(option_constraint_t::kOptional, option_type_t::kFlag, "c,case", "preserve case of filenames. Default converts to UPPER", option_default_t::kNotPresent);
    const auto directory_option = opts.add(option_constraint_t::kOptional, option_type_t::kText, "d,directory", "source directory to copy to disk image", option_default_t::kNotPresent);
    const auto output_option = opts.add(option_constraint_t::kRequired, option_type_t::kText, "o,output", "output path name of created disk image", option_default_t::kNotPresent);
    const auto label_option = opts.add(option_constraint_t::kOptional, option_type_t::kText, "l,label", "volume label of image", option_default_t::kPresent, "NOLABEL");
    const auto reformat_disk_option = opts.add(option_constraint_t::kOptional, option_type_t::kFlag, "f,format", "reformat existing boot image (if exists)", option_default_t::kNotPresent);
    //NOTE: help is *always* available as -h or --help

    const auto parse_result = opts.parse(argc, argv);
    if (!parse_result || parse_result.value() == 0)
    {
        std::cerr << "Invalid or missing arguments. Options are:\n";
        opts.print_about(std::cerr) << std::endl;
        return -1;
    }

    if (opts.help_needed())
    {
        opts.print_about(std::cout) << std::endl;
    }

    disktools::_verbose = verbose_option.as<bool>();
    disktools::_preserve_case = case_option.as<bool>();
    disktools::_reformat = reformat_disk_option.as<bool>();

    char* buffer = nullptr;
    disktools::fs_t fs;

    // load a bootimage from disk and create standard EFI\BOOT structure
    if (bootimage_option)
    {
        auto dir_result = fs.create_directory("EFI");
        CHECK_REPORT_ABORT_ERROR(dir_result);
        dir_result = fs.create_directory(dir_result.value(), "BOOT");
        CHECK_REPORT_ABORT_ERROR(dir_result);

        const auto fpath = fs::path{ bootimage_option.as<const std::string&>() };
        const auto dir_fname = fpath.stem().string() + " " + fpath.filename().extension().string().substr(1);        
        // because a case insensitive comparison of std::string either requires a completely new type (traits) or a different algorithm...
        if (xstricmp(dir_fname.c_str(), "BOOTX64 EFI") != 0)
        {
            std::cerr << "*error: bootimage must be called BOOTX64.EFI\n";
            return -1;
        }
        std::ifstream ifs{ fpath.string(), std::ios::binary };
        if (ifs.is_open())
        {
            ifs.seekg(0, std::ios::end);
            const auto endpos = ifs.tellg();
            ifs.seekg(0, std::ios::beg);

            const auto size = size_t(endpos);
            buffer = new char[size];
            ifs.read(buffer, size);
            ifs.close();
            auto file_result = fs.create_file(dir_result.value(), "BOOTX64 EFI", buffer, size);
            CHECK_REPORT_ABORT_ERROR(file_result);
        }
        else
        {
            std::cerr << "*error: couldn't open " << fpath.string() << "\n";
            return -1;
        }
    }

    // copy whatever is in a given directory into the disk image
    if (directory_option)
    {
        if (!fs.empty())
        {
            std::cerr << "*error: you can't have both bootimage and directory options specified\n";
            return -1;
        }

        auto create_result = fs.create_from_source(directory_option.as<std::string_view>());

        if (disktools::_verbose)
        {
            std::cout << "\tloaded content from " << directory_option.as<std::string_view>() << "...\n";
            fs.dump_contents(nullptr, 2);
            std::cout << "\n";
        }

        CHECK_REPORT_ABORT_ERROR(create_result);
    }

    // partition & format 

    disktools::disk_sector_image_t image;
    const auto image_open_result = image.open(output_option.as<const std::string&>(), fs.size(), disktools::_reformat);
    CHECK_REPORT_ABORT_ERROR(image_open_result);

    disktools::disk_sector_writer_t writer{image};
    if (!image.using_existing())
    {
        create_blank_image(&writer);
    }

    auto part_result = disktools::gpt::create_efi_boot_image(&writer);
    CHECK_REPORT_ABORT_ERROR(part_result);
    
    const auto part_info = part_result.value();
    writer.set_beg(part_info._first_usable_lba);
    auto fat_result = disktools::fat::create_fat_partition(&writer, part_info.num_sectors(), label_option.as<const std::string&>().c_str(), fs);
    CHECK_REPORT_ABORT_ERROR(fat_result);

    delete[] buffer;

    std::cout << "\tboot image created" << std::endl;
}
