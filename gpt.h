#pragma once

namespace disktools
{
    namespace gpt
    {
        // UEFI Specification 2.6, Chapter 5
        static constexpr uint8_t kUefiPartitionOSType = 0xef;
        static constexpr uint8_t kGptProtectivePartitionOSType = 0xee;
        static constexpr uint64_t kEfiPartSignature = 0x5452415020494645; // "EFI PART"
        static constexpr uint32_t kEfiRevision = 0x00010000;
        // as per standard
        static constexpr uint8_t kEfiSystemPartitionUuid[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4B, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
        // as per standard
        static constexpr uint8_t kNoVolumeLabel[11] = { 'N','O',' ','N','A','M','E',' ',' ',' ',' ' };
        static constexpr uint8_t kEfiBootPartName[] = { 'E', 'F', 'I', ' ', 'B', 'O', 'O', 'T' };

#pragma pack(push,1)
        struct mbr_partition_record
        {
            uint8_t		_boot_indicator;
            uint8_t		_starting_chs[3];
            uint8_t		_os_type;			//< always 0xee for GPT protective
            uint8_t		_ending_chs[3];
            uint32_t	_starting_lba;
            uint32_t	_size_in_lba;

        };

        struct gpt_header
        {
            uint64_t		_signature;	// EFI PART
            uint32_t		_revision;
            uint32_t		_header_size;
            uint32_t		_header_crc32;
            uint32_t		_reserved0;
            uint64_t		_my_lba;
            uint64_t		_alternate_lba;
            uint64_t		_first_usable_lba;
            uint64_t		_last_usable_lba;
            uint8_t			_disk_guid[16];
            uint64_t		_partition_entry_lba;
            uint32_t		_partition_entry_count;
            uint32_t		_partition_entry_size;
            uint32_t		_partition_array_crc32;

            // remainder of sector is 0

        };

        struct gpt_partition_header
        {
            // Unused Entry 00000000-0000-0000-0000-000000000000
            // EFI System Partition C12A7328-F81F-11D2-BA4B-00A0C93EC93B
            // Partition containing a legacy MBR 024DEE41-33E7-11D3-9D69-0008C781F39F
            uint8_t			_type_guid[16];

            uint8_t			_part_guid[16];
            uint64_t		_start_lba;
            uint64_t		_end_lba;
            uint64_t		_attributes;
            uint8_t			_name[72];

            // remainder of sector is 0
        };
#pragma pack(pop)
    }
}
