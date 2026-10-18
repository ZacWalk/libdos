// libdos by Zac Walker
//
// See disk-image.h. A hard disk follows what DOS 4.0's own FDISK and FORMAT
// make: FDISK's cylinder alignment and partition types, and the cluster size
// and FAT that IO.SYS's DiskTable2 gives a volume of each size.

#include "disk-image.h"

#include <algorithm>
#include <cstring>

namespace
{
	constexpr uint32_t sector = disk_sector_size;
	constexpr uint32_t hard_disk_sectors_per_track = 63;
	constexpr uint32_t hard_disk_root_entries = 512;
	constexpr int hard_disk_volumes = 24; // C: to Z:
	constexpr uint64_t largest_volume = 4194144; // 65524 clusters of 64 sectors

	void put16(uint8_t* p, uint32_t value)
	{
		p[0] = uint8_t(value);
		p[1] = uint8_t(value >> 8);
	}

	void put32(uint8_t* p, uint32_t value)
	{
		put16(p, value);
		put16(p + 2, value >> 16);
	}

	uint32_t get16(const uint8_t* p)
	{
		return p[0] | p[1] << 8;
	}

	uint32_t get32(const uint8_t* p)
	{
		return get16(p) | get16(p + 2) << 16;
	}

	uint8_t upper(char c)
	{
		return uint8_t(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
	}

	std::string trimmed(const uint8_t* text, size_t length)
	{
		while (length > 0 && text[length - 1] == ' ') --length;
		return std::string(reinterpret_cast<const char*>(text), length);
	}

	class vector_sink final : public disk_sink
	{
	public:
		explicit vector_sink(std::vector<uint8_t>& image) : image(image)
		{
		}

		bool size(uint64_t bytes) override
		{
			image.assign(size_t(bytes), 0);
			return true;
		}

		bool write(uint64_t offset, const uint8_t* data, size_t length) override
		{
			if (offset > image.size() || length > image.size() - offset) return false;
			std::memcpy(image.data() + offset, data, length);
			return true;
		}

	private:
		std::vector<uint8_t>& image;
	};

	// A volume being built. File data goes straight to the sink; the FAT and
	// the root directory are held until write_volume.
	struct builder
	{
		disk_sink& sink;
		const disk_stamp& stamp;
		disk_volume info;
		uint32_t sectors_per_fat = 0;
		uint32_t root_entries = 0;
		uint8_t media = 0;
		uint32_t next_cluster = 2;
		uint32_t entries = 0;
		std::vector<uint8_t> fat;
		std::vector<uint8_t> root;
		uint8_t label[11] = {};

		uint64_t data_start() const
		{
			return info.start + 1 + 2ull * sectors_per_fat + root_entries * 32 / sector;
		}
	};

	// One reserved sector, two FATs, the root directory, then the clusters.
	void begin_volume(builder& b, uint64_t start, uint64_t sectors, uint32_t sectors_per_cluster,
	                  uint32_t sectors_per_fat, uint32_t root_entries, uint8_t media, bool fat16)
	{
		b.info = {};
		b.info.start = start;
		b.info.sectors = sectors;
		b.info.formatted = true;
		b.info.fat16 = fat16;
		b.info.sectors_per_cluster = sectors_per_cluster;
		b.info.clusters = uint32_t((sectors - 1 - 2ull * sectors_per_fat - root_entries * 32 / sector) /
		                           sectors_per_cluster);
		b.info.free_clusters = b.info.clusters;
		b.sectors_per_fat = sectors_per_fat;
		b.root_entries = root_entries;
		b.media = media;
		b.next_cluster = 2;
		b.entries = 0;
		b.fat.assign(size_t(sectors_per_fat) * sector, 0);
		b.fat[0] = media;
		b.fat[1] = 0xff;
		b.fat[2] = 0xff;
		if (fat16) b.fat[3] = 0xff;
		b.root.assign(size_t(root_entries) * 32, 0);
		std::memset(b.label, ' ', sizeof(b.label));
	}

	void set_fat(builder& b, uint32_t cluster, uint32_t value)
	{
		if (b.info.fat16)
		{
			put16(&b.fat[size_t(cluster) * 2], value);
			return;
		}
		uint8_t* p = &b.fat[cluster + cluster / 2];
		if (cluster & 1)
		{
			p[0] = uint8_t((p[0] & 0x0f) | (value & 0x0f) << 4);
			p[1] = uint8_t(value >> 4);
		}
		else
		{
			p[0] = uint8_t(value);
			p[1] = uint8_t((p[1] & 0xf0) | (value >> 8 & 0x0f));
		}
	}

	uint8_t* new_entry(builder& b, std::string& error)
	{
		if (b.entries >= b.root_entries)
		{
			error = "the root directory is full";
			return nullptr;
		}
		uint8_t* entry = &b.root[size_t(b.entries++) * 32];
		put16(entry + 22, b.stamp.time);
		put16(entry + 24, b.stamp.date);
		return entry;
	}

	// Files are never deleted from a volume being built, so every one is
	// contiguous - which IO.SYS has to be.
	bool add_file(builder& b, const disk_file& file, std::string& error)
	{
		const uint32_t cluster_bytes = b.info.sectors_per_cluster * sector;
		const uint64_t count = (file.data.size() + cluster_bytes - 1) / cluster_bytes;
		if (b.next_cluster + count > uint64_t(b.info.clusters) + 2)
		{
			error = "the volume is full: no room for " + file.name;
			return false;
		}
		std::vector<uint8_t> cluster(cluster_bytes);
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint32_t number = b.next_cluster + i;
			const size_t offset = size_t(i) * cluster_bytes;
			const size_t length = std::min<size_t>(cluster_bytes, file.data.size() - offset);
			std::memcpy(cluster.data(), file.data.data() + offset, length);
			std::fill(cluster.begin() + length, cluster.end(), uint8_t(0));
			if (!b.sink.write((b.data_start() + uint64_t(number - 2) * b.info.sectors_per_cluster) * sector,
			                  cluster.data(), cluster_bytes))
			{
				error = "cannot write " + file.name;
				return false;
			}
			set_fat(b, number, i + 1 < count ? number + 1 : b.info.fat16 ? 0xffff : 0xfff);
		}
		uint8_t* entry = new_entry(b, error);
		if (!entry) return false;
		disk_name_83(file.name, entry);
		entry[11] = file.system ? 0x07 : 0x20;
		put16(entry + 26, count ? b.next_cluster : 0);
		put32(entry + 28, uint32_t(file.data.size()));
		b.next_cluster += uint32_t(count);
		b.info.free_clusters -= uint32_t(count);
		return true;
	}

	bool add_label(builder& b, const std::string& text, std::string& error)
	{
		for (size_t i = 0; i < sizeof(b.label) && i < text.size(); ++i) b.label[i] = upper(text[i]);
		uint8_t* entry = new_entry(b, error);
		if (!entry) return false;
		std::memcpy(entry, b.label, sizeof(b.label));
		entry[11] = 0x08;
		b.info.label = trimmed(b.label, sizeof(b.label));
		return true;
	}

	bool fill_volume(builder& b, const std::vector<disk_file>& files, const std::string& label, std::string& error)
	{
		size_t i = 0;
		for (; i < files.size() && files[i].system; ++i)
			if (!add_file(b, files[i], error)) return false;
		if (!add_label(b, label, error)) return false;
		for (; i < files.size(); ++i)
			if (!add_file(b, files[i], error)) return false;
		return true;
	}

	// The boot sector with its parameter block filled in, both FATs and the
	// root directory.
	bool write_volume(builder& b, const uint8_t* boot_sector, uint32_t heads, uint32_t sectors_per_track,
	                  uint8_t drive, std::string& error)
	{
		uint8_t boot[sector];
		std::memcpy(boot, boot_sector, sector);
		const uint64_t sectors = b.info.sectors;
		put16(boot + 11, sector);
		boot[13] = uint8_t(b.info.sectors_per_cluster);
		put16(boot + 14, 1);
		boot[16] = 2;
		put16(boot + 17, b.root_entries);
		put16(boot + 19, sectors < 65536 ? uint32_t(sectors) : 0);
		boot[21] = b.media;
		put16(boot + 22, b.sectors_per_fat);
		put16(boot + 24, sectors_per_track);
		put16(boot + 26, heads);
		put32(boot + 28, uint32_t(b.info.start)); // hidden sectors: those before the volume
		put32(boot + 32, sectors < 65536 ? 0 : uint32_t(sectors));
		boot[36] = drive;
		boot[37] = 0;
		boot[38] = 0x29; // the serial number, label and file system type follow
		put32(boot + 39, b.stamp.serial + uint32_t(b.info.start));
		std::memcpy(boot + 43, b.label, sizeof(b.label));
		std::memcpy(boot + 54, b.info.fat16 ? "FAT16   " : "FAT12   ", 8);

		const uint64_t offset = b.info.start * sector;
		const size_t fat_bytes = b.fat.size();
		if (!b.sink.write(offset, boot, sector) || !b.sink.write(offset + sector, b.fat.data(), fat_bytes) ||
		    !b.sink.write(offset + sector + fat_bytes, b.fat.data(), fat_bytes) ||
		    !b.sink.write(offset + sector + 2ull * fat_bytes, b.root.data(), b.root.size()))
		{
			error = "cannot write the volume at sector " + std::to_string(b.info.start);
			return false;
		}
		return true;
	}

	// FORMAT's choice for a hard disk volume of this size: the cluster size
	// from DiskTable2, and a FAT just big enough for the clusters that leaves.
	void begin_hard_disk_volume(builder& b, uint64_t start, uint64_t sectors)
	{
		static const struct
		{
			uint64_t most;
			uint32_t sectors_per_cluster;
		} table[] = {
			{32680, 8}, {0x40000, 4}, {0x80000, 8}, {0x100000, 16}, {0x200000, 32}, {largest_volume, 64},
		};
		size_t i = 0;
		while (i + 1 < std::size(table) && sectors > table[i].most) ++i;
		const uint32_t sectors_per_cluster = table[i].sectors_per_cluster;
		const bool fat16 = sectors > 32680;
		uint32_t sectors_per_fat;
		if (fat16)
		{
			const uint64_t per_fat_sector = 256ull * sectors_per_cluster + 2;
			sectors_per_fat = uint32_t((sectors - 1 - 32 + per_fat_sector - 1) / per_fat_sector);
		}
		else
		{
			const uint64_t clusters = ((sectors + sectors_per_cluster - 1) / sectors_per_cluster + 1) & ~1ull;
			sectors_per_fat = uint32_t((clusters + clusters / 2 + 511) / 512);
		}
		begin_volume(b, start, sectors, sectors_per_cluster, sectors_per_fat, hard_disk_root_entries, 0xf8, fat16);
	}

	// A partition table entry's CHS address. Beyond cylinder 1023 the largest
	// is stored, as partitioners that know LBA do.
	void put_chs(uint8_t* p, uint64_t lba, uint32_t heads, uint32_t sectors_per_track)
	{
		uint64_t cylinder = lba / (uint64_t(heads) * sectors_per_track);
		uint32_t head = uint32_t(lba / sectors_per_track % heads);
		uint32_t number = uint32_t(lba % sectors_per_track) + 1;
		if (cylinder > 1023)
		{
			cylinder = 1023;
			head = heads - 1;
			number = sectors_per_track;
		}
		p[0] = uint8_t(head);
		p[1] = uint8_t(number | (cylinder >> 2 & 0xc0));
		p[2] = uint8_t(cylinder);
	}

	void put_partition(uint8_t* entry, bool active, uint8_t type, uint64_t start, uint64_t relative,
	                   uint64_t count, uint32_t heads, uint32_t sectors_per_track)
	{
		entry[0] = active ? 0x80 : 0x00;
		put_chs(entry + 1, start, heads, sectors_per_track);
		entry[4] = type;
		put_chs(entry + 5, start + count - 1, heads, sectors_per_track);
		put32(entry + 8, uint32_t(relative));
		put32(entry + 12, uint32_t(count));
	}

	// The partition type DOS 4.0's FDISK gives a volume of this size.
	uint8_t partition_type(uint64_t sectors, bool lba)
	{
		if (sectors <= 32680) return 0x01;
		if (lba) return 0x0e;
		return sectors <= 65535 ? 0x04 : 0x06;
	}

	// What a volume's boot sector says about where everything is.
	struct fat_layout
	{
		uint64_t sectors = 0;
		uint32_t sectors_per_cluster = 0;
		uint32_t sectors_per_fat = 0;
		uint32_t root_entries = 0;
		uint64_t fat_start = 0; // absolute sectors
		uint64_t root_start = 0;
		uint64_t data_start = 0;
		uint32_t clusters = 0;
		bool fat16 = false;
	};

	bool parse_boot_sector(const uint8_t* boot, uint64_t start, fat_layout& f)
	{
		const uint32_t sectors_per_cluster = boot[13];
		const uint32_t reserved = get16(boot + 14);
		const uint32_t fats = boot[16];
		const uint32_t root_entries = get16(boot + 17);
		const uint64_t sectors = get16(boot + 19) ? get16(boot + 19) : get32(boot + 32);
		const uint32_t sectors_per_fat = get16(boot + 22);
		if (get16(boot + 11) != sector || sectors_per_cluster == 0 ||
		    (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 || reserved == 0 || fats == 0 || fats > 2 ||
		    root_entries == 0 || sectors_per_fat == 0 || boot[21] < 0xf0)
			return false;
		const uint64_t data = reserved + uint64_t(fats) * sectors_per_fat + (root_entries * 32 + sector - 1) / sector;
		if (sectors <= data) return false;
		const uint64_t clusters = (sectors - data) / sectors_per_cluster;
		if (clusters == 0 || clusters >= 65525) return false; // FAT32 starts here
		f.sectors = sectors;
		f.sectors_per_cluster = sectors_per_cluster;
		f.sectors_per_fat = sectors_per_fat;
		f.root_entries = root_entries;
		f.fat_start = start + reserved;
		f.root_start = f.fat_start + uint64_t(fats) * sectors_per_fat;
		f.data_start = start + data;
		f.clusters = uint32_t(clusters);
		f.fat16 = clusters >= 4085;
		return true;
	}

	// A cluster's FAT entry; one past the end of the FAT reads as the end of a chain.
	uint32_t fat_entry(const fat_layout& f, const std::vector<uint8_t>& fat, uint32_t cluster)
	{
		if (f.fat16)
		{
			const size_t offset = size_t(cluster) * 2;
			return offset + 1 < fat.size() ? get16(&fat[offset]) : 0xffff;
		}
		const size_t offset = cluster + cluster / 2;
		if (offset + 1 >= fat.size()) return 0xfff;
		const uint32_t value = get16(&fat[offset]);
		return cluster & 1 ? value >> 4 : value & 0xfff;
	}

	bool read_fat(disk_source& source, const fat_layout& f, std::vector<uint8_t>& fat, std::string& error)
	{
		fat.resize(size_t(f.sectors_per_fat) * sector);
		if (source.read(f.fat_start * sector, fat.data(), fat.size())) return true;
		error = "cannot read the FAT";
		return false;
	}

	// The data of a chain of clusters, or of the root directory for cluster 0.
	bool read_chain(disk_source& source, const fat_layout& f, const std::vector<uint8_t>& fat, uint32_t first,
	                std::vector<uint8_t>& data, std::string& error)
	{
		if (first == 0)
		{
			data.resize(size_t(f.root_entries) * 32);
			if (source.read(f.root_start * sector, data.data(), data.size())) return true;
			error = "cannot read the root directory";
			return false;
		}
		data.clear();
		const size_t cluster_bytes = size_t(f.sectors_per_cluster) * sector;
		const uint32_t end_of_chain = f.fat16 ? 0xfff8 : 0xff8;
		for (uint32_t cluster = first, count = 0; cluster < end_of_chain; cluster = fat_entry(f, fat, cluster), ++count)
		{
			if (cluster < 2 || cluster >= f.clusters + 2 || count > f.clusters)
			{
				error = "a broken cluster chain at cluster " + std::to_string(cluster);
				return false;
			}
			data.resize(data.size() + cluster_bytes);
			if (!source.read((f.data_start + uint64_t(cluster - 2) * f.sectors_per_cluster) * sector,
			                 data.data() + data.size() - cluster_bytes, cluster_bytes))
			{
				error = "cannot read cluster " + std::to_string(cluster);
				return false;
			}
		}
		return true;
	}

	std::string entry_name(const uint8_t* entry)
	{
		uint8_t name[8];
		std::memcpy(name, entry, sizeof(name));
		if (name[0] == 0x05) name[0] = 0xe5; // a name really starting with E5h, which marks a deleted entry
		std::string text = trimmed(name, sizeof(name));
		const std::string extension = trimmed(entry + 8, 3);
		return extension.empty() ? text : text + "." + extension;
	}

	bool live_entry(const uint8_t* entry)
	{
		return entry[0] != 0xe5 && entry[11] != 0x0f && !(entry[11] & 0x08);
	}

	bool list_directory(disk_source& source, const fat_layout& f, const std::vector<uint8_t>& fat, uint32_t first,
	                    const std::string& parent, int depth, std::vector<disk_entry>& entries, std::string& error)
	{
		if (depth > 32)
		{
			error = "directories nested too deep";
			return false;
		}
		std::vector<uint8_t> data;
		if (!read_chain(source, f, fat, first, data, error)) return false;
		for (size_t offset = 0; offset + 32 <= data.size() && data[offset] != 0; offset += 32)
		{
			const uint8_t* e = &data[offset];
			const std::string name = entry_name(e);
			if (!live_entry(e) || name == "." || name == "..") continue;
			disk_entry entry;
			entry.path = parent + "\\" + name;
			entry.size = get32(e + 28);
			entry.attributes = e[11];
			entry.time = uint16_t(get16(e + 22));
			entry.date = uint16_t(get16(e + 24));
			entries.push_back(entry);
			if ((e[11] & 0x10) && get16(e + 26) >= 2 &&
			    !list_directory(source, f, fat, get16(e + 26), entry.path, depth + 1, entries, error))
				return false;
		}
		return true;
	}

	bool open_volume(disk_source& source, const disk_volume& volume, fat_layout& f, std::vector<uint8_t>& fat,
	                 std::string& error)
	{
		uint8_t boot[sector];
		if (!source.read(volume.start * sector, boot, sector))
		{
			error = "cannot read the boot sector";
			return false;
		}
		if (!parse_boot_sector(boot, volume.start, f))
		{
			error = "not a FAT12 or FAT16 volume";
			return false;
		}
		return read_fat(source, f, fat, error);
	}

	// Fills in what the boot sector, the FAT and the root directory say.
	void describe_volume(disk_source& source, disk_volume& volume)
	{
		fat_layout f;
		std::vector<uint8_t> fat;
		std::vector<uint8_t> root;
		std::string error;
		if (!open_volume(source, volume, f, fat, error) || !read_chain(source, f, fat, 0, root, error)) return;
		volume.formatted = true;
		volume.fat16 = f.fat16;
		volume.sectors_per_cluster = f.sectors_per_cluster;
		volume.clusters = f.clusters;
		volume.free_clusters = 0;
		for (uint32_t cluster = 2; cluster < f.clusters + 2; ++cluster)
			if (fat_entry(f, fat, cluster) == 0) ++volume.free_clusters;
		for (size_t offset = 0; offset + 32 <= root.size() && root[offset] != 0; offset += 32)
		{
			const uint8_t* e = &root[offset];
			if (e[0] != 0xe5 && e[11] != 0x0f && (e[11] & 0x08))
			{
				volume.label = trimmed(e, 11);
				return;
			}
		}
		uint8_t boot[sector];
		if (source.read(volume.start * sector, boot, sector) && boot[38] == 0x29)
			volume.label = trimmed(boot + 43, 11);
		if (volume.label == "NO NAME") volume.label.clear();
	}

	void add_partition(disk_source& source, uint8_t type, uint64_t start, uint64_t sectors,
	                   std::vector<disk_volume>& volumes)
	{
		disk_volume volume;
		volume.start = start;
		volume.sectors = sectors;
		volume.type = type;
		describe_volume(source, volume);
		volumes.push_back(volume);
	}
}

bool disk_build_floppy(int kilobytes, const uint8_t* boot_sector, const std::vector<disk_file>& files,
                       const std::string& label, const disk_stamp& stamp, std::vector<uint8_t>& image,
                       disk_report& report, std::string& error)
{
	static const struct
	{
		int kilobytes;
		uint32_t sectors_per_track;
		uint32_t tracks;
		uint32_t sectors_per_cluster;
		uint32_t root_entries;
		uint32_t sectors_per_fat;
		uint8_t media;
	} formats[] = {
		{360, 9, 40, 2, 112, 2, 0xfd},
		{1440, 18, 80, 1, 224, 9, 0xf0},
	};
	const auto format = std::find_if(std::begin(formats), std::end(formats),
	                                 [&](const auto& f) { return f.kilobytes == kilobytes; });
	if (format == std::end(formats))
	{
		error = "a floppy is 360 or 1440 KB";
		return false;
	}
	const uint32_t sectors = format->sectors_per_track * 2 * format->tracks;
	vector_sink sink(image);
	sink.size(uint64_t(sectors) * sector);
	builder b{sink, stamp};
	begin_volume(b, 0, sectors, format->sectors_per_cluster, format->sectors_per_fat, format->root_entries,
	             format->media, false);
	if (!fill_volume(b, files, label, error) ||
	    !write_volume(b, boot_sector, 2, format->sectors_per_track, 0x00, error))
		return false;
	report = {};
	report.cylinders = format->tracks;
	report.heads = 2;
	report.sectors_per_track = format->sectors_per_track;
	report.sectors = sectors;
	report.volumes.push_back(b.info);
	return true;
}

bool disk_build_hard_disk(const disk_layout& layout, const uint8_t* master_boot, const uint8_t* boot_sector,
                          const std::vector<disk_file>& files, const std::string& label,
                          const disk_stamp& stamp, disk_sink& sink, disk_report& report, std::string& error)
{
	const uint32_t sectors_per_track = hard_disk_sectors_per_track;
	const uint32_t heads = layout.heads ? layout.heads : layout.megabytes <= 504 ? 16 : 255;
	const uint64_t cylinder = uint64_t(heads) * sectors_per_track;
	const uint64_t cylinders = uint64_t(layout.megabytes) * 2048 / cylinder;
	const uint64_t total = cylinders * cylinder;
	if (cylinders < 2)
	{
		error = "the disk is too small";
		return false;
	}
	if (total > 0xffffffff)
	{
		error = "the disk is too big: a partition table reaches 2 TB";
		return false;
	}
	if (!sink.size(total * sector))
	{
		error = "cannot size the image";
		return false;
	}
	const uint64_t largest = std::min<uint64_t>(uint64_t(layout.partition_megabytes) * 2048, largest_volume);

	report = {};
	report.cylinders = uint32_t(cylinders);
	report.heads = heads;
	report.sectors_per_track = sectors_per_track;
	report.sectors = total;

	uint8_t master[sector];
	uint8_t extended[sector];
	std::memcpy(master, master_boot, sector);
	builder b{sink, stamp};

	// Volume n takes [start, end): the sector with its partition table - the
	// MBR for C:, an EBR for a logical drive - the rest of that track, then
	// the volume, to a cylinder boundary.
	uint64_t start = 0;
	uint64_t end = std::min((sectors_per_track + largest) / cylinder * cylinder, total);
	uint64_t extended_start = 0;
	for (int n = 0; n < hard_disk_volumes && end > start + sectors_per_track; ++n)
	{
		const uint64_t size = end - start - sectors_per_track;
		const uint64_t next_end = std::min((end + sectors_per_track + largest) / cylinder * cylinder, total);
		const uint8_t type = partition_type(size, layout.lba_types);
		if (n == 1)
		{
			extended_start = start;
			put_partition(master + 0x1ce, false, layout.lba_types ? 0x0f : 0x05, start, start, total - start, heads,
			              sectors_per_track);
		}
		if (n > 0)
		{
			std::memset(extended, 0, sizeof(extended));
			extended[510] = 0x55;
			extended[511] = 0xaa;
			// The link to the next EBR, which starts relative to the extended partition.
			if (next_end > end + sectors_per_track && n + 1 < hard_disk_volumes)
				put_partition(extended + 0x1ce, false, 0x05, end, end - extended_start, next_end - end, heads,
				              sectors_per_track);
		}
		put_partition((n == 0 ? master : extended) + 0x1be, n == 0, type, start + sectors_per_track,
		              sectors_per_track, size, heads, sectors_per_track);
		if (n > 0 && !sink.write(start * sector, extended, sector))
		{
			error = "cannot write the partition table at sector " + std::to_string(start);
			return false;
		}

		begin_hard_disk_volume(b, start + sectors_per_track, size);
		b.info.type = type;
		const std::string volume_label = n == 0 ? label : std::string("DRIVE ") + char('C' + n);
		if (!fill_volume(b, n == 0 ? files : std::vector<disk_file>{}, volume_label, error) ||
		    !write_volume(b, boot_sector, heads, sectors_per_track, 0x80, error))
			return false;
		report.volumes.push_back(b.info);
		start = end;
		end = next_end;
	}
	if (!sink.write(0, master, sector))
	{
		error = "cannot write the master boot record";
		return false;
	}
	return true;
}

bool disk_volumes(disk_source& source, std::vector<disk_volume>& volumes, std::string& error)
{
	volumes.clear();
	uint8_t first[sector];
	if (!source.read(0, first, sector))
	{
		error = "cannot read the first sector";
		return false;
	}
	fat_layout f;
	if ((first[0] == 0xeb || first[0] == 0xe9) && parse_boot_sector(first, 0, f))
	{
		add_partition(source, 0, 0, f.sectors, volumes);
		return true;
	}
	if (first[510] != 0x55 || first[511] != 0xaa)
	{
		error = "neither a boot sector nor a partition table";
		return false;
	}

	uint64_t extended = 0;
	for (int i = 0; i < 4; ++i)
	{
		const uint8_t* entry = first + 0x1be + 16 * i;
		const uint8_t type = entry[4];
		if (type == 0 || get32(entry + 12) == 0) continue;
		if (type == 0x05 || type == 0x0f)
		{
			if (!extended) extended = get32(entry + 8);
			continue;
		}
		add_partition(source, type, get32(entry + 8), get32(entry + 12), volumes);
	}
	// Each EBR's first entry is a logical drive, starting relative to that
	// EBR; its second links the next EBR, relative to the extended partition.
	for (uint64_t table_start = extended, links = 0; table_start && links < 64; ++links)
	{
		uint8_t table[sector];
		if (!source.read(table_start * sector, table, sector) || table[510] != 0x55 || table[511] != 0xaa)
		{
			error = "a broken extended partition at sector " + std::to_string(table_start);
			return false;
		}
		const uint8_t* entry = table + 0x1be;
		if (entry[4] && get32(entry + 12))
			add_partition(source, entry[4], table_start + get32(entry + 8), get32(entry + 12), volumes);
		const uint8_t* next = table + 0x1ce;
		table_start = (next[4] == 0x05 || next[4] == 0x0f) && get32(next + 12) ? extended + get32(next + 8) : 0;
	}
	return true;
}

bool disk_list(disk_source& source, const disk_volume& volume, std::vector<disk_entry>& entries, std::string& error)
{
	entries.clear();
	fat_layout f;
	std::vector<uint8_t> fat;
	return open_volume(source, volume, f, fat, error) && list_directory(source, f, fat, 0, "", 0, entries, error);
}

bool disk_read_file(disk_source& source, const disk_volume& volume, const std::string& path,
                    std::vector<uint8_t>& data, std::string& error)
{
	fat_layout f;
	std::vector<uint8_t> fat;
	if (!open_volume(source, volume, f, fat, error)) return false;

	std::vector<std::string> names;
	for (size_t at = 0; at <= path.size();)
	{
		const size_t next = std::min(path.find_first_of("\\/", at), path.size());
		if (next > at) names.push_back(path.substr(at, next - at));
		at = next + 1;
	}
	if (names.empty())
	{
		error = "no file named";
		return false;
	}

	uint32_t directory = 0;
	for (size_t i = 0; i < names.size(); ++i)
	{
		uint8_t wanted[11];
		disk_name_83(names[i], wanted);
		std::vector<uint8_t> listing;
		if (!read_chain(source, f, fat, directory, listing, error)) return false;
		const uint8_t* found = nullptr;
		for (size_t offset = 0; !found && offset + 32 <= listing.size() && listing[offset] != 0; offset += 32)
		{
			const uint8_t* e = &listing[offset];
			if (live_entry(e) && std::equal(wanted, wanted + 11, e, [](uint8_t a, uint8_t b) { return a == upper(char(b)); }))
				found = e;
		}
		if (!found)
		{
			error = path + " not found";
			return false;
		}
		const bool is_directory = (found[11] & 0x10) != 0;
		if (i + 1 < names.size())
		{
			directory = get16(found + 26);
			if (!is_directory || directory < 2)
			{
				error = names[i] + " is not a directory";
				return false;
			}
			continue;
		}
		if (is_directory)
		{
			error = path + " is a directory";
			return false;
		}
		const uint32_t size = get32(found + 28);
		data.clear();
		if (size == 0) return true;
		if (!read_chain(source, f, fat, get16(found + 26), data, error)) return false;
		if (data.size() < size)
		{
			error = path + " is shorter than its directory entry says";
			return false;
		}
		data.resize(size);
	}
	return true;
}

void disk_name_83(const std::string& name, uint8_t out[11])
{
	std::memset(out, ' ', 11);
	const size_t slash = name.find_last_of("\\/");
	const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
	const size_t dot = base.rfind('.');
	const size_t stem = std::min<size_t>(dot == std::string::npos ? base.size() : dot, 8);
	for (size_t i = 0; i < stem; ++i) out[i] = upper(base[i]);
	if (dot != std::string::npos)
		for (size_t i = 0; i < 3 && dot + 1 + i < base.size(); ++i) out[8 + i] = upper(base[dot + 1 + i]);
}
