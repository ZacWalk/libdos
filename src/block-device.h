#pragma once

// libdos by Zac Walker
//
// A drive for boot mode's BIOS: sectors a host keeps wherever it likes - an
// image file, memory - read and written by LBA. The emulator never learns
// where they live.

#include <cstdint>

class block_device
{
public:
	virtual ~block_device() = default;
	virtual uint64_t sectors() = 0;
	virtual bool read(uint64_t lba, uint8_t* data, uint32_t count) = 0;
	virtual bool write(uint64_t lba, const uint8_t* data, uint32_t count) = 0;
};

// How INT 13h's CHS calls see a drive.
struct disk_geometry
{
	uint32_t cylinders = 0;
	uint32_t heads = 0;
	uint32_t sectors_per_track = 0;
};
