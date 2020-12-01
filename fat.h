#pragma once

namespace disktools
{
    namespace fat
    {
        // *All* the information needed to understand the FAT format can be found here: http://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/fatgen103.doc

        static constexpr uint8_t kFat32FsType[8] = { 'F','A','T','3','2',' ',' ',' ' };
        static constexpr uint8_t kFat16FsType[8] = { 'F','A','T','1','6',' ',' ',' ' };

#pragma pack(push,1)
        struct fat_bpb
        {
            uint16_t		_bytes_per_sector;		// in powers of 2
            uint8_t			_sectors_per_cluster;
            uint16_t		_reserved_sectors;
            uint8_t			_num_fats;
            uint16_t		_root_entry_count;
            uint16_t		_total_sectors16;
            uint8_t			_media_descriptor;		// 0xf8 : fixed disk partition
            uint16_t		_sectors_per_fat16;		// 0 for fat32
            uint16_t		_sectors_per_track;
            uint16_t		_num_heads;
            uint32_t		_num_hidden_sectors;
            uint32_t		_total_sectors32;
        };

        struct fat16_extended_bpb
        {
            uint8_t			_drive_num;
            uint8_t			_reserved1 = 0;
            uint8_t			_boot_sig;
            uint32_t		_volume_serial;
            uint8_t			_volume_label[11];
            uint8_t			_file_sys_type[8];	// "FAT16   "	AND see comments in MS FAT document; this field is *not* relied on to determine the fs type	
        };

        struct fat32_extended_bpb
        {
            uint32_t		_sectors_per_fat;
            uint16_t		_flags;
            uint16_t		_version;	// 0.0
            uint32_t		_root_cluster;
            uint16_t		_information_sector;
            uint16_t		_boot_copy_sector;
            uint8_t			_reserved_00[12];
            uint8_t			_phys_drive_number;
            uint8_t			_unused;		// not really used but can be non 0
            uint8_t			_ext_boot_signature;
            uint32_t		_volume_id;
            uint8_t			_volume_label[11];
            uint8_t			_file_system_type[8];	// "FAT32   "
        };

        struct fat_boot_sector_t
        {
            uint8_t					_jmp[3];
            uint8_t					_oem_name[8];
            fat_bpb				_bpb;

            // what follows is either a fat16_extended_bpb or a fat32_extended_bpb, depending on the number of clusters on the disk	
        };

        struct fat32_partition_desc
        {
            uint8_t			_boot_flag;
            uint8_t			_chs_begin[3];
            uint8_t			type;
            uint8_t			_chs_end[3];
            uint32_t		_lba_begin;
            uint32_t		_sectors;
        };

        struct fat_dir_entry_t
        {
            uint8_t			_short_name[11];	// 8.3 format
            uint8_t			_attrib;
            uint8_t         _reserved00;        // always 0 (DIR_NtRes)
            uint8_t         _crt_time_tenth;
            uint16_t        _crt_time;
            uint16_t        _crt_date;
            uint16_t        _last_access_date;
            uint16_t		_first_cluster_hi;
            uint16_t        _wrt_time;
            uint16_t        _wrt_date;
            uint16_t		_first_cluster_lo;
            uint32_t		_size;

            void set_name(const char* name)
            {
                const auto len = std::min<size_t>(sizeof _short_name, strlen(name));
                memset(_short_name, ' ', sizeof _short_name);
                memcpy(_short_name, name, len);
            }

            void get_name(char* buffer, size_t buff_len) const
            {
                assert(buff_len >= 12);
                memcpy(buffer, _short_name, 11);
                buffer[11] = 0;                
            }
        };

        struct fat32_fsinfo
        {
            uint32_t		_lead_sig;
            uint8_t			_reserved1[480];
            uint32_t		_struc_sig;
            uint32_t		_free_count;
            uint32_t		_next_free;
            uint8_t			_reserved2[12];
            uint32_t		_tail_sig;
        };

#pragma pack(pop)

        enum class fat_file_attribute : uint8_t
        {
            kReadOnly = 0x01,
            kHidden = 0x02,
            kSystem = 0x04,
            kVolumeId = 0x08,
            kDirectory = 0x10,
            kArchive = 0x20,
            kLongName = kReadOnly | kHidden | kSystem | kVolumeId
        };

        enum class fat_type
        {
            kFat16,
            kFat32,
        };

        static constexpr uint16_t	kReservedSectorCount = 32;
        static constexpr uint8_t	kNumFats = 2;
        static constexpr uint32_t   kFsInfoLeadSig = 0x41615252;
        static constexpr uint32_t	kFsInfoStrucSig = 0x61417272;
        static constexpr uint32_t   kFsInfoTailSig = 0xaa550000;
        static constexpr uint32_t   kFat32EOC = 0x0ffffff8;
        static constexpr uint16_t   kFat16EOC = 0xfff8;
        static constexpr uint8_t    kShortJmp = 0xeb;
        static constexpr uint8_t    kLongJmp = 0xe9;

        struct disksize_to_sectors_per_cluster
        {
            size_t		_sector_limit;
            uint8_t		_sectors_per_cluster;
        };

        // from Microsoft's FAT format technical design document
        static constexpr disksize_to_sectors_per_cluster kDiskTableFat16[] =
        {
            {    262144,   4},   /* disks up to 128 MB,  2k cluster */
            {   524288,    8},   /* disks up to 256 MB,  4k cluster */
            { 1048576,  16},     /* disks up to 512 MB,  8k cluster */
        };
        // from Microsoft's FAT format technical design document
        static constexpr disksize_to_sectors_per_cluster kDiskTableFat32[] =
        {
            { 16777216,   8},     /* disks up to     8 GB,    4k cluster */
            { 33554432, 16},      /* disks up to   16 GB,    8k cluster */
            { 67108864, 32},      /* disks up to   32 GB,  16k cluster */
            { 0xFFFFFFFF, 64}		/* disks greater than 32GB, 32k cluster */
        };
    }
}