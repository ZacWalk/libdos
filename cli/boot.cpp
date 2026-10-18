// libdos by Zac Walker
//
// demu --boot: the machine in boot mode, headless. Disk images become drives,
// the power goes on, and a script of steps runs against the console - wait for
// some text, type some keys, reboot - as a person at the keyboard would, so a
// DOS can be tested booting and working from its own disks.

#include "cli.h"

#include "../host/host-win32.h"
#include "../src/machine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t ibm_pc_hz = 4772727;

	// A disk image file as a drive. With `snapshot` what is written stays in
	// memory and the file is left as it was.
	class image_drive final : public block_device
	{
	public:
		~image_drive() override
		{
			if (file_ != nullptr) fclose(file_);
		}

		bool open(const char* path, bool snapshot)
		{
			snapshot_ = snapshot;
			if (fopen_s(&file_, path, snapshot ? "rb" : "rb+") != 0 || file_ == nullptr) return false;
			if (_fseeki64(file_, 0, SEEK_END) != 0) return false;
			sectors_ = uint64_t(_ftelli64(file_)) / 512;
			return sectors_ > 0;
		}

		uint64_t sectors() override
		{
			return sectors_;
		}

		bool read(uint64_t lba, uint8_t* data, uint32_t count) override
		{
			for (uint32_t i = 0; i < count; ++i, ++lba, data += 512)
			{
				const auto written = changed_.find(lba);
				if (written != changed_.end())
				{
					memcpy(data, written->second.data(), 512);
					continue;
				}
				if (_fseeki64(file_, int64_t(lba * 512), SEEK_SET) != 0 || fread(data, 1, 512, file_) != 512)
					return false;
			}
			return true;
		}

		bool write(uint64_t lba, const uint8_t* data, uint32_t count) override
		{
			if (snapshot_)
			{
				for (uint32_t i = 0; i < count; ++i) changed_[lba + i].assign(data + i * 512, data + (i + 1) * 512);
				return true;
			}
			const size_t length = size_t(count) * 512;
			return _fseeki64(file_, int64_t(lba * 512), SEEK_SET) == 0 && fwrite(data, 1, length, file_) == length;
		}

	private:
		FILE* file_ = nullptr;
		uint64_t sectors_ = 0;
		bool snapshot_ = false;
		std::map<uint64_t, std::vector<uint8_t>> changed_;
	};

	// Keeps everything the console wrote, which the steps search.
	class boot_host final : public win32_host
	{
	public:
		std::string console;
		bool tracing = false;

		void video_text(uint8_t character) override
		{
			console.push_back(char(character));
		}

		void trace(const char* text) override
		{
			if (tracing) fprintf(stderr, "%s\n", text);
		}
	};

	// A key as the BIOS buffer holds it: the scancode and the character, 0
	// for a key that has none.
	struct keystroke
	{
		uint8_t scancode = 0;
		uint8_t ascii = 0;
	};

	struct step
	{
		enum class kind
		{
			expect,
			type,
			reboot,
		};

		kind what = kind::expect;
		std::string text; // what an expect waits for
		std::vector<keystroke> keys; // what a type step types
		double timeout = 0;
	};

	bool floppy_geometry(uint64_t sectors, disk_geometry& g)
	{
		static const struct
		{
			uint64_t sectors;
			disk_geometry geometry;
		} formats[] = {
			{320, {40, 1, 8}}, {360, {40, 1, 9}}, {640, {40, 2, 8}}, {720, {40, 2, 9}},
			{1440, {80, 2, 9}}, {2400, {80, 2, 15}}, {2880, {80, 2, 18}}, {5760, {80, 2, 36}},
		};
		for (const auto& format : formats)
		{
			if (format.sectors != sectors) continue;
			g = format.geometry;
			return true;
		}
		return false;
	}

	// The geometry a hard disk was partitioned with. FDISK and dimg end every
	// partition on a cylinder, so each one's last head and sector are the
	// disk's heads and sectors per track; a volume with no partition table
	// says in its boot sector. Anything else - a blank disk - gets dimg's
	// choice for its size: 63 sectors a track, 16 heads up to 504 MB, 255 above.
	disk_geometry hard_disk_geometry(block_device& disk, uint32_t heads, uint32_t sectors_per_track)
	{
		const uint64_t sectors = disk.sectors();
		uint32_t found_heads = 0;
		uint32_t found_sectors = 0;
		uint8_t first[512];
		if (disk.read(0, first, 1) && first[510] == 0x55 && first[511] == 0xaa)
		{
			bool table = true;
			for (int i = 0; i < 4 && table; ++i)
			{
				const uint8_t* entry = first + 0x1be + 16 * i;
				if (entry[4] == 0) continue;
				const uint32_t h = entry[5] + 1u;
				const uint32_t s = entry[6] & 0x3fu;
				table = (entry[0] == 0x00 || entry[0] == 0x80) && s != 0 &&
				        (found_heads == 0 || (h == found_heads && s == found_sectors));
				found_heads = h;
				found_sectors = s;
			}
			if (!table) found_heads = found_sectors = 0;
			if (found_heads == 0 && (first[0] == 0xeb || first[0] == 0xe9) && (first[11] | first[12] << 8) == 512)
			{
				found_sectors = first[24] | first[25] << 8;
				found_heads = first[26] | first[27] << 8;
			}
			if (found_heads == 0 || found_heads > 256 || found_sectors == 0 || found_sectors > 63)
				found_heads = found_sectors = 0;
		}
		disk_geometry g;
		g.heads = heads != 0 ? heads : found_heads != 0 ? found_heads : sectors <= 1024ull * 16 * 63 ? 16 : 255;
		g.sectors_per_track = sectors_per_track != 0 ? sectors_per_track : found_heads != 0 ? found_sectors : 63;
		const uint64_t cylinders = sectors / (uint64_t(g.heads) * g.sectors_per_track);
		g.cylinders = uint32_t(cylinders > 0 ? cylinders : 1);
		return g;
	}

	// The keys a US keyboard types a character with; false for one it has no key for.
	bool keystroke_for(char ch, keystroke& key)
	{
		key.ascii = uint8_t(ch);
		switch (ch)
		{
		case ' ': key.scancode = 0x39; return true;
		default: break;
		}
		static const char* const rows[] = {
			"1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "\\zxcvbnm,./",
			"!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"~", "|ZXCVBNM<>?",
		};
		static const uint8_t first[] = {0x02, 0x10, 0x1e, 0x2b};
		for (int row = 0; row < 8 && ch != 0; ++row)
		{
			const char* at = strchr(rows[row], ch);
			if (at == nullptr) continue;
			key.scancode = uint8_t(first[row % 4] + (at - rows[row]));
			return true;
		}
		return false;
	}

	bool keystrokes_for(const char* text, std::vector<keystroke>& keys)
	{
		for (const char* p = text; *p != 0; ++p)
		{
			keystroke key;
			if (!keystroke_for(*p, key)) return false;
			keys.push_back(key);
		}
		return true;
	}

	// --key's names: the keys that are not characters.
	bool named_key(const char* name, keystroke& key)
	{
		static const struct
		{
			const char* name;
			keystroke key;
		} names[] = {
			{"enter", {0x1c, 0x0d}}, {"esc", {0x01, 0x1b}}, {"tab", {0x0f, 0x09}}, {"backspace", {0x0e, 0x08}},
			{"up", {0x48, 0}}, {"down", {0x50, 0}}, {"left", {0x4b, 0}}, {"right", {0x4d, 0}},
			{"f1", {0x3b, 0}}, {"f2", {0x3c, 0}}, {"f3", {0x3d, 0}}, {"f4", {0x3e, 0}}, {"f5", {0x3f, 0}},
			{"f6", {0x40, 0}}, {"f7", {0x41, 0}}, {"f8", {0x42, 0}}, {"f9", {0x43, 0}}, {"f10", {0x44, 0}},
		};
		for (const auto& entry : names)
		{
			if (_stricmp(entry.name, name) != 0) continue;
			key = entry.key;
			return true;
		}
		return false;
	}

	bool keyboard_empty(machine_state& s)
	{
		return read16(s, bda_segment, bda_keyboard_head) == read16(s, bda_segment, bda_keyboard_tail);
	}

	void print_screen(const machine& m, FILE* out)
	{
		char text[80 * 50 + 64] = {};
		m.text_screen(text, int(sizeof(text)));
		fprintf(out, "%s", text);
	}

	struct drive_option
	{
		uint8_t number;
		const char* path;
	};
}

void boot_usage()
{
	printf(
		"Boot mode: an IBM PC with no DOS of its own, booting from disk images\n"
		"  demu --boot [drives] [options] [steps]\n\n"
		"  -a <image>, -b <image>  floppies in A: and B: (160 KB to 2.88 MB)\n"
		"  -c <image>, -d <image>  hard disks C: and D:\n"
		"  --from a|c        drive to boot (default A: if it has a floppy, else C:)\n"
		"  --chs <h>,<s>     hard disk heads (1-256) and sectors per track (1-63); by\n"
		"                    default the partition table's, else 16 heads up to\n"
		"                    504 MB and 255 above, with 63 sectors\n"
		"  --no-lba          no INT 13h extensions (AH=41h-48h) on the hard disks\n"
		"  --snapshot        keep what is written in memory; the images stay as they were\n"
		"  --cpu <model>     8086 (default: an IBM PC's 8088), 8086-strict, 186 or 386\n"
		"  --trace           report BIOS calls on stderr\n"
		"  --screen          print the screen at the end\n"
		"  --log <file>      write all the console's output to a file\n\n"
		"Steps, carried out in order:\n"
		"  --expect <text>   wait until the console writes <text>, after the last match\n"
		"  --type <text>     type <text> as it is\n"
		"  --line <text>     type <text> and press Enter\n"
		"  --key <name>      press enter, esc, tab, backspace, up, down, left, right\n"
		"                    or f1-f10\n"
		"  --reboot          power off and on again, keeping the disks\n"
		"  --timeout <s>     emulated seconds the --expect steps after it wait (default 120)\n\n"
		"Exits 0 when every step has happened, 1 if an --expect timed out and 2 if\n"
		"the machine stopped - an instruction an 8088 does not have, say.\n");
}

int run_boot(int argc, char** argv)
{
	std::vector<drive_option> drives;
	std::vector<step> steps;
	char from = 0;
	uint32_t heads = 0;
	uint32_t sectors_per_track = 0;
	bool lba = true;
	bool snapshot = false;
	bool trace = false;
	bool screen = false;
	const char* log_path = nullptr;
	cpu_model model = cpu_model::i8086;
	double timeout = 120;

	for (int i = 0; i < argc; ++i)
	{
		const char* arg = argv[i];
		const bool value = i + 1 < argc;
		if ((strcmp(arg, "-a") == 0 || strcmp(arg, "-b") == 0 || strcmp(arg, "-c") == 0 || strcmp(arg, "-d") == 0) &&
		    value)
		{
			static const uint8_t numbers[] = {0x00, 0x01, 0x80, 0x81};
			drives.push_back({numbers[arg[1] - 'a'], argv[++i]});
		}
		else if (strcmp(arg, "--from") == 0 && value)
			from = char(argv[++i][0] | 0x20);
		else if (strcmp(arg, "--chs") == 0 && value)
		{
			if (sscanf_s(argv[++i], "%u,%u", &heads, &sectors_per_track) != 2 || heads < 1 || heads > 256 ||
			    sectors_per_track < 1 || sectors_per_track > 63)
			{
				fprintf(stderr, "--chs wants heads (1-256) and sectors per track (1-63), as 16,63\n");
				return 2;
			}
		}
		else if (strcmp(arg, "--no-lba") == 0)
			lba = false;
		else if (strcmp(arg, "--snapshot") == 0)
			snapshot = true;
		else if (strcmp(arg, "--cpu") == 0 && value)
		{
			const char* name = argv[++i];
			if (strcmp(name, "8086") == 0) model = cpu_model::i8086;
			else if (strcmp(name, "8086-strict") == 0) model = cpu_model::i8086_strict;
			else if (strcmp(name, "186") == 0) model = cpu_model::i80186;
			else if (strcmp(name, "386") == 0) model = cpu_model::i386;
			else
			{
				fprintf(stderr, "unknown --cpu %s\n", name);
				return 2;
			}
		}
		else if (strcmp(arg, "--trace") == 0)
			trace = true;
		else if (strcmp(arg, "--screen") == 0)
			screen = true;
		else if (strcmp(arg, "--log") == 0 && value)
			log_path = argv[++i];
		else if (strcmp(arg, "--expect") == 0 && value)
		{
			step wait;
			wait.text = argv[++i];
			wait.timeout = timeout;
			steps.push_back(wait);
		}
		else if ((strcmp(arg, "--type") == 0 || strcmp(arg, "--line") == 0) && value)
		{
			step typing;
			typing.what = step::kind::type;
			if (!keystrokes_for(argv[++i], typing.keys))
			{
				fprintf(stderr, "%s \"%s\": a character with no key on a US keyboard\n", arg, argv[i]);
				return 2;
			}
			if (strcmp(arg, "--line") == 0) typing.keys.push_back({0x1c, 0x0d});
			steps.push_back(typing);
		}
		else if (strcmp(arg, "--key") == 0 && value)
		{
			step typing;
			typing.what = step::kind::type;
			keystroke key;
			if (!named_key(argv[++i], key))
			{
				fprintf(stderr, "--key %s: not a key name\n", argv[i]);
				return 2;
			}
			typing.keys.push_back(key);
			steps.push_back(typing);
		}
		else if (strcmp(arg, "--reboot") == 0)
		{
			step reboot;
			reboot.what = step::kind::reboot;
			reboot.timeout = timeout;
			steps.push_back(reboot);
		}
		else if (strcmp(arg, "--timeout") == 0 && value)
			timeout = atof(argv[++i]);
		else
		{
			fprintf(stderr, "unknown boot option %s\n\n", arg);
			boot_usage();
			return 2;
		}
	}

	boot_host host;
	host.tracing = trace;
	machine m;
	m.set_host(&host);
	m.set_cpu_model(model);
	m.set_render_enabled(false);
	machine_state& s = m.state();
	s.cpu_hz = ibm_pc_hz;
	s.trace = trace;

	std::vector<std::unique_ptr<image_drive>> images;
	bool floppy = false;
	for (const drive_option& option : drives)
	{
		auto image = std::make_unique<image_drive>();
		if (!image->open(option.path, snapshot))
		{
			fprintf(stderr, "cannot open the disk image %s\n", option.path);
			return 2;
		}
		const bool hard = (option.number & 0x80) != 0;
		disk_geometry geometry;
		if (hard)
			geometry = hard_disk_geometry(*image, heads, sectors_per_track);
		else if (!floppy_geometry(image->sectors(), geometry))
		{
			fprintf(stderr, "%s is not the size of a floppy\n", option.path);
			return 2;
		}
		floppy = floppy || !hard;
		m.attach_disk(option.number, image.get(), geometry, lba);
		images.push_back(std::move(image));
	}
	const uint8_t boot_drive = from == 'c' || (from == 0 && !floppy) ? 0x80 : 0x00;

	// The log is written however the run ends: it matters most when it fails.
	const auto finish = [&](int code) {
		if (log_path != nullptr)
		{
			FILE* log = nullptr;
			if (fopen_s(&log, log_path, "wb") != 0 || log == nullptr)
			{
				fprintf(stderr, "cannot write %s\n", log_path);
				return 2;
			}
			fwrite(host.console.data(), 1, host.console.size(), log);
			fclose(log);
		}
		return code;
	};
	const auto stopped = [&]() {
		fprintf(stderr, "demu: the machine stopped at %04X:%04X: %s\n", s.cpu.cs(), s.cpu.ip, m.error());
		print_screen(m, stderr);
		return finish(2);
	};
	if (!m.power_on(boot_drive)) return stopped();

	// Typed keys go into the BIOS buffer one at a time, each once the console
	// has been quiet for a moment, as a person types: a program that prints a
	// prompt and then flushes the keyboard - FORMAT does - would otherwise
	// throw away a key that arrived in between.
	std::vector<keystroke> keys;
	size_t typed = 0;
	size_t seen = 0;
	uint64_t quiet_since = 0;
	const uint64_t quiet = ibm_pc_hz / 20;
	const auto feed = [&]() {
		if (host.console.size() != seen)
		{
			seen = host.console.size();
			quiet_since = s.cycles;
			return;
		}
		if (typed >= keys.size() || !keyboard_empty(s) || s.cycles - quiet_since < quiet) return;
		const keystroke key = keys[typed++];
		bios_push_key(s, key.scancode, key.ascii);
		quiet_since = s.cycles;
	};
	const uint64_t slice = ibm_pc_hz / 100;

	// Runs until the program has read every key typed so far, for up to
	// `seconds` of emulated time; false if it has not.
	const auto drain = [&](double seconds) {
		const uint64_t limit = s.cycles + uint64_t(seconds * ibm_pc_hz);
		while ((typed < keys.size() || !keyboard_empty(s)) && s.cycles < limit && !m.faulted())
		{
			feed();
			m.exec(slice);
		}
		return typed >= keys.size() && keyboard_empty(s);
	};
	const auto unread = [&]() {
		fprintf(stderr, "demu: %zu typed keys were never read; the screen:\n",
		        keys.size() - typed + (keyboard_empty(s) ? 0 : 1));
		print_screen(m, stderr);
		return finish(1);
	};

	size_t matched = 0;
	for (const step& st : steps)
	{
		if (st.what == step::kind::type)
		{
			keys.insert(keys.end(), st.keys.begin(), st.keys.end());
			continue;
		}
		if (st.what == step::kind::reboot)
		{
			// What was typed before the restart is read before it.
			const bool read = drain(st.timeout);
			if (m.faulted()) return stopped();
			if (!read) return unread();
			if (!m.power_on(boot_drive)) return stopped();
			keys.clear();
			typed = 0;
			seen = host.console.size();
			quiet_since = s.cycles;
			continue;
		}
		const uint64_t limit = s.cycles + uint64_t(st.timeout * ibm_pc_hz);
		size_t found;
		while ((found = host.console.find(st.text, matched)) == std::string::npos)
		{
			if (m.faulted()) return stopped();
			if (s.cycles >= limit)
			{
				fprintf(stderr, "demu: no \"%s\" after %.0f emulated seconds; the screen:\n", st.text.c_str(),
				        st.timeout);
				print_screen(m, stderr);
				return finish(1);
			}
			feed();
			m.exec(slice);
		}
		// The text may have come in the slice that stopped the machine.
		if (m.faulted()) return stopped();
		matched = found + st.text.size();
	}

	// Whatever was typed last is read before the machine is left.
	const bool read = drain(timeout);
	if (m.faulted()) return stopped();
	if (!read) return unread();
	if (screen) print_screen(m, stdout);
	return finish(0);
}
