#pragma once

#include "status.h"
#include <map>

namespace disktools
{
    // generate more output
    extern bool _verbose;
    // preserve source file name cases or convert all to UPPER
    extern bool _preserve_case;
    // reformat an existing image (if exists)
    extern bool _reformat;

    // ================================================================================================================
    // this is the *only* sector size we support here. UEFI does support other sector sizes but we don't bother and
    // most of the reference literature and definitions assume a 512 byte sector size.
    inline constexpr size_t kSectorSizeBytes = 512;
    inline constexpr uint16_t kMBRSignature = 0xaa55;

    struct disk_sector_writer_t;
    struct fs_t;

    // create a blank image for the size determined in writer
    bool create_blank_image(disk_sector_writer_t* writer);

    // simple helper to write a file in units of 1 sector of kSectorSizeBytes bytes
    struct disk_sector_writer_t
    {
        disk_sector_writer_t(std::fstream&& fs, size_t total_sectors, bool use_existing_image)
            : _fs{ std::move(fs) }
            , _total_sectors{ total_sectors }
            , _use_existing_image{ use_existing_image }
        {
            _start_pos = _fs.tellp();
        }

        ~disk_sector_writer_t()
        {
            delete[] _sector;
            _fs.close();
        }

        // create a writer for a file and size
        static System::status_or_t<disk_sector_writer_t*> create_writer(const std::string&, size_t);

        bool set_pos(size_t lba);

        size_t size() const
        {
            return _total_sectors * kSectorSizeBytes;
        }

        bool using_existing() const
        {
            return _use_existing_image;
        }

        bool good() const
        {
            return _fs.good();
        }

        void reset()
        {
            if (good())
            {
                _fs.seekp(_start_pos, std::ios::beg);
            }
        }

        char* blank_sector(size_t count = 1);

        bool write_at_ex(size_t lba, size_t src_sector_offset, size_t sector_count);

        bool write_sector();

        bool write_at(size_t lba, size_t sector_count);

        size_t  last_lba() const
        {
            return _total_sectors - 1;
        }


        char* _sector = nullptr;
        size_t                  _sectors_in_buffer = 1;
        size_t                  _total_sectors = 0;
        std::fstream            _fs;
        std::fstream::pos_type  _start_pos;
        bool                    _use_existing_image = false;
    };

    struct fs_t
    {
        struct dir_t;
        struct file_t
        {
            dir_t* _parent;
            const void* _data;
            size_t          _size;
            size_t          _start_cluster;
        };

        struct dir_entry_t
        {
            bool        _is_dir;
            union
            {
                dir_t* _dir;
                file_t* _file = nullptr;
            } _content;

            dir_entry_t() = default;
            explicit dir_entry_t(bool is_dir)
                : _is_dir{ is_dir }
            {}
        };

        //NOTE: explicit comparison to support heterogenuous indexing pre C++20...
        using dir_entries_t = std::map<std::string, dir_entry_t, std::less<>>;

        struct dir_t
        {
            dir_t() = default;
            ~dir_t() = default;

            std::string     _name;
            dir_entries_t   _entries;
            size_t          _start_cluster;
            dir_t* _parent;
        };

        fs_t()
        {
            _root._name = "\\";
        }

        size_t size() const
        {
            return _size;
        }

        bool empty() const
        {
            return _root._entries.empty();
        }

        System::status_or_t<bool> add_dir(dir_t* parent, const std::string& sysRootPath);
        System::status_or_t<bool> create_from_source(std::string_view systemRootPath);
        System::status_or_t<dir_t*> create_directory(dir_t* parent, std::string name_);
        System::status_or_t<dir_t*> create_directory(std::string name)
        {
            return create_directory(&_root, name);
        }
        System::status_or_t<file_t*> create_file(dir_t* parent, const std::string& name_, const void* data,
                                                 size_t size);

        void dump_contents(const dir_t* dir = nullptr, int depth = 0) const;

        dir_t           _root{};
        size_t          _size = 0;
    };

    namespace gpt
    {
        struct partition_info_t
        {
            size_t  _first_usable_lba = 0;
            size_t  _last_usable_lba = 0;

            size_t num_sectors() const
            {
                return _last_usable_lba - _first_usable_lba;
            }
        };
        // ======================================================================================================================================================
        //
        // This creates a single partition UEFI disk image which contains the following sections:
        //
        // | protective mbr | primary EFI gpt + GPT partition array | UEFI system partition (FAT format)...[Last usable LBA] | backup GPT partition array + backup GPT |
        //
        //
        // assumes writer is initialised and a blank image has been created.
        //
        System::status_or_t<partition_info_t> create_efi_boot_image(disk_sector_writer_t* writer);
    }

    namespace fat
    {
        // ======================================================================================================================================================
        //
        // format a partition as FAT16 or FAT32 depending on size requirements and initialise it with the contents of fs.
        // 
        System::status_or_t<bool> create_fat_partition(disk_sector_writer_t* writer, size_t total_sectors, const char* volumeLabel, const fs_t& fs);
    }
}
