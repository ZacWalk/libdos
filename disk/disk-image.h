#pragma once

// libdos by Zac Walker
//
// Bootable DOS disk images: a FAT12 floppy, or a hard disk partitioned and
// formatted as DOS 4.0's FDISK and FORMAT leave one - and reading either back.
// Platform independent like the emulator: nothing here opens a file. A caller
// hands in the boot code and the files' bytes, and a sink or a source for the
// sectors.

#include <cstdint>
#include <string>
#include <vector>

constexpr uint32_t disk_sector_size = 512;

// A file for a volume's root directory.
struct disk_file
{
	std::string name; // cut to 8.3; any directories in it are dropped
	std::vector<uint8_t> data;
	bool system = false; // read-only, hidden and system, as IO.SYS and MSDOS.SYS are
};

// The date and time every directory entry gets, and the volume serial number,
// to which each volume adds the sector it starts at so no two are the same.
struct disk_stamp
{
	uint16_t date = 0; // DOS format: year - 1980, month and day
	uint16_t time = 0; // hours, minutes and seconds / 2
	uint32_t serial = 0;
};

// Where an image is written. It is told its size first and then gets only the
// sectors that hold something, so a multi-gigabyte disk can be a sparse file.
class disk_sink
{
public:
	virtual ~disk_sink() = default;
	virtual bool size(uint64_t bytes) = 0;
	virtual bool write(uint64_t offset, const uint8_t* data, size_t length) = 0;
};

// Where an image is read from.
class disk_source
{
public:
	virtual ~disk_source() = default;
	virtual bool read(uint64_t offset, uint8_t* data, size_t length) = 0;
};

// A FAT volume: one disk_build_* made, or one disk_volumes found.
struct disk_volume
{
	uint64_t start = 0; // the sector holding its boot sector
	uint64_t sectors = 0;
	uint8_t type = 0; // its partition type; 0 on a floppy
	char drive = 0; // the letter DOS 4.0 gives it, or 0 for a partition DOS does not use
	bool formatted = false; // a FAT12 or FAT16 boot sector; the rest is unknown if not
	bool fat16 = false;
	std::string label;
	uint32_t sectors_per_cluster = 0;
	uint32_t clusters = 0;
	uint32_t free_clusters = 0;
};

struct disk_report
{
	uint32_t cylinders = 0;
	uint32_t heads = 0;
	uint32_t sectors_per_track = 0;
	uint64_t sectors = 0;
	std::vector<disk_volume> volumes;
};

// A floppy of 360 or 1440 KB, built whole in `image`. `boot_sector` is the 512
// bytes of the boot record, whose parameter block is rewritten for the format.
// The files go into the root directory in order, the label after the system
// files that lead the list: the boot record loads the first two entries.
bool disk_build_floppy(int kilobytes, const uint8_t* boot_sector, const std::vector<disk_file>& files,
                       const std::string& label, const disk_stamp& stamp, std::vector<uint8_t>& image,
                       disk_report& report, std::string& error);

struct disk_layout
{
	uint32_t megabytes = 0;
	uint32_t partition_megabytes = 2047; // the largest volume, capped where FAT16 stops
	uint32_t heads = 0; // 0 for 16 up to 504 MB and 255 above; 63 sectors a track either way
	bool lba_types = false; // partition types 0Eh and 0Fh rather than 06h and 05h
};

// A hard disk in whole cylinders, behind FDISK's `master_boot` record: C:, the
// primary partition, holds the files under `label` as a floppy does, and logical
// drives labelled DRIVE D, DRIVE E and so on fill the rest of the disk in an
// extended partition, up to Z:. Partitions start and end on a cylinder as FDISK
// makes them, and each volume is laid out as FORMAT lays one out.
bool disk_build_hard_disk(const disk_layout& layout, const uint8_t* master_boot, const uint8_t* boot_sector,
                          const std::vector<disk_file>& files, const std::string& label,
                          const disk_stamp& stamp, disk_sink& sink, disk_report& report, std::string& error);

// The volumes on an image: a floppy's one, or a hard disk's partitions - the
// primary ones in table order, then the extended partition's chain. Each has
// the letter DOS 4.0 would give it: a floppy's is A:, and on a hard disk C: is
// the first primary partition of a DOS type (01h, 04h, 06h or 0Eh) and the
// logical drives of a DOS type follow, formatted or not.
bool disk_volumes(disk_source& source, std::vector<disk_volume>& volumes, std::string& error);

struct disk_entry
{
	std::string path; // "\DOS\FORMAT.COM"
	uint32_t size = 0;
	uint8_t attributes = 0;
	uint16_t date = 0;
	uint16_t time = 0;
};

// Every file and directory on a formatted volume, a directory before what is in it.
bool disk_list(disk_source& source, const disk_volume& volume, std::vector<disk_entry>& entries,
               std::string& error);

// A file's bytes, by its path on the volume: "\COMMAND.COM" or "DOS/FORMAT.COM".
bool disk_read_file(disk_source& source, const disk_volume& volume, const std::string& path,
                    std::vector<uint8_t>& data, std::string& error);

// The 11 bytes a directory entry names a file with: up to eight of its name and
// three of its extension, upper case and padded with spaces.
void disk_name_83(const std::string& name, uint8_t out[11]);
