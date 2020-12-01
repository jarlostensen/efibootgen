#pragma once

#include "status.h"
#include <map>
#include <filesystem>

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
    
    struct disk_sector_writer_t;
    struct fs_t;

    // create a blank image for the size determined in writer
    bool create_blank_image(disk_sector_writer_t* writer);

    // adapter for a generic read/write image file
    struct disk_sector_image_t
    {
        disk_sector_image_t() = default;
        ~disk_sector_image_t() = default;

        [[nodiscard]]
        size_t size() const
        {
            return _total_sectors * kSectorSizeBytes;
        }
        [[nodiscard]]
        bool good() const
        {
            return _fs.good();
        }
        [[nodiscard]]
        std::ios::iostate   iostate() const
        {
            return _fs.rdstate();
        }
        [[nodiscard]]
        size_t total_sectors() const
        {
            return _total_sectors;
        }
        [[nodiscard]]
        size_t  last_lba() const
        {
            return _total_sectors - 1;
        }
        [[nodiscard]]
        bool using_existing() const
        {
            return _using_existing;
        }
        // open/create an image that can hold at least content_size bytes.
        // if reformat: if file exists and is big enough it will be overwritten, otherwise it will be truncated
        System::status_t open(const std::string& oName, size_t content_size, bool reformat);

        size_t                  _total_sectors = 0;
        std::fstream            _fs;
        bool                    _using_existing = false;
    };

    // simple helper to write a file in units of 1 sector of kSectorSizeBytes bytes
    struct disk_sector_writer_t
    {
        explicit disk_sector_writer_t(disk_sector_image_t& sector_stream)
            : _image{sector_stream}
        {
        }
        ~disk_sector_writer_t()
        {
            delete[] _sector;            
        }

        disk_sector_image_t& image() const
        {
            return _image;
        }
        // seek lba sectors from beginning
        bool seek_from_beg(size_t lba);
        // set start position for subsequent seek_from_beg        
        void set_beg(size_t lba);        
        size_t get_beg_lba() const
        {
            return size_t(_seek_beg)/kSectorSizeBytes;
        }
        // image fs iostate
        std::ios::iostate iostate() const
        {
            return _image.iostate();
        }
        // allocate (if need be) and return a pointer to array of count kSectorSizeBytes 
        char* blank_sector(size_t count = 1);
        // write current sector at current position
        bool write_sector();
        // write sector number sector_index to current position
        bool write_sector_index(size_t sector_index);
        // write count sectors
        bool write_sectors(size_t count);
        
        disk_sector_image_t&        _image;
        char*                       _sector = nullptr;
        size_t                      _sectors_in_buffer = 1;
        std::ofstream::pos_type     _seek_beg{};
    };

    struct disk_sector_reader_t
    {
        explicit disk_sector_reader_t(disk_sector_image_t& sector_stream)
            : _image{sector_stream}
        {
        }
        ~disk_sector_reader_t()
        {
            delete[] _sector;
        }
        disk_sector_image_t& image() const
        {
            return _image;
        }
        // seek lba sectors from beginning
        bool seek_from_beg(size_t lba);
        // set start position for subsequent seek_from_beg
        bool set_beg(size_t lba);
        // read one sector at current pos
        bool read_sector();
        // last read sector data
        char* sector() const
        {
            return _sector;
        }
        // image fs iostate
        std::ios::iostate iostate() const
        {
            return _image.iostate();
        }

        char*                       _sector = nullptr;
        disk_sector_image_t&        _image;
        std::ifstream::pos_type     _seek_beg{};
    };

    struct fs_t
    {
        struct dir_t;
        struct file_t
        {
            dir_t*          _parent;
            const void*     _data;
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

        using mount_point_t = void*;
        System::status_or_t<mount_point_t> mount(disk_sector_reader_t* reader, size_t root_dir_start_lba, size_t first_data_lba, size_t sectors_per_cluster);
    }
}
