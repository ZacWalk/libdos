// ovrwin by Zac Walker
//
// Windows implementation of host_services. One directory is the emulated
// drive; a program cannot see or reach anything outside it.

#include "host-win32.h"

#include <cstdio>
#include <cstring>

namespace
{
	void to_dos_name(const WIN32_FIND_DATAA& found, char* out)
	{
		const char* name = found.cAlternateFileName[0] != 0 ? found.cAlternateFileName : found.cFileName;
		int length = 0;
		for (int i = 0; name[i] != 0 && length < 12; ++i)
			out[length++] = char(toupper(static_cast<unsigned char>(name[i])));
		out[length] = 0;
	}

	void to_dos_times(const FILETIME& time, uint16_t& date, uint16_t& stamp)
	{
		FILETIME local{};
		WORD dos_date = 0, dos_time = 0;
		FileTimeToLocalFileTime(&time, &local);
		FileTimeToDosDateTime(&local, &dos_date, &dos_time);
		date = dos_date;
		stamp = dos_time;
	}

	uint8_t to_dos_attributes(DWORD attributes)
	{
		uint8_t result = 0;
		if (attributes & FILE_ATTRIBUTE_READONLY) result |= 0x01;
		if (attributes & FILE_ATTRIBUTE_HIDDEN) result |= 0x02;
		if (attributes & FILE_ATTRIBUTE_SYSTEM) result |= 0x04;
		if (attributes & FILE_ATTRIBUTE_DIRECTORY) result |= 0x10;
		if (attributes & FILE_ATTRIBUTE_ARCHIVE) result |= 0x20;
		return result;
	}
}

win32_host::win32_host()
{
	for (auto& file : files_) file = INVALID_HANDLE_VALUE;
	GetCurrentDirectoryA(MAX_PATH, root_);
}

win32_host::~win32_host()
{
	for (auto& file : files_) if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
	for (auto& search : searches_) if (search.handle != INVALID_HANDLE_VALUE) FindClose(search.handle);
}

bool win32_host::set_root(const char* directory)
{
	char full[MAX_PATH] = {};
	if (GetFullPathNameA(directory, MAX_PATH, full, nullptr) == 0) return false;
	const DWORD attributes = GetFileAttributesA(full);
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;

	int length = int(strlen(full));
	while (length > 3 && (full[length - 1] == '\\' || full[length - 1] == '/')) full[--length] = 0;
	strcpy_s(root_, full);
	return true;
}

bool win32_host::to_windows_path(const char* dos_path, char* out, int out_size) const
{
	const char* rest = dos_path;
	if (rest[0] != 0 && rest[1] == ':') rest += 2;
	while (*rest == '\\' || *rest == '/') ++rest;

	// A path that would climb out of the root is rejected rather than clamped,
	// so nothing outside the mounted directory is ever reachable.
	int depth = 0;
	for (const char* p = rest; *p != 0;)
	{
		const char* end = p;
		while (*end != 0 && *end != '\\' && *end != '/') ++end;
		const size_t length = size_t(end - p);
		if (length == 2 && p[0] == '.' && p[1] == '.')
		{
			if (--depth < 0) return false;
		}
		else if (length > 0 && !(length == 1 && p[0] == '.'))
		{
			++depth;
		}
		p = *end == 0 ? end : end + 1;
	}

	if (rest[0] == 0) return snprintf(out, size_t(out_size), "%s", root_) < out_size;
	return snprintf(out, size_t(out_size), "%s\\%s", root_, rest) < out_size;
}

int win32_host::file_open(const char* path, int access)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(path, windows_path, MAX_PATH)) return -1;

	DWORD desired = GENERIC_READ;
	if (access == host_open_write) desired = GENERIC_WRITE;
	else if (access == host_open_read_write) desired = GENERIC_READ | GENERIC_WRITE;

	HANDLE handle = CreateFileA(windows_path, desired, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE && access != host_open_read)
	{
		// Some programs ask for write access to files that are read-only on
		// the host; fall back rather than failing the open.
		handle = CreateFileA(windows_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	}
	if (handle == INVALID_HANDLE_VALUE) return -1;

	for (int i = 0; i < max_files; ++i)
	{
		if (files_[i] == INVALID_HANDLE_VALUE)
		{
			files_[i] = handle;
			if (verbose_) printf("[host] open %s -> %d\n", path, i);
			return i;
		}
	}
	CloseHandle(handle);
	return -1;
}

int win32_host::file_create(const char* path, uint8_t attributes)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(path, windows_path, MAX_PATH)) return -1;

	const HANDLE handle = CreateFileA(windows_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
	                                  CREATE_ALWAYS,
	                                  (attributes & 0x01) ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return -1;

	for (int i = 0; i < max_files; ++i)
	{
		if (files_[i] == INVALID_HANDLE_VALUE)
		{
			files_[i] = handle;
			if (verbose_) printf("[host] create %s -> %d\n", path, i);
			return i;
		}
	}
	CloseHandle(handle);
	return -1;
}

void win32_host::file_close(int handle)
{
	if (handle < 0 || handle >= max_files || files_[handle] == INVALID_HANDLE_VALUE) return;
	CloseHandle(files_[handle]);
	files_[handle] = INVALID_HANDLE_VALUE;
}

int win32_host::file_read(int handle, uint8_t* buffer, int length)
{
	if (handle < 0 || handle >= max_files || files_[handle] == INVALID_HANDLE_VALUE) return -1;
	DWORD read = 0;
	if (!ReadFile(files_[handle], buffer, DWORD(length), &read, nullptr)) return -1;
	return int(read);
}

int win32_host::file_write(int handle, const uint8_t* buffer, int length)
{
	if (handle < 0 || handle >= max_files || files_[handle] == INVALID_HANDLE_VALUE) return -1;
	DWORD written = 0;
	if (!WriteFile(files_[handle], buffer, DWORD(length), &written, nullptr)) return -1;
	return int(written);
}

int32_t win32_host::file_seek(int handle, int32_t offset, int origin)
{
	if (handle < 0 || handle >= max_files || files_[handle] == INVALID_HANDLE_VALUE) return -1;
	const DWORD method = origin == 1 ? FILE_CURRENT : (origin == 2 ? FILE_END : FILE_BEGIN);
	const DWORD position = SetFilePointer(files_[handle], offset, nullptr, method);
	if (position == INVALID_SET_FILE_POINTER) return -1;
	return int32_t(position);
}

bool win32_host::file_delete(const char* path)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(path, windows_path, MAX_PATH)) return false;
	return DeleteFileA(windows_path) != 0;
}

bool win32_host::file_rename(const char* from, const char* to)
{
	char source[MAX_PATH] = {};
	char destination[MAX_PATH] = {};
	if (!to_windows_path(from, source, MAX_PATH) || !to_windows_path(to, destination, MAX_PATH)) return false;
	return MoveFileA(source, destination) != 0;
}

bool win32_host::file_info(const char* path, host_file_info& info)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(path, windows_path, MAX_PATH)) return false;

	WIN32_FIND_DATAA found{};
	const HANDLE handle = FindFirstFileA(windows_path, &found);
	if (handle == INVALID_HANDLE_VALUE) return false;
	FindClose(handle);

	to_dos_name(found, info.name);
	info.size = found.nFileSizeLow;
	info.attributes = to_dos_attributes(found.dwFileAttributes);
	info.directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	to_dos_times(found.ftLastWriteTime, info.dos_date, info.dos_time);
	return true;
}

bool win32_host::file_set_attributes(const char* path, uint8_t attributes)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(path, windows_path, MAX_PATH)) return false;
	DWORD flags = FILE_ATTRIBUTE_NORMAL;
	if (attributes & 0x01) flags = FILE_ATTRIBUTE_READONLY;
	return SetFileAttributesA(windows_path, flags) != 0;
}

int win32_host::find_first(const char* pattern, uint8_t attributes, host_file_info& info)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(pattern, windows_path, MAX_PATH)) return -1;

	int slot = -1;
	for (int i = 0; i < host_max_searches; ++i)
	{
		if (searches_[i].handle == INVALID_HANDLE_VALUE)
		{
			slot = i;
			break;
		}
	}
	if (slot < 0) return -1;

	WIN32_FIND_DATAA found{};
	const HANDLE handle = FindFirstFileA(windows_path, &found);
	if (handle == INVALID_HANDLE_VALUE) return -1;

	searches_[slot].handle = handle;
	searches_[slot].attributes = attributes;

	// The first match may be one the caller's attribute mask excludes.
	for (;;)
	{
		const uint8_t found_attributes = to_dos_attributes(found.dwFileAttributes);
		const bool wants_directories = (attributes & 0x10) != 0;
		if ((found_attributes & 0x10) == 0 || wants_directories)
		{
			to_dos_name(found, info.name);
			info.size = found.nFileSizeLow;
			info.attributes = found_attributes;
			info.directory = (found_attributes & 0x10) != 0;
			to_dos_times(found.ftLastWriteTime, info.dos_date, info.dos_time);
			return slot;
		}
		if (!FindNextFileA(handle, &found))
		{
			FindClose(handle);
			searches_[slot].handle = INVALID_HANDLE_VALUE;
			return -1;
		}
	}
}

bool win32_host::find_next(int search, host_file_info& info)
{
	if (search < 0 || search >= host_max_searches || searches_[search].handle == INVALID_HANDLE_VALUE) return false;

	WIN32_FIND_DATAA found{};
	while (FindNextFileA(searches_[search].handle, &found))
	{
		const uint8_t found_attributes = to_dos_attributes(found.dwFileAttributes);
		const bool wants_directories = (searches_[search].attributes & 0x10) != 0;
		if ((found_attributes & 0x10) != 0 && !wants_directories) continue;

		to_dos_name(found, info.name);
		info.size = found.nFileSizeLow;
		info.attributes = found_attributes;
		info.directory = (found_attributes & 0x10) != 0;
		to_dos_times(found.ftLastWriteTime, info.dos_date, info.dos_time);
		return true;
	}
	find_close(search);
	return false;
}

void win32_host::find_close(int search)
{
	if (search < 0 || search >= host_max_searches) return;
	if (searches_[search].handle != INVALID_HANDLE_VALUE) FindClose(searches_[search].handle);
	searches_[search].handle = INVALID_HANDLE_VALUE;
}

bool win32_host::make_directory(const char* path)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(path, windows_path, MAX_PATH)) return false;
	return CreateDirectoryA(windows_path, nullptr) != 0;
}

bool win32_host::remove_directory(const char* path)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(path, windows_path, MAX_PATH)) return false;
	return RemoveDirectoryA(windows_path) != 0;
}

bool win32_host::directory_exists(const char* path)
{
	char windows_path[MAX_PATH] = {};
	if (!to_windows_path(path, windows_path, MAX_PATH)) return false;
	const DWORD attributes = GetFileAttributesA(windows_path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

uint32_t win32_host::free_space()
{
	ULARGE_INTEGER available{};
	if (!GetDiskFreeSpaceExA(root_, &available, nullptr, nullptr)) return 0x02000000;
	return available.QuadPart > 0x7fffffff ? 0x7fffffff : uint32_t(available.QuadPart);
}

void win32_host::clock(int& year, int& month, int& day, int& weekday, int& hour, int& minute, int& second,
                       int& hundredths)
{
	SYSTEMTIME now{};
	GetLocalTime(&now);
	year = now.wYear;
	month = now.wMonth;
	day = now.wDay;
	weekday = now.wDayOfWeek;
	hour = now.wHour;
	minute = now.wMinute;
	second = now.wSecond;
	hundredths = now.wMilliseconds / 10;
}

void* win32_host::code_alloc(uint32_t size)
{
	if (size == 0) return nullptr;
	return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

void win32_host::code_free(void* code, uint32_t size)
{
	(void)size;
	if (code != nullptr) VirtualFree(code, 0, MEM_RELEASE);
}

// The page is writable until this is called and executable afterwards, so it is
// never both at once.
void win32_host::code_commit(void* code, uint32_t size)
{
	if (code == nullptr || size == 0) return;
	DWORD was = 0;
	if (VirtualProtect(code, size, PAGE_EXECUTE_READ, &was))
		FlushInstructionCache(GetCurrentProcess(), code, size);
}

void win32_host::trace(const char* text)
{
	if (verbose_) printf("[host] %s\n", text);
}
