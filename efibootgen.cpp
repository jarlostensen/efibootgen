
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <random>
#include <functional>
#include <filesystem>
#include <map>
#include <stack>
#include <cstdarg>
namespace fs = std::filesystem;

// none of these are critical to us at this point
#pragma warning(disable:5045)
#pragma warning(disable:4514)
#pragma warning(disable:4820)

#include "status.h"
#include "fat.h"
#include "gpt.h"
#include "jopts.h"

namespace utils
{
    // https://rosettacode.org/wiki/CRC-32#C
    uint32_t rc_crc32(uint32_t crc, const char* buf, size_t len)
    {
        static uint32_t table[256];
        static int have_table = 0;

        if (have_table == 0) {
            for (auto i = 0u; i < 256u; i++) {
                auto rem = i;
                for (auto j = 0u; j < 8u; j++) {
                    if (rem & 1) {
                        rem >>= 1;
                        rem ^= 0xedb88320;
                    }
                    else
                        rem >>= 1;
                }
                table[i] = rem;
            }
            have_table = 1;
        }

        crc = ~crc;
        const auto* q = buf + len;
        for (const auto* p = buf; p < q; p++) {
            const auto octet = static_cast<unsigned char>(*p);  /* Cast to unsigned octet. */
            crc = (crc >> 8) ^ table[(crc & 0xff) ^ octet];
        }
        return ~crc;
    }

    namespace uuid
    {
        std::random_device              rd;
        std::mt19937                    gen(rd());
        std::uniform_int_distribution<> dis(0, 15);

        void generate(uint8_t* uuid)
        {
            // yes, it's just random numbers...
            std::generate(uuid, uuid + 16, []()->char { return static_cast<char>(dis(gen) & 0xff); });
        }

        uint32_t rand_int()
        {
            return uint32_t(dis(gen));
        }
    }
}


namespace disktools
{
    // ================================================================================================================
    // this is the *only* sector size we support here. UEFI does support other sector sizes but we don't bother and
    // most of the reference literature and definitions assume a 512 byte sector size.
    static constexpr size_t kSectorSizeBytes = 512;
    static constexpr uint16_t kMBRSignature = 0xaa55;

    struct fs_t;

    // generate more output
    auto _verbose = false;
    // preserve source file name cases or convert all to UPPER
    auto _preserve_case = false;
    // reformat an existing image (if exists)
    auto _reformat = false;

    // helper to make it a bit more intuitive to use and write sectors to a file
    struct disk_sector_writer_t
    {
        disk_sector_writer_t(std::fstream&& fs, size_t total_sectors, bool use_existing_image)
            : _fs{ std::move(fs) }
            , _total_sectors{ total_sectors }
            , _use_existing_image{ use_existing_image }
        {
            _start_pos = _fs.tellp();
        }

        disk_sector_writer_t(disk_sector_writer_t&& rhs)
            : _fs{ std::move(rhs._fs) }
            , _sector{ rhs._sector }
            , _total_sectors{ rhs._total_sectors }
            , _sectors_in_buffer{ rhs._sectors_in_buffer }
            , _start_pos{ rhs._start_pos }
            , _use_existing_image{ rhs._use_existing_image }
        {
            rhs._sector = nullptr;
        }

        ~disk_sector_writer_t()
        {
            delete[] _sector;
            _fs.close();
        }

        static System::status_or_t<disk_sector_writer_t*> create_writer(const std::string&, size_t);

        bool set_pos(size_t lba)
        {
            if (good())
            {
                _fs.seekp(_start_pos + std::ofstream::pos_type(lba * kSectorSizeBytes), std::ios::beg);
                _start_pos = _fs.tellp();
                return good();
            }
            return false;
        }

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
            return _fs.is_open() && _fs.good();
        }

        void reset()
        {
            if (good())
                _fs.seekp(_start_pos, std::ios::beg);
        }

        void flush()
        {
            if (good())
            {
                _fs.flush();
            }
        }

        char* blank_sector(size_t count = 1)
        {
            if (!_sector || _sectors_in_buffer < count)
            {
                delete[] _sector;
                _sector = new char[kSectorSizeBytes * (_sectors_in_buffer = count)];
            }
            else if (_sectors_in_buffer > count)
            {
                _sectors_in_buffer = count;
            }
            memset(_sector, 0, _sectors_in_buffer * kSectorSizeBytes);
            return _sector;
        }

        bool        write_at_ex(size_t lba, size_t src_sector_offset, size_t sector_count)
        {
            _fs.seekp(_start_pos + std::ofstream::pos_type(lba * kSectorSizeBytes), std::ios::beg);
            if (!_fs.good() || sector_count > _sectors_in_buffer)
            {
                return false;
            }
            _fs.write(_sector + src_sector_offset * kSectorSizeBytes, (sector_count * kSectorSizeBytes));
            return _fs.good();
        }

        bool        write_at(size_t lba, size_t sector_count)
        {
            _fs.seekp(_start_pos + std::ofstream::pos_type(lba * kSectorSizeBytes), std::ios::beg);
            if (!_fs.good())
            {
                return false;
            }
            _fs.write(_sector, (sector_count * kSectorSizeBytes));
            return _fs.good();
        }

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


    // like "dd"; create a blank image of writer->_total_sectors sectors
    bool create_blank_image(disk_sector_writer_t* writer)
    {
        if (_verbose)
        {
            std::cout << "\tcreating blank image of " << writer->_total_sectors << " " << kSectorSizeBytes << " byte sectors\n";
        }

        writer->reset();
        writer->blank_sector();
        auto sector_count = writer->_total_sectors;
        while (writer->good() && sector_count--)
        {
            writer->_fs.write(writer->_sector, kSectorSizeBytes);
        }
        const auto result = writer->good();
        writer->reset();
        return result;
    }

    // a basic container for files and directories in a hierarchy
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

        System::status_or_t<bool> add_dir(dir_t* parent, const std::string& sysRootPath) {

            //NOTE: we need to keep track of the current directory entry
            //      which is why this code does the recrusion "manually" instead of using recursive_directory_iterator

            using recursion_stack_item_t = std::pair<dir_t*, fs::directory_iterator>;
            using recursion_stack_t = std::stack<recursion_stack_item_t>;
            recursion_stack_t rec_stack;

            fs::path fsSystemRootPath{ sysRootPath };
            auto i = fs::directory_iterator(fsSystemRootPath);
            for (;;)
            {
                if (i != fs::directory_iterator())
                {
                    if (i->is_directory())

                    {
                        auto result = create_directory(parent, i->path().filename().string());
                        if (result)
                        {
                            const auto rec_path = i->path();
                            //NOTE: advance past current entry before pushing
                            rec_stack.emplace(parent, std::move(++i));
                            parent = result.value();
                            i = fs::directory_iterator(rec_path);
                        }
                        else
                        {
                            return result.error_code();
                        }
                    }
                    else
                    {
                        const auto path = i->path().string();
                        std::ifstream ifs{ path, std::ios::binary };
                        if (ifs.is_open())
                        {
                            ifs.seekg(0, std::ios::end);
                            const auto endpos = ifs.tellg();
                            ifs.seekg(0, std::ios::beg);

                            const auto size = size_t(endpos);
                            auto* buffer = new char[size];
                            ifs.read(buffer, size);
                            ifs.close();

                            /* _ =*/ create_file(parent,
                                //NOTE: "FOO.BAR" -> "FOO BAR"
                                i->path().stem().string() + " " + i->path().filename().extension().string().substr(1),
                                buffer, size);

                            // next item
                            ++i;
                        }
                        else
                        {
                            return System::Code::UNAVAILABLE;
                        }
                    }
                }

                if (i == fs::directory_iterator())
                {
                    // end of this directory, pop the stack or end
                    if (rec_stack.empty())

                    {
                        break;
                    }

                    i = std::move(rec_stack.top().second);
                    parent = rec_stack.top().first;
                    rec_stack.pop();
                }
            }

            return true;
        };

        [[nodiscard]]
        System::status_or_t<bool> create_from_source(std::string_view systemRootPath)
        {
            // strip any leading gunk so that the name is clean for the root directory entry
            const auto name_start = systemRootPath.find_first_not_of("./\\");
            if (name_start == std::string::npos)
            {
                return System::Code::NOT_FOUND;
            }

            const std::string stripped_root_path{ systemRootPath.substr(name_start) };
            const auto result = create_directory(&_root, stripped_root_path);
            if (!result)
            {
                return result.error_code();
            }
            return add_dir(result.value(), stripped_root_path);
        }

        [[nodiscard]]
        System::status_or_t<dir_t*> create_directory(dir_t* parent, std::string name_)
        {
            std::string_view name = name_;
            if (!_preserve_case)
            {
                std::transform(name_.begin(), name_.end(), name_.begin(), ::toupper);
            }

            assert(parent->_entries.find(name) == parent->_entries.end());

            dir_entry_t dir_entry{ true };
            dir_entry._content._dir = new dir_t;
            dir_entry._content._dir->_name = std::move(name_);
            dir_entry._content._dir->_parent = parent;
            //NOTE: a directory is limited to 512 bytes = 16 entries here
            _size += kSectorSizeBytes;

            parent->_entries.emplace(dir_entry._content._dir->_name, dir_entry);
            return dir_entry._content._dir;
        }

        [[nodiscard]]
        System::status_or_t<dir_t*> create_directory(std::string name)
        {
            return create_directory(&_root, name);
        }

        [[nodiscard]]
        System::status_or_t<file_t*> create_file(dir_t* parent, const std::string& name_, const void* data, size_t size)
        {
            std::string name = name_;
            if (!_preserve_case)
            {
                std::transform(name_.begin(), name_.end(), name.begin(), ::toupper);
            }

            assert(parent->_entries.find(name) == parent->_entries.end());
            assert(data && size);

            dir_entry_t dir_entry{ false };
            dir_entry._content._file = new file_t;
            dir_entry._content._file->_parent = parent;
            dir_entry._content._file->_data = data;
            dir_entry._content._file->_size = size;
            _size += size;

            //NOTE: index by NAME not source path
            parent->_entries.emplace(name, dir_entry);

            return dir_entry._content._file;
        }

        void dump_contents(const dir_t* dir = nullptr, int depth = 0) const
        {
            if (!dir)
                dir = &_root;

            const auto pad = std::string(depth * 4, ' ');
            for (auto const& [key, val] : dir->_entries)
            {
                //NOTE: setw just doesn't work, why?
                std::cout << pad << key << (val._is_dir ? "\\" : "") << std::endl;
                if (val._is_dir)
                {
                    dump_contents(val._content._dir, depth + 1);
                }
            }
        }

        dir_t           _root{};
        size_t          _size = 0;
    };

    System::status_or_t<disk_sector_writer_t*> disk_sector_writer_t::create_writer(const std::string& oName, size_t content_size)
    {
        // round size up to nearest 128 Megs. This pushes us out of the "floppy disk" domain
        size_t size = (content_size + (0x8000000 - 1)) & ~(0x8000000 - 1);

        // if the disk image already exists, and we're reformatting, then we'll just keep it (as long as it's big enough)
        std::fstream file;
        auto using_existing = false;
        if (disktools::_reformat)
        {
            file.open(oName, std::ios::binary | std::ios::in | std::ios::out);
            if (file.good())
            {
                file.seekg(0, std::ios::end);
                size_t image_size = file.tellg();
                file.seekg(0);
                if (image_size >= size)
                {
                    if (disktools::_verbose)
                    {
                        std::cout << "\tre-using existing disk image " << oName << "\n";
                    }

                    size = image_size;
                    using_existing = true;
                }
            }
        }

        if (!using_existing)
        {
            file.open(oName, std::ios::binary | std::ios::trunc | std::ios::out);
        }

        if (!file.is_open())
        {
            return System::Code::NOT_FOUND;
        }

        // round up to nearest 512 byte block
        size = (size + (kSectorSizeBytes - 1)) & ~(kSectorSizeBytes - 1);
        const auto blocks = size / kSectorSizeBytes;

        return new disk_sector_writer_t{ std::move(file), blocks, using_existing };
    }

    namespace fat
    {
        static constexpr uint8_t kFatOemName[8] = { 'j','O','S','X',' ','6','4',' ' };
        static constexpr uint8_t kRootFolderName[11] = { 'E','F','I' };

        enum class validation_result
        {
            kOk,
            kUninitialisedPartition,
            kUnsupportedSectorSize,
            kUnsupportedFat12,
            kInvalidFat32Structure,
            kInvalidFat16Structure,
            kInvalidVersion,
            kInvalidReservedField,
            kInvalidFatTypeCalculation,
            kCorruptFat32,
            kCorruptFat16,
            kVolumeMayHaveErrors,
            kFat32FsInfoCorrupt,
            kFat32CorruptRootDirectory,

            kNotValidated,
        };

        // reads and validates a FAT formatted partition
        // left in for reference
        struct reader
        {

            reader(std::ifstream& rawImage, size_t sectorCount)
                : _raw_image(rawImage)
                , _total_sectors{ sectorCount }
            {
                _partition_start = rawImage.tellg();
            }

            ~reader()
            {
                //ZZZ: does delete depend on the type, check it
                if (type == fat_type::kFat16)
                {
                    delete[] _fat._16;
                }
                else
                {
                    delete[] _fat._32;
                }
            }

            validation_result operator()()
            {
                char sector[kSectorSizeBytes];

                // read BPB
                _raw_image.read(sector, kSectorSizeBytes);
                const auto* boot_sector = reinterpret_cast<const fat_boot_sector_t*>(sector);
                memcpy(&_boot_sector, boot_sector, sizeof fat_boot_sector_t);

                if ((_boot_sector._jmp[0] != kShortJmp && _boot_sector._jmp[0] != kLongJmp)
                    ||
                    (reinterpret_cast<const uint16_t*>(sector + 510)[0] != kMBRSignature)
                    )
                {
                    return (_validation_result = validation_result::kUninitialisedPartition);
                }

                if (_boot_sector._bpb._bytes_per_sector != kSectorSizeBytes)
                {
                    return (_validation_result = validation_result::kUnsupportedSectorSize);
                }

                _root_dir_sector_count = ((_boot_sector._bpb._root_entry_count * 32) + (kSectorSizeBytes - 1)) / kSectorSizeBytes;

                const auto num_clusters = _total_sectors / _boot_sector._bpb._sectors_per_cluster;
                auto sectors_per_fat = 0u;
                auto total_sectors = 0u;

                // this IS NOT WRONG (see MS FAT tech document)
                if (num_clusters < 4085)
                {
                    // FAT12, we don't support it
                    return (_validation_result = validation_result::kUnsupportedFat12);
                }
                if (num_clusters < 65525)
                {
                    type = fat_type::kFat16;
                    const auto* fat16_bpb = reinterpret_cast<const fat16_extended_bpb*>(boot_sector + 1);
                    memcpy(&_extended_bpb._fat16, fat16_bpb, sizeof fat16_extended_bpb);

                    sectors_per_fat = _boot_sector._bpb._sectors_per_fat16;
                    total_sectors = _boot_sector._bpb._total_sectors16;

                    if (sectors_per_fat == 0 || total_sectors == 0)
                    {
                        return (_validation_result = validation_result::kInvalidFat16Structure);
                    }
                }
                else
                {
                    type = fat_type::kFat32;
                    const auto* fat32_bpb = reinterpret_cast<const fat32_extended_bpb*>(boot_sector + 1);
                    memcpy(&_extended_bpb._fat32, fat32_bpb, sizeof fat32_extended_bpb);

                    sectors_per_fat = fat32_bpb->_sectors_per_fat;
                    total_sectors = _boot_sector._bpb._total_sectors32;

                    if (_boot_sector._bpb._sectors_per_fat16 != 0 || fat32_bpb->_sectors_per_fat == 0 || total_sectors == 0)
                    {
                        return (_validation_result = validation_result::kInvalidFat32Structure);
                    }
                    if (fat32_bpb->_version != 0)
                    {
                        return (_validation_result = validation_result::kInvalidVersion);
                    }

                    // FSInfo
                    _raw_image.seekg(size_t(_partition_start) + (fat32_bpb->_information_sector) * kSectorSizeBytes, std::ios::beg);
                    _raw_image.read(sector, kSectorSizeBytes);
                    const auto* fsinfo = reinterpret_cast<const fat32_fsinfo*>(sector);
                    if (fsinfo->_lead_sig != kFsInfoLeadSig
                        ||
                        fsinfo->_struc_sig != kFsInfoStrucSig
                        ||
                        fsinfo->_tail_sig != kFsInfoTailSig
                        )
                    {
                        return (_validation_result = validation_result::kFat32FsInfoCorrupt);
                    }
                }

                first_data_lba = _boot_sector._bpb._reserved_sectors + (_boot_sector._bpb._num_fats * sectors_per_fat) + _root_dir_sector_count;
                const auto data_sector_count = total_sectors - (_boot_sector._bpb._reserved_sectors + (_boot_sector._bpb._num_fats * sectors_per_fat) + _root_dir_sector_count);
                const auto calc_num_clusters = data_sector_count / _boot_sector._bpb._sectors_per_cluster;

                if (type == fat_type::kFat32
                    &&
                    calc_num_clusters < 65525)
                {
                    return (_validation_result = validation_result::kInvalidFatTypeCalculation);
                }

                // first FAT
                _raw_image.seekg(size_t(_partition_start) + _boot_sector._bpb._reserved_sectors * kSectorSizeBytes, std::ios::beg);
                _raw_image.read(sector, kSectorSizeBytes);

                auto root_dir_start_lba = 0u;

                if (type == fat_type::kFat16)
                {
                    // 16 bit FAT
                    const auto* fat16 = reinterpret_cast<const uint16_t*>(sector);

                    if ((fat16[0] & 0xff) != _boot_sector._bpb._media_descriptor
                        ||
                        (fat16[0] | 0xff) != 0xffff)
                    {
                        return (_validation_result = validation_result::kCorruptFat16);
                    }

                    if (fat16[1] < kFat16EOC)
                    {
                        return (_validation_result = validation_result::kCorruptFat16);
                    }

                    // clean shutdown bitmask or hardware error mask
                    if ((fat16[1] & 0x8000) == 0
                        ||
                        (fat16[1] & 0x4000) == 0
                        )
                    {
                        return (_validation_result = validation_result::kVolumeMayHaveErrors);
                    }

                    // copy FAT#1 
                    _fat._16 = new uint16_t[_boot_sector._bpb._sectors_per_fat16 * kSectorSizeBytes];
                    memcpy(_fat._16, sector, kSectorSizeBytes);
                    auto fat_sectors_left = _boot_sector._bpb._sectors_per_fat16 - 1;
                    auto* fat_sector = reinterpret_cast<char*>(_fat._16) + kSectorSizeBytes;
                    while (fat_sectors_left--)
                    {
                        _raw_image.read(sector, kSectorSizeBytes);
                        memcpy(fat_sector, sector, kSectorSizeBytes);
                        fat_sector += kSectorSizeBytes;
                    }

                    root_dir_start_lba = _boot_sector._bpb._reserved_sectors + (_boot_sector._bpb._num_fats * _boot_sector._bpb._sectors_per_fat16);
                }
                else
                {
                    // 32 (28) bit FAT
                    const auto* fat32 = reinterpret_cast<const uint32_t*>(sector);

                    // check special entries 0 and 1
                    const auto entry0 = fat32[0];
                    if ((entry0 & 0xff) != _boot_sector._bpb._media_descriptor
                        ||
                        ((entry0 | 0xff) != 0x0fffffff)
                        )
                    {
                        return (_validation_result = validation_result::kCorruptFat32);
                    }
                    // must be a valid EOC marker
                    if (fat32[1] < kFat32EOC)
                    {
                        return (_validation_result = validation_result::kCorruptFat32);
                    }

                    // clean shutdown bitmask or hardware error mask
                    if ((fat32[1] & 0x08000000) == 0
                        ||
                        (fat32[1] & 0x04000000) == 0
                        )
                    {
                        return (_validation_result = validation_result::kVolumeMayHaveErrors);
                    }

                    // copy FAT#1 
                    _fat._32 = new uint32_t[_extended_bpb._fat32._sectors_per_fat * kSectorSizeBytes];
                    memcpy(_fat._32, sector, kSectorSizeBytes);
                    auto fat_sectors_left = _extended_bpb._fat32._sectors_per_fat - 1;
                    auto* fat_sector = reinterpret_cast<char*>(_fat._32) + kSectorSizeBytes;
                    while (fat_sectors_left--)
                    {
                        _raw_image.read(sector, kSectorSizeBytes);
                        memcpy(fat_sector, sector, kSectorSizeBytes);
                        fat_sector += kSectorSizeBytes;
                    }

                    // read and check the root directory
                    //const auto root_dir_fat_entry = fat32[_extended_bpb._fat32._root_cluster];
                    root_dir_start_lba = first_data_lba + ((_extended_bpb._fat32._root_cluster - 2) * _boot_sector._bpb._sectors_per_cluster);
                    _raw_image.seekg(size_t(_partition_start) + root_dir_start_lba * kSectorSizeBytes, std::ios::beg);
                    _raw_image.read(sector, kSectorSizeBytes);
                    _root_dir = reinterpret_cast<fat_dir_entry_t*>(sector);
                    if ((_root_dir->_attrib & uint8_t(fat_file_attribute::kVolumeId)) == 0)
                    {
                        return (_validation_result = validation_result::kFat32CorruptRootDirectory);
                    }
                }

                // read the first root directory cluster
                _raw_image.seekg(size_t(_partition_start) + root_dir_start_lba * kSectorSizeBytes, std::ios::beg);
                const auto dir_entries_per_cluster = (kSectorSizeBytes / 32) * _boot_sector._bpb._sectors_per_cluster;
                _root_dir = new fat_dir_entry_t[dir_entries_per_cluster];
                _raw_image.read(reinterpret_cast<char*>(_root_dir), (dir_entries_per_cluster * kSectorSizeBytes * _boot_sector._bpb._sectors_per_cluster));

                // re-set before we exit cleanly
                _raw_image.seekg(_partition_start, std::ios::beg);
                return (_validation_result = validation_result::kOk);
            }

            bool get_volume_label(char* dest, size_t destSize) const
            {
                if (_validation_result != validation_result::kOk || destSize < 12)
                    return false;

                memcpy(dest, _root_dir->_short_name, sizeof _root_dir->_short_name);
                dest[11] = 0;

                return true;
            }

            std::ifstream& _raw_image;
            std::ios::pos_type      _partition_start;
            fat_boot_sector_t		_boot_sector{};
            union _extended_bpb
            {
                fat16_extended_bpb	_fat16;
                fat32_extended_bpb	_fat32;
                _extended_bpb() : _fat32() {}
            }
            _extended_bpb;

            validation_result _validation_result = validation_result::kNotValidated;

            union
            {
                uint16_t* _16;
                uint32_t* _32;
            }
            _fat{ nullptr };

            fat_dir_entry_t* _root_dir = nullptr;
            size_t					_root_dir_sector_count = 0;
            size_t					first_data_lba = 0;
            size_t					_size{};
            size_t					_num_clusters{};
            size_t					_total_sectors;
            fat_type				type{};
        };

        // helper to write out the FAT for files and directories in an fs_t container
        struct write_fat16_context_t
        {
            uint16_t* _fat16 = nullptr;
            size_t          _cluster_idx = 0;
            size_t          _fat_sector = 0;
            size_t          _max_clusters_per_sector = 0;
            size_t          _bytes_per_cluster = 0;
            size_t          _entries_per_cluster = 0;

            // allocates and writes FAT clusters for the directory structure from dir *depth first*
            void write_dir(disk_sector_writer_t* writer, const fs_t::dir_t* dir)
            {
                for (const auto& [name, entry] : dir->_entries)
                {
                    if (entry._is_dir)
                    {
                        //TODO: more things
                        assert(entry._content._dir->_entries.size() <= _entries_per_cluster);
                        entry._content._dir->_start_cluster = _cluster_idx;
                        _fat16[_cluster_idx++] = kFat16EOC;
                        // recurse
                        write_dir(writer, entry._content._dir);
                    }
                    else
                    {
                        // file
                        const auto num_clusters = entry._content._file->_size / _bytes_per_cluster + (entry._content._file->_size % _bytes_per_cluster ? 1 : 0);
                        entry._content._file->_start_cluster = _cluster_idx;
                        for (auto n = 1u; n < num_clusters; ++n)
                        {
                            _fat16[_cluster_idx++] = n + entry._content._file->_start_cluster;
                            if (_cluster_idx == _max_clusters_per_sector)
                            {
                                if (n == (num_clusters - 1))
                                {
                                    _fat16[_cluster_idx] = kFat16EOC;
                                }

                                // next FAT sector
                                writer->write_at(_fat_sector++, 1);
                                _fat16 = reinterpret_cast<uint16_t*>(writer->blank_sector());
                                _cluster_idx = 0;
                            }
                        }

                        if (_cluster_idx)
                        {
                            _fat16[_cluster_idx] = kFat16EOC;
                            writer->write_at(_fat_sector, 1);
                        }
                    }
                }
            };
        };

        void write_fat16(disk_sector_writer_t* writer, fat_boot_sector_t& boot_sector, const fs_t& fs)
        {
            auto* sector = writer->blank_sector();

            write_fat16_context_t ctx;
            ctx._fat16 = reinterpret_cast<uint16_t*>(sector);
            ctx._max_clusters_per_sector = kSectorSizeBytes / sizeof(uint16_t);
            ctx._bytes_per_cluster = boot_sector._bpb._sectors_per_cluster * kSectorSizeBytes;
            ctx._entries_per_cluster = ctx._bytes_per_cluster / sizeof(fat_dir_entry_t);

            ctx._fat_sector = boot_sector._bpb._reserved_sectors;
            ctx._cluster_idx = 2;

            // fixed entries 0 and 1
            ctx._fat16[0] = 0xff00 | boot_sector._bpb._media_descriptor;
            ctx._fat16[1] = kFat16EOC;

            // recurse the directories and files
            ctx.write_dir(writer, &fs._root);
        }

        using cluster_to_lba_func_t = std::function<size_t(size_t)>;


        void write_file(disk_sector_writer_t* writer, cluster_to_lba_func_t cluster_to_lba, const fs_t::dir_entry_t& entry)
        {
            // the contents of a file are laid out in a linear chain starting at 
            // the start cluster, here we just copy it in sector by sector
            auto file_sector = cluster_to_lba(entry._content._file->_start_cluster);
            auto bytes_left = static_cast<long long>(entry._content._file->_size);
            const auto* bytes = static_cast<const char*>(entry._content._file->_data);
            do
            {
                memcpy(writer->blank_sector(), bytes, kSectorSizeBytes);
                writer->write_at(file_sector++, 1);
                bytes += kSectorSizeBytes;
                bytes_left -= kSectorSizeBytes;
            } while (bytes_left >= static_cast<long long>(kSectorSizeBytes));

            if (bytes_left > 0)
            {
                memcpy(writer->blank_sector(), bytes, bytes_left);
                writer->write_at(file_sector, 1);
            }
        }


        void write_dir(disk_sector_writer_t* writer, cluster_to_lba_func_t cluster_to_lba, const fs_t::dir_entry_t& entry_)
        {
            auto* dir_entry = reinterpret_cast<fat_dir_entry_t*>(writer->blank_sector());

            // add standard "." and ".." entries
            dir_entry->set_name(".");
            dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
            dir_entry->_first_cluster_lo = entry_._content._dir->_start_cluster;
            dir_entry++;
            dir_entry->set_name("..");
            dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
            dir_entry->_first_cluster_lo = entry_._content._dir->_parent->_start_cluster;
            dir_entry++;

            const auto entries = entry_._content._dir->_entries;
            for (auto& [name, entry] : entries)
            {
                dir_entry->set_name(name.c_str());
                if (entry._is_dir)
                {
                    dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
                    dir_entry->_first_cluster_lo = entry._content._dir->_start_cluster;

                    if (_verbose)
                    {
                        std::cout << "\t\tadded directory \"" << name << "\", starting at cluster " << dir_entry->_first_cluster_lo << "\n";
                    }
                }
                else
                {
                    dir_entry->_size = entry._content._file->_size;
                    dir_entry->_first_cluster_lo = entry._content._file->_start_cluster;

                    if (_verbose)
                    {
                        std::cout << "\t\tadded file \"" << name << "\", " << dir_entry->_size << " bytes, starting at cluster " << dir_entry->_first_cluster_lo << "\n";
                    }
                }
                ++dir_entry;
            }
            writer->write_at(cluster_to_lba(entry_._content._dir->_start_cluster), 1);

            // now recurse...
            //NOTE: having to do this and not [name, entry] is down to some internal ms build 142 compiler issue which I have no intent on tracking down
            for (auto i : entries)
            {
                if (i.second._is_dir)
                {
                    write_dir(writer, cluster_to_lba, i.second);
                }
                else
                {
                    write_file(writer, cluster_to_lba, i.second);
                }
            }
        }

        System::status_or_t<bool> write_fs_contents_to_disk(disk_sector_writer_t* writer, size_t root_dir_start_lba,
            size_t first_data_lba, size_t sectors_per_cluster,
            const char* volumeLabel, const fs_t& fs)
        {
            const auto cluster_to_lba = [=](size_t cluster) -> size_t {
                return first_data_lba + ((cluster - 2) * sectors_per_cluster);
            };

            // the first entry is always the volume label entry (which must match the volume label set in the BPB)
            auto* dir_entry = reinterpret_cast<fat_dir_entry_t*>(writer->blank_sector());
            dir_entry->set_name(volumeLabel);
            dir_entry->_attrib = uint8_t(fat_file_attribute::kVolumeId);
            ++dir_entry;

            if (_verbose)
            {
                std::cout << "\tvolume label \"" << volumeLabel << "\"\n";
            }

            // the root directory (for either FAT16 or FAT32) is special and has no '.' or '..' entries
            //TODO:ZZZ: check for overrun
            for (auto& [name, entry] : fs._root._entries)
            {
                dir_entry->set_name(name.c_str());
                if (entry._is_dir)
                {
                    dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
                    dir_entry->_first_cluster_lo = entry._content._dir->_start_cluster;

                    if (_verbose)
                    {
                        std::cout << "\tadded directory \"" << name << "\", starting at cluster " << dir_entry->_first_cluster_lo << "\n";
                    }
                }
                else
                {
                    dir_entry->_size = entry._content._file->_size;
                    dir_entry->_first_cluster_lo = entry._content._file->_start_cluster;

                    if (_verbose)
                    {
                        std::cout << "\tadded file \"" << name << "\", " << dir_entry->_size << " bytes, starting at cluster " << dir_entry->_first_cluster_lo << "\n";
                    }
                }
                ++dir_entry;
            }
            writer->write_at(root_dir_start_lba, 1);

            // remaining file system contents
            for (auto& [name, entry] : fs._root._entries)
            {
                if (entry._is_dir)
                {
                    write_dir(writer, cluster_to_lba, entry);
                }
                else
                {
                    write_file(writer, cluster_to_lba, entry);
                }
            }

            return true;
        }

        System::status_or_t<bool> format_efi_boot_partition(disk_sector_writer_t* writer, size_t total_sectors, const char* volumeLabel, const fs_t& fs)
        {
            if (!writer->good() || !total_sectors)
                return System::Code::FAILED_PRECONDITION;

            const auto size = static_cast<unsigned long long>(total_sectors * kSectorSizeBytes);

            // =======================================================================================
            // boot sector
            //
            fat_boot_sector_t boot_sector;
            memset(&boot_sector, 0, sizeof boot_sector);
            boot_sector._bpb._bytes_per_sector = kSectorSizeBytes;
            boot_sector._bpb._num_fats = 2;				    // industry standard
            boot_sector._bpb._media_descriptor = 0xf8;		// fixed disk partition type
            // this isn't used, but it should still be valid
            boot_sector._jmp[0] = kLongJmp;

            // we need to specify geometry information (heads, cylinders, and sectors per track) for the MBR to be valid
            // so we calculate these values according to the table https://en.wikipedia.org/wiki/Logical_block_addressing#LBA-assisted_translation        
            boot_sector._bpb._sectors_per_track = 63;
            if (size <= 0x1f800000)
            {
                boot_sector._bpb._num_heads = 16;
            }
            else if (size <= 0x3f000000)
            {
                boot_sector._bpb._num_heads = 32;
            }
            else if (size <= 0x7e000000)
            {
                boot_sector._bpb._num_heads = 64;
            }
            else if (size <= 0xfc000000)
            {
                boot_sector._bpb._num_heads = 128;
            }
            else
            {
                // maxed out, can't go higher
                boot_sector._bpb._num_heads = 255;
            }

            // the extended bpb depends on the type of FAT
            union
            {
                fat16_extended_bpb    _fat16;
                fat32_extended_bpb    _fat32;
            }
            extended_bpb{};
            fat_type type;

            char* extended_bpb_ptr = nullptr;
            size_t extended_bpb_size = 0;

            // as per MS Windows' standard; any volume of size < 512MB shall be FAT16
            if (size < 0x20000000)
            {
                type = fat_type::kFat16;

                // anything not set defaults to 0
                memset(&extended_bpb._fat16, 0, sizeof extended_bpb._fat16);

                if (total_sectors < 0x1000)
                {
                    // total_sectors32 = 0
                    boot_sector._bpb._total_sectors16 = total_sectors & 0xffff;
                }
                else
                {
                    // total_sectors16 = 0
                    boot_sector._bpb._total_sectors32 = total_sectors;
                }

                boot_sector._bpb._reserved_sectors = 1;		// as per standard for FAT16
                boot_sector._bpb._root_entry_count = 512;		// as per standard for FAT16
                extended_bpb._fat16._drive_num = 0x80;
                extended_bpb._fat16._boot_sig = 0x29;
                extended_bpb._fat16._volume_serial = utils::uuid::rand_int();
                //NOTE: this must match what is set in the root directory below
                memset(extended_bpb._fat16._volume_label, 0x20, sizeof extended_bpb._fat16._volume_label);
                memcpy(extended_bpb._fat16._volume_label, volumeLabel, std::min(sizeof extended_bpb._fat16._volume_label, strlen(volumeLabel)));
                memcpy(extended_bpb._fat16._file_sys_type, kFat16FsType, sizeof kFat16FsType);

                // from MS' white paper on FAT
                for (const auto& entry : kDiskTableFat16)
                {
                    if (total_sectors <= entry._sector_limit)
                    {
                        boot_sector._bpb._sectors_per_cluster = entry._sectors_per_cluster;
                        break;
                    }
                }

                extended_bpb_ptr = reinterpret_cast<char*>(&extended_bpb._fat16);
                extended_bpb_size = sizeof extended_bpb._fat16;

                if (_verbose)
                {
                    std::cout << "\tfilesystem is FAT16\n";
                }
            }
            else
            {
                type = fat_type::kFat32;

                memset(&extended_bpb._fat32, 0, sizeof extended_bpb._fat32);

                // total_sectors16 = 0
                boot_sector._bpb._total_sectors32 = total_sectors;
                boot_sector._bpb._reserved_sectors = 32;		// as per standard for FAT32, this is 16K

                extended_bpb._fat32._flags = 0x80;				// no mirroring, FAT 0 is active	
                extended_bpb._fat32._root_cluster = 2;			// data cluster where the root directory resides, this is always 2 for FAT32 and it maps to the first sector of the data area (see below)
                extended_bpb._fat32._information_sector = 1;
                extended_bpb._fat32._phys_drive_number = 0x80;	    // standard hardisk ID
                extended_bpb._fat32._ext_boot_signature = 0x29;     // indicates that volume ID, volume label, and file system type, are present. NOTE: volume label is ignored 
                extended_bpb._fat32._volume_id = utils::uuid::rand_int();
                //NOTE: this must match what is set in the root directory below
                memset(extended_bpb._fat32._volume_label, 0x20, sizeof extended_bpb._fat32._volume_label);
                memcpy(extended_bpb._fat32._volume_label, volumeLabel, std::min(sizeof extended_bpb._fat32._volume_label, strlen(volumeLabel)));
                memcpy(extended_bpb._fat32._file_system_type, kFat32FsType, sizeof kFat32FsType);

                // from MS' white paper on FAT
                for (const auto& entry : kDiskTableFat32)
                {
                    if (total_sectors <= entry._sector_limit)
                    {
                        boot_sector._bpb._sectors_per_cluster = entry._sectors_per_cluster;
                        break;
                    }
                }

                extended_bpb_ptr = reinterpret_cast<char*>(&extended_bpb._fat32);
                extended_bpb_size = sizeof extended_bpb._fat32;

                if (_verbose)
                {
                    std::cout << "\tfilesystem is FAT32\n";
                }
            }
            //TODO: support FAT12 for small disks

            const auto bytes_per_cluster = boot_sector._bpb._sectors_per_cluster * kSectorSizeBytes;

            const auto root_dir_sector_count = ((boot_sector._bpb._root_entry_count * 32) + (kSectorSizeBytes - 1)) / kSectorSizeBytes;

            // this magic piece of calculation is taken from from MS' white paper where it states;
            // "Do not spend too much time trying to figure out why this math works."

            auto sectors_per_fat = 0u;
            const auto tmp1 = static_cast<unsigned long long>(total_sectors - (boot_sector._bpb._reserved_sectors + root_dir_sector_count));
            auto tmp2 = (256 * boot_sector._bpb._sectors_per_cluster) + boot_sector._bpb._num_fats;
            if (type == fat_type::kFat32)
            {
                tmp2 /= 2;
            }
            const auto fatsz = (tmp1 + (tmp2 - 1)) / tmp2;
            if (type == fat_type::kFat32)
            {
                boot_sector._bpb._sectors_per_fat16 = 0;
                extended_bpb._fat32._sectors_per_fat = fatsz;
                sectors_per_fat = fatsz;
            }
            else
            {
                boot_sector._bpb._sectors_per_fat16 = uint16_t(fatsz & 0xffff);
                sectors_per_fat = fatsz;
            }

            // see MS fat documentation for this size check, we don't support FAT12
            const auto num_clusters = total_sectors / boot_sector._bpb._sectors_per_cluster;
            memcpy(boot_sector._oem_name, kFatOemName, sizeof kFatOemName);

            auto* sector = writer->blank_sector();
            memcpy(sector, &boot_sector, sizeof boot_sector);
            memcpy(sector + sizeof boot_sector, extended_bpb_ptr, extended_bpb_size);
            reinterpret_cast<uint16_t*>(sector + 510)[0] = kMBRSignature;
            if (!writer->write_at(0, 1))
            {
                return System::Code::INTERNAL;
            }

            if (type == fat_type::kFat32)
            {
                // =======================================================================================
                // FSInfo (fat32 only)
                //
                sector = writer->blank_sector();
                // FSInfo
                auto* fsinfo = reinterpret_cast<fat32_fsinfo*>(sector);
                fsinfo->_lead_sig = kFsInfoLeadSig;
                fsinfo->_struc_sig = kFsInfoStrucSig;
                fsinfo->_tail_sig = kFsInfoTailSig;

                writer->write_at(extended_bpb._fat32._information_sector, 1);
            }

            // =======================================================================================
            //

            const auto first_data_lba = boot_sector._bpb._reserved_sectors + (boot_sector._bpb._num_fats * sectors_per_fat) + root_dir_sector_count;
            auto root_dir_start_lba = 0u;

            // 3 is the first available data cluster and is the one that will hold the BOOT directory
            size_t next_cluster = 0u;

            // 2 reserved + 2 directories
            //ZZZ:
            auto bootx64_start_cluster = 0u;
            const auto bootx64_num_clusters = 0u;

            if (type == fat_type::kFat16)
            {
                write_fat16(writer, boot_sector, fs);
                // for FAT16 the root directory is stored before the data area in a fixed size area (as in; it can't grow after it has been created)
                root_dir_start_lba = boot_sector._bpb._reserved_sectors + (boot_sector._bpb._num_fats * boot_sector._bpb._sectors_per_fat16);
            }
            else
            {

                sector = writer->blank_sector();

                auto* fat32 = reinterpret_cast<uint32_t*>(sector);
                const auto max_clusters_per_sector = kSectorSizeBytes / sizeof(uint32_t);

                fat32[0] = 0x0fffff00 | boot_sector._bpb._media_descriptor;
                fat32[1] = kFat32EOC;

                // root directory, EFI and EFI/BOOT directories reside in the first three clusters and only use one cluster each
                fat32[2] = kFat32EOC;
                fat32[3] = kFat32EOC;
                fat32[4] = kFat32EOC;

                // first available cluster after the root directory is 3
                next_cluster = 3;
                bootx64_start_cluster = 5;

                auto cluster_counter = bootx64_start_cluster;
                auto fat_sector = boot_sector._bpb._reserved_sectors;

                // each FAT entry points to the *next* cluster in the chain, hence the staggered start.
                for (auto n = 1u; n < bootx64_num_clusters; ++n)
                {
                    //NOTE: FAT32 entries are only 28 bits, the upper 4 bits are reserved and since we are formatting the volume we get to set them to 0
                    fat32[cluster_counter++] = (n + bootx64_start_cluster) & 0x0fffffff;

                    if (cluster_counter == max_clusters_per_sector)
                    {
                        if (n == (bootx64_num_clusters - 1))
                        {
                            fat32[cluster_counter] = kFat32EOC;
                        }

                        // next FAT sector
                        writer->write_at(fat_sector++, 1);
                        fat32 = reinterpret_cast<uint32_t*>(writer->blank_sector());
                        cluster_counter = 0;
                    }
                }

                if (cluster_counter)
                {
                    fat32[cluster_counter] = kFat32EOC;
                    writer->write_at(fat_sector, 1);
                }

                // the root directory of a FAT32 volume is a normal file chain and can grow as large as it needs to be.
                root_dir_start_lba = first_data_lba + ((extended_bpb._fat32._root_cluster - 2) * boot_sector._bpb._sectors_per_cluster);
            }

            // =======================================================================================
            // directories and files
            //
            // the root directory comes first and resides inside the reserved area for FAT16 and in the first data cluster for FAT32
            // subsequent directories (and files) are created linearly from free clusters.

            write_fs_contents_to_disk(writer, root_dir_start_lba,
                first_data_lba, boot_sector._bpb._sectors_per_cluster,
                volumeLabel, fs);

            writer->reset();
            return true;
        }

    } // namespace fat

    // All things EFI GPT 
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
        System::status_or_t<partition_info_t> create_efi_boot_image(disk_sector_writer_t* writer)
        {
            // ===============================================
            // protective MBR

            // skip past legacy boot loader code area (446 bytes)
            auto* sector = writer->blank_sector();
            auto* mbr_prec = reinterpret_cast<mbr_partition_record*>(sector + 446);
            mbr_prec->_boot_indicator = 0;
            mbr_prec->_starting_chs[1] = 0x02; // 0x000200/512 bytes in
            mbr_prec->_os_type = kGptProtectivePartitionOSType;
            mbr_prec->_starting_lba = 1;
            if (writer->size() > 0xffffffff)
            {
                mbr_prec->_size_in_lba = 0xffffffff;
            }
            else
            {
                mbr_prec->_size_in_lba = writer->last_lba();
            }
            // we just ignore chs altogether and set this to "infinite"
            memset(mbr_prec->_ending_chs, 0xff, sizeof(mbr_prec->_ending_chs));
            memcpy(sector + 510, &kMBRSignature, sizeof(kMBRSignature));

            writer->write_at(0, 1);

            if (_verbose)
            {
                std::cout << "\t...protective mbr";
            }

            // ===============================================
            // GPT and EFI PART
            // including backup GPT and partition info

            sector = writer->blank_sector(2);

            auto* gpt_header_ptr = reinterpret_cast<gpt_header*>(sector);
            gpt_header_ptr->_signature = kEfiPartSignature;
            gpt_header_ptr->_revision = kEfiRevision;
            gpt_header_ptr->_header_size = sizeof(gpt_header);
            gpt_header_ptr->_header_crc32 = 0;	//<NOTE: we calculate this once we have the completed header filled in
            gpt_header_ptr->_my_lba = 1;
            // backup GPT is stored in the last LBA
            gpt_header_ptr->_alternate_lba = writer->last_lba();

            // From Uefi 2.6 standard ch 5:
            // 
            // "If the block size is 512, the First Usable LBA must be greater than or equal to 34 (allowing 1
            //  block for the Protective MBR, 1 block for the Partition Table Header, and 32 blocks for the GPT
            //  Partition Entry Array)"
            //
            //  NOTE: the minimum size of the GPT entry array which is 16K (16K/512 = 32 + LBA0+LBA1 = 34)
            //  
            gpt_header_ptr->_first_usable_lba = 34;
            // minus backup GPT + backup array
            gpt_header_ptr->_last_usable_lba = writer->last_lba() - 2;

            // there is only one
            gpt_header_ptr->_partition_entry_count = 1;
            // as per standard
            gpt_header_ptr->_partition_entry_size = 128;
            // first GPT entry follows this, subsequent 33 are zero
            gpt_header_ptr->_partition_entry_lba = 2;

            utils::uuid::generate(gpt_header_ptr->_disk_guid);

            // partition array starts at LBA2
            auto* gpt_partition = reinterpret_cast<gpt_partition_header*>(sector + kSectorSizeBytes);

            memcpy(gpt_partition->_type_guid, kEfiSystemPartitionUuid, sizeof kEfiSystemPartitionUuid);
            utils::uuid::generate(gpt_partition->_part_guid);
            gpt_partition->_start_lba = gpt_header_ptr->_first_usable_lba;
            gpt_partition->_end_lba = gpt_header_ptr->_last_usable_lba;
            // bit 0: required partition, can't be deleted
            gpt_partition->_attributes = 1;

            memset(gpt_partition->_name, 0x20, sizeof gpt_partition->_name);
            memcpy(gpt_partition->_name, kEfiBootPartName, sizeof kEfiBootPartName);

            // we're only considering ONE header here
            gpt_header_ptr->_partition_array_crc32 = utils::rc_crc32(0, reinterpret_cast<const char*>(gpt_partition), sizeof gpt_partition_header);
            gpt_header_ptr->_header_crc32 = utils::rc_crc32(0, reinterpret_cast<const char*>(gpt_header_ptr), sizeof gpt_header);

            // this writes both header and array sectors
            writer->write_at(1, 2);

            if (_verbose)
            {
                std::cout << "...GPT + partition array";
            }

            // link back
            std::swap(gpt_header_ptr->_my_lba, gpt_header_ptr->_alternate_lba);
            gpt_header_ptr->_partition_entry_lba = writer->last_lba() - 1;
            // need to recalculate this since we've changed some entries
            gpt_header_ptr->_header_crc32 = 0;
            gpt_header_ptr->_header_crc32 = utils::rc_crc32(0, reinterpret_cast<const char*>(gpt_header_ptr), sizeof gpt_header);

            // backup array
            writer->write_at_ex(gpt_header_ptr->_my_lba - 1, 1, 1);
            // backup header
            writer->write_at_ex(gpt_header_ptr->_my_lba, 0, 1);

            if (_verbose)
            {
                std::cout << "...backup GPT and partition array\n";
            }

            // ===============================================

            partition_info_t info;
            info._first_usable_lba = gpt_header_ptr->_first_usable_lba;
            info._last_usable_lba = gpt_header_ptr->_last_usable_lba;
            writer->set_pos(gpt_header_ptr->_first_usable_lba);

            return info;
        }
    }
}

#define CHECK_REPORT_ABORT_ERROR(result)\
if (!result)\
{\
    std::cerr << "* error: " << result.error_code() << std::endl;\
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
        if (_stricmp(dir_fname.c_str(), "BOOTX64 EFI") != 0)
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

    auto writer_result = disktools::disk_sector_writer_t::create_writer(output_option.as<const std::string&>(), fs.size());
    CHECK_REPORT_ABORT_ERROR(writer_result);
    auto* writer = writer_result.value();
    if (!writer->using_existing())
    {
        create_blank_image(writer);
    }

    auto part_result = disktools::gpt::create_efi_boot_image(writer);
    CHECK_REPORT_ABORT_ERROR(part_result);

    const auto part_info = part_result.value();
    auto fat_result = disktools::fat::format_efi_boot_partition(writer, part_info.num_sectors(), label_option.as<const std::string&>().c_str(), fs);
    CHECK_REPORT_ABORT_ERROR(fat_result);

    writer->flush();
    delete writer;
    delete[] buffer;

    std::cout << "\tboot image created" << std::endl;
}
