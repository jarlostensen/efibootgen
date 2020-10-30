
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <random>
#include <functional>
#include <filesystem>
#include <stack>
namespace fs = std::filesystem;

#pragma warning(push, 3)
#include "cxxopts-2.2.0/include/cxxopts.hpp"
#pragma warning(pop)

#include "status.h"
#include "fat.h"
#include "gpt.h"

#define CHECK_REPORT_ABORT_ERROR(result)\
    if (!result)\
    {\
        std::cerr << "* error: " << create_result.ErrorCode() << std::endl;\
        return -1;\
    }


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

    auto _verbose = false;

    // helper to make it a bit more intuitive to use and write sectors to a file
    struct disk_sector_writer_t
    {
        disk_sector_writer_t(std::ofstream&& ofs, size_t total_sectors)
            : _ofs{ std::move(ofs) }
            , _total_sectors{ total_sectors }
        {
            _start_pos = _ofs.tellp();
        }

        disk_sector_writer_t(disk_sector_writer_t&& rhs)
            : _ofs{ std::move(rhs._ofs) }
            , _sector{ rhs._sector }
            , _total_sectors{ rhs._total_sectors }
            , _sectors_in_buffer{ rhs._sectors_in_buffer }
            , _start_pos{ rhs._start_pos }
        {
            rhs._sector = nullptr;
        }

        ~disk_sector_writer_t()
        {
            delete[] _sector;
            _ofs.close();
        }

        static System::StatusOr<disk_sector_writer_t*> create_writer(const std::string&, size_t);

        bool set_pos(size_t lba)
        {
            if (good())
            {
                _ofs.seekp(_start_pos + std::ofstream::pos_type(lba * kSectorSizeBytes), std::ios::beg);
                _start_pos = _ofs.tellp();
                return good();
            }
            return false;
        }

        size_t size() const
        {
            return _total_sectors * kSectorSizeBytes;
        }

        bool good() const
        {
            return _ofs.is_open() && _ofs.good();
        }

        void reset()
        {
            if (good())
                _ofs.seekp(_start_pos, std::ios::beg);
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
            _ofs.seekp(_start_pos + std::ofstream::pos_type(lba * kSectorSizeBytes), std::ios::beg);
            if (!_ofs.good() || sector_count > _sectors_in_buffer)
            {
                return false;
            }
            _ofs.write(_sector + src_sector_offset * kSectorSizeBytes, (sector_count * kSectorSizeBytes));
            return _ofs.good();
        }

        bool        write_at(size_t lba, size_t sector_count)
        {
            _ofs.seekp(_start_pos + std::ofstream::pos_type(lba * kSectorSizeBytes), std::ios::beg);
            if (!_ofs.good())
            {
                return false;
            }
            _ofs.write(_sector, (sector_count * kSectorSizeBytes));
            return _ofs.good();
        }

        size_t  last_lba() const
        {
            return _total_sectors - 1;
        }

        char* _sector = nullptr;
        size_t                  _sectors_in_buffer = 1;
        size_t                  _total_sectors = 0;
        std::ofstream           _ofs;
        std::ofstream::pos_type _start_pos;
    };


    // like "dd"; create a blank image of writer->_total_sectors sectors
    bool create_blank_image(disk_sector_writer_t* writer)
    {
        writer->reset();
        writer->blank_sector();
        auto sector_count = writer->_total_sectors;
        while (writer->good() && sector_count--)
        {
            writer->_ofs.write(writer->_sector, kSectorSizeBytes);
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

        using dir_entries_t = std::unordered_map<std::string, dir_entry_t>;

        struct dir_t
        {
            dir_t() = default;
            ~dir_t() = default;

            std::string     _name;
            dir_entries_t   _entries;
            size_t          _start_cluster;
        };

        fs_t()
        {
            _root._name = "\\";
        }

        size_t size() const
        {
            return _size;
        }

        System::StatusOr<bool> add_dir(dir_t* parent, const std::string& sysRootPath) {

            //NOTE: the std recursive_directory_iterator does not make any guarantees about the
            //      order of traversal so instead of using it we're doing our own version here.

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
                            parent = result.Value();
                            i = fs::directory_iterator(rec_path);
                        }
                        else
                        {
                            return result.ErrorCode();
                        }
                    }
                    else
                    {
                        const auto path = i->path().string();
                        std::ifstream ifs{ path, std::ios::binary };
                        if (ifs.is_open())
                        {
                            ifs.seekg(0, std::ios::end);
                            const auto size = ifs.tellg();
                            ifs.seekg(0, std::ios::end);

                            auto* buffer = new char[size];
                            ifs.read(buffer, size);
                            ifs.close();

                            /* _ =*/ create_file(parent, i->path().filename().string(), buffer, size);

                            // next item
                            ++i;
                        }
                        else
                        {
                            //TODO:
                            break;
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
        System::StatusOr<bool> create_from_source(const std::string& systemRootPath)
        {
            auto result = create_directory(&_root, systemRootPath);
            if (!result)
            {
                return result.ErrorCode();
            }
            return add_dir(result.Value(), systemRootPath);
        }

        [[nodiscard]]
        System::StatusOr<dir_t*> create_directory(dir_t* parent, const std::string& name)
        {
            assert(parent->_entries.find(name) == parent->_entries.end());

            dir_entry_t dir_entry{ true };
            dir_entry._content._dir = new dir_t;
            dir_entry._content._dir->_name = name;
            //NOTE: a directory is limited to 512 bytes = 16 entries here
            _size += kSectorSizeBytes;

            parent->_entries.emplace(name, dir_entry);
            return dir_entry._content._dir;
        }

        [[nodiscard]]
        System::StatusOr<dir_t*> create_directory(const std::string& name)
        {
            return create_directory(&_root, name);
        }

        [[nodiscard]]
        System::StatusOr<file_t*> create_file(dir_t* parent, const std::string& name, const void* data, size_t size)
        {
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

        void dumpDir(const dir_t* dir = nullptr, int depth = 1) const
        {
            if (!dir)
                dir = &_root;

            for (auto const& [key, val] : dir->_entries)
            {
                std::cout << std::setw(depth * 4) << key << (val._is_dir ? "\\" : "") << std::endl;
                if (val._is_dir)
                {
                    dumpDir(val._content._dir, depth + 1);
                }
            }
        }

        dir_t           _root{};
        size_t          _size = 0;
    };

    System::StatusOr<disk_sector_writer_t*> disk_sector_writer_t::create_writer(const std::string& oName, size_t content_size)
    {
        // round size up to nearest 128 Megs. This pushes us out of the "floppy disk" domain
        size_t size = (content_size + (0x8000000 - 1)) & ~(0x8000000 - 1);

        std::ofstream file{ oName, std::ios::binary | std::ios::trunc };
        if (!file.is_open())
        {
            return System::Code::NOT_FOUND;
        }

        // round up to nearest 512 byte block
        size = (size + (kSectorSizeBytes - 1)) & ~(kSectorSizeBytes - 1);
        const auto blocks = size / kSectorSizeBytes;

        return new disk_sector_writer_t{ std::move(file), blocks };
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


        void write_file(disk_sector_writer_t* writer, cluster_to_lba_func_t& cluster_to_lba, const fs_t::dir_entry_t& entry)
        {
            // the contents of a file are laid out in a linear chain starting at 
            // the start cluster, here we just copy it in sector by sector
            auto file_sector = cluster_to_lba(entry._content._file->_start_cluster);
            auto bytes_left = entry._content._file->_size;
            const auto* bytes = static_cast<const char*>(entry._content._file->_data);
            do
            {
                memcpy(writer->blank_sector(), bytes, kSectorSizeBytes);
                writer->write_at(file_sector++, 1);
                bytes += kSectorSizeBytes;
                bytes_left -= kSectorSizeBytes;
            } while (bytes_left >= kSectorSizeBytes);

            if (bytes_left)
            {
                memcpy(writer->blank_sector(), bytes, bytes_left);
                writer->write_at(file_sector, 1);
            }
        }


        void write_dir(disk_sector_writer_t* writer, cluster_to_lba_func_t& cluster_to_lba, const fs_t::dir_entry_t& entry_)
        {
            auto* dir_entry = reinterpret_cast<fat_dir_entry_t*>(writer->blank_sector());
            const auto entries = entry_._content._dir->_entries;
            for (auto& [name, entry] : entries)
            {
                dir_entry->set_name(name.c_str());
                if (entry._is_dir)
                {
                    dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
                    dir_entry->_first_cluster_lo = entry._content._dir->_start_cluster;
                }
                else
                {
                    dir_entry->_size = entry._content._file->_size;
                    dir_entry->_first_cluster_lo = entry._content._file->_start_cluster;
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

        System::StatusOr<bool> write_fs_contents_to_disk(disk_sector_writer_t* writer, size_t root_dir_start_lba,
            cluster_to_lba_func_t&& cluster_to_lba, const char* volumeLabel, const fs_t& fs)
        {
            // the first entry is always the volume label entry (which must match the volume label set in the BPB)
            auto* dir_entry = reinterpret_cast<fat_dir_entry_t*>(writer->blank_sector());
            dir_entry->set_name(volumeLabel);
            dir_entry->_attrib = uint8_t(fat_file_attribute::kVolumeId);
            ++dir_entry;

            // the root directory (for either FAT16 or FAT32) is special and has no '.' or '..' entries
            //TODO:ZZZ: check for overrun
            for (auto& [name, entry] : fs._root._entries)
            {
                dir_entry->set_name(name.c_str());
                if (entry._is_dir)
                {
                    dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
                    dir_entry->_first_cluster_lo = entry._content._dir->_start_cluster;
                }
                else
                {
                    dir_entry->_size = entry._content._file->_size;
                    dir_entry->_first_cluster_lo = entry._content._file->_start_cluster;
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

        System::StatusOr<bool> format_efi_boot_partition_2(disk_sector_writer_t* writer, size_t total_sectors, const char* volumeLabel, const fs_t& fs)
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
                [&boot_sector, first_data_lba](size_t cluster) -> size_t
            {
                return first_data_lba + ((cluster - 2) * boot_sector._bpb._sectors_per_cluster);
            },
                volumeLabel, fs
                );

            writer->reset();
            return true;
        }

        bool format_efi_boot_partition(disk_sector_writer_t* writer, size_t total_sectors, const char* volumeLabel, const void* bootx64_efi, size_t bootx64_size)
        {
            if (!writer->good() || !total_sectors)
                return false;

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
                return false;
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
            // first FAT
            //

            // NOTE: we're hard coding a particular layout here, for an EFI boot disk:
            //
            // /EFI
            //      /BOOT
            //            BOOTX64.EFI
            //
            // The root (for FAT32), EFI, and BOOT directories all occupy one cluster each that we lay out 
            // linearly starting at 2. The payload (bootx64.efi) is laid out following the directories, also linearly.
            //

            sector = writer->blank_sector();

            const auto first_data_lba = boot_sector._bpb._reserved_sectors + (boot_sector._bpb._num_fats * sectors_per_fat) + root_dir_sector_count;
            auto root_dir_start_lba = 0u;

            // 3 is the first available data cluster and is the one that will hold the BOOT directory
            auto next_cluster = 0u;

            // 2 reserved + 2 directories 
            auto bootx64_start_cluster = 0u;
            const auto bootx64_num_clusters = (bootx64_size / bytes_per_cluster) + ((bootx64_size % bytes_per_cluster) != 0 ? 1 : 0);

            if (type == fat_type::kFat16)
            {
                auto* fat16 = reinterpret_cast<uint16_t*>(sector);
                const auto max_clusters_per_sector = kSectorSizeBytes / sizeof(uint16_t);

                fat16[0] = 0xff00 | boot_sector._bpb._media_descriptor;
                fat16[1] = kFat16EOC;

                // EFI and EFI/BOOT directories reside in the first three clusters and only use one cluster each
                fat16[2] = kFat16EOC;
                fat16[3] = kFat16EOC;

                // first available cluster is 2
                next_cluster = 2;
                // first free after 2 reserved entries + 2 clusters for EFI + BOOT 
                bootx64_start_cluster = 4;

                auto cluster_counter = bootx64_start_cluster;
                auto fat_sector = boot_sector._bpb._reserved_sectors;

                // each FAT entry points to the *next* cluster in the chain, hence the staggered start.
                for (auto n = 1u; n < bootx64_num_clusters; ++n)
                {
                    fat16[cluster_counter++] = n + bootx64_start_cluster;

                    if (cluster_counter == max_clusters_per_sector)
                    {
                        if (n == (bootx64_num_clusters - 1))
                        {
                            fat16[cluster_counter] = kFat16EOC;
                        }

                        // next FAT sector
                        writer->write_at(fat_sector++, 1);
                        fat16 = reinterpret_cast<uint16_t*>(writer->blank_sector());
                        cluster_counter = 0;
                    }
                }

                if (cluster_counter)
                {
                    fat16[cluster_counter] = kFat16EOC;
                    writer->write_at(fat_sector, 1);
                }

                // for FAT16 the root directory is stored before the data area in a fixed size area (as in; it can't grow after it has been created)
                root_dir_start_lba = boot_sector._bpb._reserved_sectors + (boot_sector._bpb._num_fats * boot_sector._bpb._sectors_per_fat16);
            }
            else
            {
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

            // the first entry is always the volume label entry (which must match the volume label set in the BPB)
            auto* dir_entry = reinterpret_cast<fat_dir_entry_t*>(writer->blank_sector());
            dir_entry->set_name(volumeLabel);
            dir_entry->_attrib = uint8_t(fat_file_attribute::kVolumeId);
            ++dir_entry;

            // EFI and EFI/BOOT directories

            // EFI directory
            dir_entry->set_name("EFI");
            dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
            dir_entry->_first_cluster_lo = next_cluster++;
            const auto efi_dir_cluster = dir_entry->_first_cluster_lo;

            // write root dir sector
            writer->write_at(root_dir_start_lba, 1);

            const auto cluster_2_lba = [&boot_sector, first_data_lba](size_t cluster)
            {
                return first_data_lba + ((cluster - 2) * boot_sector._bpb._sectors_per_cluster);
            };

            // EFI directory contents:
            //  .
            //  ..
            //  BOOT
            dir_entry = reinterpret_cast<fat_dir_entry_t*>(writer->blank_sector());

            dir_entry->set_name(".");
            dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
            dir_entry->_first_cluster_lo = efi_dir_cluster;
            dir_entry++;
            dir_entry->set_name("..");
            dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
            dir_entry->_first_cluster_lo = 0;
            dir_entry++;
            dir_entry->set_name("BOOT");
            dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
            dir_entry->_first_cluster_lo = next_cluster++;
            const auto boot_dir_cluster = dir_entry->_first_cluster_lo;

            // write the BOOT sub-directory entry to the efi_dir_cluster
            const auto efi_dir_lba = cluster_2_lba(efi_dir_cluster);
            writer->write_at(efi_dir_lba, 1);

            // BOOT directory contents
            // .
            // ..
            // BOOTx64.EFI
            //
            dir_entry = reinterpret_cast<fat_dir_entry_t*>(writer->blank_sector());

            dir_entry->set_name(".");
            dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
            dir_entry->_first_cluster_lo = boot_dir_cluster;
            dir_entry++;
            dir_entry->set_name("..");
            dir_entry->_attrib = uint8_t(fat_file_attribute::kDirectory);
            dir_entry->_first_cluster_lo = efi_dir_cluster;
            dir_entry++;
            dir_entry->set_name("BOOTX64 EFI");
            dir_entry->_size = bootx64_size;
            dir_entry->_first_cluster_lo = bootx64_start_cluster;

            const auto boot_dir_lba = cluster_2_lba(boot_dir_cluster);
            writer->write_at(boot_dir_lba, 1);

            // the contents of BOOTX64.EFI itself are laid out in a linear chain starting at 
            // bootx64_start_cluster, here we just copy it in sector by sector
            auto bootx64_sector = cluster_2_lba(bootx64_start_cluster);
            auto bytes_left = bootx64_size;
            const auto* bytes = reinterpret_cast<const char*>(bootx64_efi);
            do
            {
                memcpy(writer->blank_sector(), bytes, kSectorSizeBytes);
                writer->write_at(bootx64_sector++, 1);
                bytes += kSectorSizeBytes;
                bytes_left -= kSectorSizeBytes;
            } while (bytes_left >= kSectorSizeBytes);

            if (bytes_left)
            {
                memcpy(writer->blank_sector(), bytes, bytes_left);
                writer->write_at(bootx64_sector, 1);
            }

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
        System::StatusOr<partition_info_t> create_efi_boot_image(disk_sector_writer_t* writer)
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

        /*
        System::StatusOr<bool> create_efi_boot_image(const std::string& bootIname, const std::string& oname)
        {
            std::ifstream ifile{ bootIname, std::ios::binary };
            if (!ifile.is_open())
            {
                if (_verbose)
                {
                    std::cerr << bootIname << " not found\n";
                }
                return System::Code::NOT_FOUND;
            }

            ifile.seekg(0, std::ios::end);
            const auto payload_size = ifile.tellg();
            ifile.seekg(0, std::ios::beg);

            auto* payload = new char[payload_size];
            ifile.read(payload, payload_size);

            ifile.close();

            // round size up to nearest 128 Megs. This pushes us out of the "floppy disk" domain
            size_t size = (size_t(payload_size) + (0x8000000 - 1)) & ~(0x8000000 - 1);

            std::ofstream file{ oname, std::ios::binary | std::ios::trunc };
            if (!file.is_open())
            {
                if (_verbose)
                {
                    std::cerr << oname << " not found\n";
                }
                return System::Code::NOT_FOUND;
            }

            // we'll build the image sector by sector but we use two to make life easier when we generate the GPT

            // round up to nearest 512 byte block
            size = (size + (kSectorSizeBytes - 1)) & ~(kSectorSizeBytes - 1);
            auto blocks = size / kSectorSizeBytes;
            auto last_lba = blocks - 1;

            disk_sector_writer_t writer{ file, blocks };

            // ===============================================
            // create empty file
            create_blank_image(writer);

            if (_verbose)
            {
                std::cout << "\timage is " << writer->_total_sectors << " sectors, " << size << " bytes\n";
            }

            // ===============================================
            // protective MBR

            // skip past legacy boot loader code area (446 bytes)
            auto* sector = writer->blank_sector();
            auto* mbr_prec = reinterpret_cast<mbr_partition_record*>(sector + 446);
            mbr_prec->_boot_indicator = 0;
            mbr_prec->_starting_chs[1] = 0x02; // 0x000200/512 bytes in
            mbr_prec->_os_type = kGptProtectivePartitionOSType;
            mbr_prec->_starting_lba = 1;
            if (size > 0xffffffff)
            {
                mbr_prec->_size_in_lba = 0xffffffff;
            }
            else
            {
                mbr_prec->_size_in_lba = last_lba;
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
            gpt_header_ptr->_alternate_lba = last_lba;

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
            gpt_header_ptr->_last_usable_lba = last_lba - 2;

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
            gpt_header_ptr->_partition_entry_lba = last_lba - 1;
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
            // the data partition will be formatted as FAT
            const auto partition_size = size_t(gpt_header_ptr->_last_usable_lba - gpt_header_ptr->_first_usable_lba);
            writer->set_pos(gpt_header_ptr->_first_usable_lba);
            const auto result = fat::format_efi_boot_partition_2(writer, partition_size, "efi_boot");

            file.close();
            return result;
        }
        */
    }
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

    cxxopts::Options options("efibootgen");

    options.add_options()
        ("d, directory", "source directory to copy to disk image", cxxopts::value<std::string>())
        ("b, bootimage", "source kernel binary, must be BOOTX64.EFI. This creates a standard EFI/BOOOT/BOOTX64.EFI layout.", cxxopts::value<std::string>())
        ("o,output", "output disk image file", cxxopts::value<std::string>())
        ("v,verbose", "output more information about the build process", cxxopts::value<bool>()->default_value("false"))
        ("h,help", "usage")
        ;

    const auto result = options.parse(argc, argv);

    std::cout << "------------------------------------\n";
    std::cout << "efibootgen EFI boot disk creator\n";
    std::cout << "by jarl.ostensen\n\n";

    if (result.count("help")
        ||
        (!result.count("bootimage") && !result.count("output") && !result.count("directory")))
    {
        std::cerr << options.help() << std::endl;
        return -1;
    }

    if (result.count("verbose"))
    {
        disktools::_verbose = result["verbose"].as<bool>();
    }

    const auto output_file = result["output"].as<std::string>();

    if (result.count("directory"))
    {
        disktools::fs_t fs;
        auto create_result = fs.create_from_source(result["directory"].as<std::string>());        
        CHECK_REPORT_ABORT_ERROR(create_result);

        auto writer_result = disktools::disk_sector_writer_t::create_writer(output_file, fs.size());
        CHECK_REPORT_ABORT_ERROR(writer_result);
        auto* writer = writer_result.Value();
        create_blank_image(writer);

        auto part_result = disktools::gpt::create_efi_boot_image(writer);
        CHECK_REPORT_ABORT_ERROR(part_result);
        const auto part_info = part_result.Value();
        auto fat_result = disktools::fat::format_efi_boot_partition_2(writer, part_info.num_sectors(), "efi boot", fs);
        CHECK_REPORT_ABORT_ERROR(fat_result);

        delete writer;
    }

    //else
    //{
    //    const auto boot_image_file = result["bootimage"].as<std::string>();

    //    if (!disktools::gpt::create_efi_boot_image(boot_image_file, output_file))
    //    {
    //        //TODO: print error
    //        std::cerr << "\tcreate failed\n";
    //        return -1;
    //    }
    //}

    std::cout << "\tboot image created" << std::endl;
}
