// libdos by Zac Walker
//
// dimg: a bootable DOS disk image from a directory of DOS binaries - a floppy,
// or a hard disk partitioned and formatted as FDISK and FORMAT would leave it -
// and the volumes and files on one, listed or copied out. The images are
// disk/disk-image.h's; this is the files and the command line.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "disk-image.h"

namespace
{
	bool read_file(const std::string& path, std::vector<uint8_t>& data)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) return false;
		data.clear();
		uint8_t buffer[65536];
		size_t length;
		while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0) data.insert(data.end(), buffer, buffer + length);
		const bool read = !ferror(file);
		fclose(file);
		return read;
	}

	std::string join(const std::string& directory, const char* name)
	{
		if (directory.empty() || directory.back() == '\\' || directory.back() == '/') return directory + name;
		return directory + "\\" + name;
	}

	// 512 bytes of boot code at `offset` in a binary EXE2BIN made: the boot
	// sector is linked at 7C00h and FDISK's master boot record at 600h.
	bool read_boot_code(const std::string& directory, const char* name, size_t offset, uint8_t* out)
	{
		const std::string path = join(directory, name);
		std::vector<uint8_t> data;
		if (!read_file(path, data) || data.size() < offset + disk_sector_size || data[offset + 510] != 0x55 ||
		    data[offset + 511] != 0xaa)
		{
			fprintf(stderr, "Error: no boot record at offset 0x%zX in '%s'\n", offset, path.c_str());
			return false;
		}
		memcpy(out, data.data() + offset, disk_sector_size);
		return true;
	}

	// The DOS date and time of `when`, which is the volume serial number too.
	disk_stamp make_stamp(time_t when, bool utc)
	{
		tm t = {};
		if (utc)
			gmtime_s(&t, &when);
		else
			localtime_s(&t, &when);
		disk_stamp stamp;
		stamp.time = uint16_t((t.tm_hour & 0x1f) << 11 | (t.tm_min & 0x3f) << 5 | (t.tm_sec / 2 & 0x1f));
		stamp.date = uint16_t(((t.tm_year - 80) & 0x7f) << 9 | ((t.tm_mon + 1) & 0x0f) << 5 | (t.tm_mday & 0x1f));
		stamp.serial = uint32_t(when);
		return stamp;
	}

	class file_sink final : public disk_sink
	{
	public:
		explicit file_sink(FILE* file) : file(file)
		{
		}

		// Sparse, so sectors never written take no space: _chsize_s would
		// write zeros to the end.
		bool size(uint64_t bytes) override
		{
			const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
			DWORD returned = 0;
			DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
			LARGE_INTEGER end;
			end.QuadPart = LONGLONG(bytes);
			return SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) && SetEndOfFile(handle);
		}

		bool write(uint64_t offset, const uint8_t* data, size_t length) override
		{
			return _fseeki64(file, int64_t(offset), SEEK_SET) == 0 && fwrite(data, 1, length, file) == length;
		}

	private:
		FILE* file;
	};

	class file_source final : public disk_source
	{
	public:
		explicit file_source(FILE* file) : file(file)
		{
		}

		bool read(uint64_t offset, uint8_t* data, size_t length) override
		{
			return _fseeki64(file, int64_t(offset), SEEK_SET) == 0 && fread(data, 1, length, file) == length;
		}

	private:
		FILE* file;
	};

	struct options
	{
		std::string output;
		std::string binaries;
		std::string label = "MSDOS4";
		std::vector<std::string> extra;
		int floppy_kilobytes = 360;
		disk_layout layout;
		bool system = true;
		bool verbose = false;
		disk_stamp stamp;
	};

	void usage()
	{
		fprintf(stderr,
		        "dimg - make a bootable DOS disk image, or look inside one\n"
		        "\n"
		        "Usage:\n"
		        "  dimg -o <image> -b <bindir> [options]   make a floppy, or a hard disk with -hd\n"
		        "  dimg -dir <image>                       list the volumes and the files on them\n"
		        "  dimg -extract <image> <path> <file>     copy a file out, as C:\\DOS\\FORMAT.COM\n"
		        "\n"
		        "Making an image:\n"
		        "  -o <file>     the image to write\n"
		        "  -b <dir>      the DOS binaries: MSBOOT.BIN (boot sector at offset 7C00h),\n"
		        "                FDBOOT.BIN for -hd (master boot record at 600h), IO.SYS,\n"
		        "                MSDOS.SYS and COMMAND.COM\n"
		        "  -s <size>     floppy size: 360 or 1440 (default: 360)\n"
		        "  -f <file>     add a file to the root directory (repeatable)\n"
		        "  -l <label>    volume label (default: MSDOS4)\n"
		        "  -nosys        leave out IO.SYS, MSDOS.SYS and COMMAND.COM\n"
		        "  -t <seconds>  date the files and number the volumes from this time, in\n"
		        "                seconds since 1970 UTC (default: SOURCE_DATE_EPOCH if set,\n"
		        "                else the time now)\n"
		        "  -v            name each file as it is added\n"
		        "\n"
		        "Hard disk images:\n"
		        "  -hd <MB>      a partitioned hard disk of this size instead of a floppy:\n"
		        "                C: as the primary partition, then logical drives in an\n"
		        "                extended partition until the disk is full or Z: is reached\n"
		        "  -p <MB>       largest partition (default and maximum: 2047)\n"
		        "  -lba          partition types 0Eh and 0Fh instead of 06h and 05h\n"
		        "  -heads <n>    heads (default 16 up to 504 MB, else 255); 63 sectors/track\n"
		        "\n"
		        "IO.SYS and MSDOS.SYS come first in the root directory, as the boot sector\n"
		        "needs, then the volume label, COMMAND.COM and the files given with -f.\n");
	}

	int make_image(const options& o)
	{
		uint8_t boot_sector[disk_sector_size];
		if (!read_boot_code(o.binaries, "MSBOOT.BIN", 0x7c00, boot_sector)) return 1;

		std::vector<std::string> paths;
		std::vector<disk_file> files;
		if (o.system)
		{
			for (const char* name : {"io.sys", "msdos.sys", "command.com"}) paths.push_back(join(o.binaries, name));
		}
		paths.insert(paths.end(), o.extra.begin(), o.extra.end());
		for (size_t i = 0; i < paths.size(); ++i)
		{
			disk_file file;
			file.name = paths[i];
			file.system = o.system && i < 2;
			if (!read_file(paths[i], file.data))
			{
				fprintf(stderr, "Error: cannot open '%s'\n", paths[i].c_str());
				return 1;
			}
			if (o.verbose) printf("Adding %s\n", paths[i].c_str());
			files.push_back(std::move(file));
		}

		disk_report report;
		std::string error;
		if (o.layout.megabytes == 0)
		{
			std::vector<uint8_t> image;
			if (!disk_build_floppy(o.floppy_kilobytes, boot_sector, files, o.label, o.stamp, image, report, error))
			{
				fprintf(stderr, "Error: %s\n", error.c_str());
				return 1;
			}
			FILE* file = nullptr;
			if (fopen_s(&file, o.output.c_str(), "wb") != 0 || file == nullptr)
			{
				fprintf(stderr, "Error: cannot create '%s'\n", o.output.c_str());
				return 1;
			}
			const bool written = fwrite(image.data(), 1, image.size(), file) == image.size();
			if (fclose(file) != 0 || !written)
			{
				fprintf(stderr, "Error: write failed on '%s'\n", o.output.c_str());
				return 1;
			}
			const disk_volume& v = report.volumes[0];
			const uint32_t used = v.clusters - v.free_clusters;
			printf("Created %d KB floppy: %s\n", o.floppy_kilobytes, o.output.c_str());
			printf("  Files: %zu (and the volume label)\n", files.size());
			printf("  Clusters used: %u / %u (%u KB free)\n", used, v.clusters,
			       v.free_clusters * v.sectors_per_cluster * disk_sector_size / 1024);
			return 0;
		}

		uint8_t master_boot[disk_sector_size];
		if (!read_boot_code(o.binaries, "fdboot.bin", 0x600, master_boot)) return 1;
		FILE* file = nullptr;
		if (fopen_s(&file, o.output.c_str(), "wb+") != 0 || file == nullptr)
		{
			fprintf(stderr, "Error: cannot create '%s'\n", o.output.c_str());
			return 1;
		}
		file_sink sink(file);
		const bool built = disk_build_hard_disk(o.layout, master_boot, boot_sector, files, o.label, o.stamp, sink,
		                                        report, error);
		if (fclose(file) != 0 && built) error = "write failed on '" + o.output + "'";
		if (!built || !error.empty())
		{
			fprintf(stderr, "Error: %s\n", error.c_str());
			return 1;
		}
		for (const disk_volume& v : report.volumes)
			printf("  %c: %5llu MB at sector %10llu, type %02Xh, %2u sectors/cluster\n",
			       'C' + int(&v - report.volumes.data()), v.sectors / 2048, v.start, v.type,
			       v.sectors_per_cluster);
		const uint64_t cylinder = uint64_t(report.heads) * report.sectors_per_track;
		printf("Created %llu MB hard disk: %s\n", report.sectors / 2048, o.output.c_str());
		printf("  Geometry: %u cylinders, %u heads, %u sectors/track (%llu sectors)\n", report.cylinders,
		       report.heads, report.sectors_per_track, report.sectors);
		printf("  DOSBox:   imgmount 2 \"%s\" -size 512,%u,%u,%u -fs none\n", o.output.c_str(),
		       report.sectors_per_track, report.heads, report.cylinders);
		if (report.cylinders > 1024)
			printf("  Beyond cylinder 1023: only the first %llu MB are reachable without LBA\n",
			       1024 * cylinder / 2048);
		return 0;
	}

	// The image's FAT volumes, lettered as DOS letters them: A: for a floppy,
	// C: onwards for a hard disk's.
	bool open_image(const char* path, FILE*& file, std::vector<disk_volume>& volumes, char& first_letter)
	{
		if (fopen_s(&file, path, "rb") != 0 || file == nullptr)
		{
			fprintf(stderr, "Error: cannot open '%s'\n", path);
			return false;
		}
		file_source source(file);
		std::string error;
		if (!disk_volumes(source, volumes, error))
		{
			fprintf(stderr, "Error: %s: %s\n", path, error.c_str());
			fclose(file);
			return false;
		}
		first_letter = !volumes.empty() && volumes[0].type == 0 ? 'A' : 'C';
		return true;
	}

	void print_date(uint16_t date, uint16_t time)
	{
		printf("%04u-%02u-%02u %02u:%02u", 1980u + (date >> 9), unsigned(date >> 5 & 15), unsigned(date & 31),
		       unsigned(time >> 11), unsigned(time >> 5 & 63));
	}

	int list_image(const char* path)
	{
		FILE* file = nullptr;
		std::vector<disk_volume> volumes;
		char letter = 'C';
		if (!open_image(path, file, volumes, letter)) return 1;
		file_source source(file);
		int result = 0;
		for (const disk_volume& v : volumes)
		{
			if (!v.formatted)
			{
				printf("Partition type %02Xh at sector %llu, %llu MB: not FAT12 or FAT16\n\n", v.type, v.start,
				       v.sectors / 2048);
				continue;
			}
			const uint64_t cluster_bytes = uint64_t(v.sectors_per_cluster) * disk_sector_size;
			printf("Volume %c: %s, %s", letter++, v.label.empty() ? "no label" : v.label.c_str(),
			       v.fat16 ? "FAT16" : "FAT12");
			if (v.type) printf(" type %02Xh", v.type);
			printf(", %llu KB at sector %llu, %llu KB free\n", v.sectors / 2, v.start,
			       v.free_clusters * cluster_bytes / 1024);
			std::vector<disk_entry> entries;
			std::string error;
			if (!disk_list(source, v, entries, error))
			{
				fprintf(stderr, "Error: %s\n", error.c_str());
				result = 1;
				continue;
			}
			for (const disk_entry& e : entries)
			{
				printf("  %-40s ", e.path.c_str());
				if (e.attributes & 0x10)
					printf("     <DIR> ");
				else
					printf("%10u ", e.size);
				print_date(e.date, e.time);
				printf(" %c%c%c%c\n", e.attributes & 0x01 ? 'R' : '-', e.attributes & 0x02 ? 'H' : '-',
				       e.attributes & 0x04 ? 'S' : '-', e.attributes & 0x20 ? 'A' : '-');
			}
			printf("\n");
		}
		fclose(file);
		return result;
	}

	int extract_file(const char* path, const char* name, const char* target)
	{
		FILE* file = nullptr;
		std::vector<disk_volume> volumes;
		char letter = 'C';
		if (!open_image(path, file, volumes, letter)) return 1;
		std::vector<const disk_volume*> lettered;
		for (const disk_volume& v : volumes)
			if (v.formatted) lettered.push_back(&v);

		std::string inside = name;
		size_t index = 0;
		if (inside.size() >= 2 && inside[1] == ':')
		{
			index = size_t((inside[0] | 0x20) - (letter | 0x20));
			inside = inside.substr(2);
		}
		if (index >= lettered.size())
		{
			if (lettered.empty())
				fprintf(stderr, "Error: '%s' has no FAT12 or FAT16 volume\n", path);
			else
				fprintf(stderr, "Error: '%s' has no drive %c:\n", path, name[0]);
			fclose(file);
			return 1;
		}
		file_source source(file);
		std::vector<uint8_t> data;
		std::string error;
		const bool read = disk_read_file(source, *lettered[index], inside, data, error);
		fclose(file);
		if (!read)
		{
			fprintf(stderr, "Error: %s\n", error.c_str());
			return 1;
		}
		FILE* out = nullptr;
		if (fopen_s(&out, target, "wb") != 0 || out == nullptr)
		{
			fprintf(stderr, "Error: cannot create '%s'\n", target);
			return 1;
		}
		const bool written = fwrite(data.data(), 1, data.size(), out) == data.size();
		if (fclose(out) != 0 || !written)
		{
			fprintf(stderr, "Error: write failed on '%s'\n", target);
			return 1;
		}
		return 0;
	}
}

int main(int argc, char* argv[])
{
	if (argc == 3 && strcmp(argv[1], "-dir") == 0) return list_image(argv[2]);
	if (argc == 5 && strcmp(argv[1], "-extract") == 0) return extract_file(argv[2], argv[3], argv[4]);

	options o;
	bool fixed_time = false;
	time_t when = 0;
	char epoch[32];
	const DWORD epoch_length = GetEnvironmentVariableA("SOURCE_DATE_EPOCH", epoch, sizeof(epoch));
	if (epoch_length > 0 && epoch_length < sizeof(epoch))
	{
		when = time_t(_strtoi64(epoch, nullptr, 10));
		fixed_time = true;
	}
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		const bool value = i + 1 < argc;
		if (arg == "-o" && value)
			o.output = argv[++i];
		else if (arg == "-b" && value)
			o.binaries = argv[++i];
		else if (arg == "-s" && value)
		{
			o.floppy_kilobytes = atoi(argv[++i]);
			if (o.floppy_kilobytes != 360 && o.floppy_kilobytes != 1440)
			{
				fprintf(stderr, "Error: -s must be 360 or 1440\n");
				return 1;
			}
		}
		else if (arg == "-f" && value)
			o.extra.push_back(argv[++i]);
		else if (arg == "-l" && value)
			o.label = argv[++i];
		else if (arg == "-hd" && value)
			o.layout.megabytes = uint32_t(strtoul(argv[++i], nullptr, 10));
		else if (arg == "-p" && value)
			o.layout.partition_megabytes = uint32_t(strtoul(argv[++i], nullptr, 10));
		else if (arg == "-heads" && value)
		{
			o.layout.heads = uint32_t(strtoul(argv[++i], nullptr, 10));
			if (o.layout.heads < 1 || o.layout.heads > 255)
			{
				fprintf(stderr, "Error: -heads must be 1 to 255\n");
				return 1;
			}
		}
		else if (arg == "-t" && value)
		{
			when = time_t(_strtoi64(argv[++i], nullptr, 10));
			fixed_time = true;
		}
		else if (arg == "-lba")
			o.layout.lba_types = true;
		else if (arg == "-nosys")
			o.system = false;
		else if (arg == "-v")
			o.verbose = true;
		else if (arg == "-h" || arg == "--help")
		{
			usage();
			return 0;
		}
		else
		{
			fprintf(stderr, "Error: unknown option '%s'\n\n", arg.c_str());
			usage();
			return 1;
		}
	}
	if (o.output.empty() || o.binaries.empty())
	{
		fprintf(stderr, "Error: -o and -b are required\n\n");
		usage();
		return 1;
	}
	if (o.layout.megabytes && o.layout.partition_megabytes < 1)
	{
		fprintf(stderr, "Error: -p must be at least 1 MB\n");
		return 1;
	}
	// DOS dates run from 1980 to 2107.
	if (fixed_time && (when < 315532800 || when > 4354819199))
	{
		fprintf(stderr, "Error: the time must fall between 1980 and 2107\n");
		return 1;
	}
	o.stamp = make_stamp(fixed_time ? when : time(nullptr), fixed_time);
	return make_image(o);
}
