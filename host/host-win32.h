#pragma once

// libdos by Zac Walker
//
// Windows implementation of host_services: a single directory is presented to
// the emulated program as its whole drive. Shared by dui and demu, which adds
// its terminal and its native programs on top.

#include "../src/host.h"

#include <memory>

constexpr int host_max_searches = 32;

class win32_host : public host_services
{
public:
	win32_host();
	~win32_host() override;
	win32_host(const win32_host&) = delete;
	win32_host& operator=(const win32_host&) = delete;

	// Everything the program sees lives under this directory.
	bool set_root(const char* directory);
	const char* root() const { return root_; }

	// Turns a DOS path into a Windows path under the root, without allowing
	// it to escape: the emulated program cannot reach the rest of the disk.
	bool to_windows_path(const char* dos_path, char* out, int out_size) const;

	void set_verbose(bool verbose) { verbose_ = verbose; }

	int file_open(const char* path, int access) override;
	int file_create(const char* path, uint8_t attributes) override;
	void file_close(int handle) override;
	int file_read(int handle, uint8_t* buffer, int length) override;
	int file_write(int handle, const uint8_t* buffer, int length) override;
	int32_t file_seek(int handle, int32_t offset, int origin) override;
	bool file_truncate(int handle) override;
	bool file_delete(const char* path) override;
	bool file_rename(const char* from, const char* to) override;
	bool file_copy(const char* from, const char* to) override;
	bool file_info(const char* path, host_file_info& info) override;
	bool file_set_attributes(const char* path, uint8_t attributes) override;

	int find_first(const char* pattern, uint8_t attributes, host_file_info& info) override;
	bool find_next(int search, host_file_info& info) override;
	void find_close(int search) override;

	bool make_directory(const char* path) override;
	bool remove_directory(const char* path) override;
	bool directory_exists(const char* path) override;
	uint32_t free_space() override;

	void clock(int& year, int& month, int& day, int& weekday, int& hour, int& minute, int& second,
	           int& hundredths) override;

	void* code_alloc(uint32_t size) override;
	void code_free(void* code, uint32_t size) override;
	void code_commit(void* code, uint32_t size) override;

	void trace(const char* text) override;

protected:
	// One line of the verbose log, through trace() so a subclass decides
	// where it goes.
	void report(const char* what, const char* path, int handle);
	bool verbose() const { return verbose_; }

private:
	struct state;
	std::unique_ptr<state> state_;
	static const int max_files = 64;
	static constexpr int path_capacity = 260;
	char root_[path_capacity] = {};
	bool verbose_ = false;
};
