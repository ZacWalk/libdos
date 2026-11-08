// dui by Zac Walker, built on libdesktop's four-library application layer
//
// Binds the emulator to the shared ui, render and platform services: the frame,
// menu, key mapping, audio stream that paces it and the frame loop. The
// emulator itself never sees a window, and nothing here names an operating system.

#include "libdesktop/core.h"
#include "libdesktop/platform.h"
#include "libdesktop/render.h"
#include "libdesktop/ui.h"
#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/menu_bar.h"
#include "libdesktop/ui/test_support.h"

// The host's handles stay in its implementation, shared with the CLI.
#include "../host/host-win32.h"

#include "../src/machine.h"
#include "../src/sound.h"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <thread>
#include <vector>

namespace
{
	constexpr int audio_blocks = 4;
	constexpr int samples_per_frame = sound_sample_rate / 60;

	class dui_app;
	class dui_frame_host;

	dui_app* g_application = nullptr;
	ui::frame_ptr g_frame;
	std::shared_ptr<dui_frame_host> g_reactor;
	std::shared_ptr<ui::elements> g_elements;
	std::unique_ptr<machine> g_emulator;
	std::unique_ptr<win32_host> g_host;
	platform::audio_stream_ptr g_audio;
	std::array<int16_t, samples_per_frame> g_audio_block = {};

	bool g_running = true;
	bool g_paused = false;
	bool g_sound_on = true;

	// What the machine has been told is held down. A key let go while another
	// window has the focus - a dialog, a shortcut such as Ctrl+O, or another
	// application - is released there, so without this it would stay down in
	// the machine and turn every later key into Ctrl+key.
	std::array<bool, 256> g_keys_down = {};
	std::array<char32_t, 256> g_key_sources = {};

	core::sizei g_client;
	core::pointi g_screen_origin;

	// The picture sits in the content element below the drawn menu bar, at a
	// whole-number scale. A fractional factor has to drop or double individual
	// rows of pixels, which is what makes an emulated screen look uneven.
	// Each axis gets its own factor, bounded so a pixel is at least square and
	// at most twice as tall as it is wide - roughly its shape on the display a
	// 200-line mode was written for.
	struct picture_layout
	{
		int x = 0, y = 0;
		int scale_x = 1, scale_y = 1;
		int width = 0, height = 0;

		bool operator==(const picture_layout&) const = default;
	};

	picture_layout g_layout;

	// The content surface is uploaded to a kept texture for each repaint, so a
	// paused or uncovered window is drawn from the same complete frame.
	core::surface_ptr g_surface;
	render::texture_ptr g_texture;

	// Menus are retained so accelerators and the menu-bar control use the same
	// commands. Child menus are built when opened, so checked state is current.
	std::vector<ui::command_ptr> g_menus;
	bool g_headless = false;
	std::string g_target;
	// Source column for each visible destination column, so scaling costs no
	// divide per pixel.
	std::vector<int> g_columns;

	// The legacy config file lived beside the executable but was created only
	// when a setting was written. Keep reads lazy so a first run stays clean.
	class lazy_ini_settings final : public platform::setting_file
	{
		core::folder_path _folder;
		core::file_path _path;
		mutable platform::setting_file_ptr _store;

		platform::setting_file_ptr read_store() const
		{
			if (!_store && platform::exists(_path)) _store = platform::create_ini_file_settings(_folder);
			return _store;
		}

		platform::setting_file_ptr write_store()
		{
			if (!_store) _store = platform::create_ini_file_settings(_folder);
			return _store;
		}

	public:
		explicit lazy_ini_settings(core::folder_path folder) : _folder(std::move(folder)),
			_path(_folder.combine_file(std::format("{}.ini", core::app_file_stem())))
		{
		}

		bool root_created() const override
		{
			return _store ? _store->root_created() : platform::exists(_path);
		}

		bool write(std::string_view section, std::string_view name, uint32_t value) override
		{
			const auto store = write_store();
			return store && store->write(section, name, value);
		}

		bool write(std::string_view section, std::string_view name, uint64_t value) override
		{
			const auto store = write_store();
			return store && store->write(section, name, value);
		}

		bool write(std::string_view section, std::string_view name, std::string_view value) override
		{
			const auto store = write_store();
			return store && store->write(section, name, value);
		}

		bool write(std::string_view section, std::string_view name, core::cspan value) override
		{
			const auto store = write_store();
			return store && store->write(section, name, value);
		}

		bool read(std::string_view section, std::string_view name, uint32_t& value) const override
		{
			const auto store = read_store();
			return store && store->read(section, name, value);
		}

		bool read(std::string_view section, std::string_view name, uint64_t& value) const override
		{
			const auto store = read_store();
			return store && store->read(section, name, value);
		}

		bool read(std::string_view section, std::string_view name, std::string& value) const override
		{
			const auto store = read_store();
			return store && store->read(section, name, value);
		}

		bool read(std::string_view section, std::string_view name, uint8_t* value, size_t& length) const override
		{
			const auto store = read_store();
			return store && store->read(section, name, value, length);
		}
	};

	picture_layout compute_layout();
	bool compose();
	void release_key(machine_key key);
	void release_all_keys();
	void report(std::string_view message);
	void update_title();
	void present();
	void open_path(const core::file_path& path);
	void toggle_pause();
	void toggle_sound();
	std::vector<ui::command_ptr> build_menus();

	class screen_element final : public ui::element
	{
	public:
		screen_element() noexcept
		{
			flex = ui::flex_item::grow | ui::flex_item::stretch;
			padding(0);
		}

		core::sizei measure(render::measure_context&, int) const override
		{
			return {};
		}

		void layout(render::measure_context& mc, const core::recti bounds_in) override;
		void render(render::draw_context& dc, core::pointi element_offset) const override;
	};

	class dui_frame_host final : public ui::frame_host
	{
		std::shared_ptr<ui::elements> _root;

	public:
		explicit dui_frame_host(std::shared_ptr<ui::elements> root) : _root(std::move(root))
		{
		}

		void on_window_layout(render::measure_context& mc, core::sizei extent, bool is_minimized) override;
		void on_window_paint(render::draw_context& dc) override;
		void on_window_destroy() override;

		void on_mouse_move(core::pointi loc, bool is_tracking) override;
		void on_mouse_left_button_down(core::pointi loc, platform::key_state keys) override;
		void on_mouse_left_button_up(core::pointi loc, platform::key_state keys) override;
		void on_mouse_middle_button_down(core::pointi loc, platform::key_state keys) override;
		void on_mouse_middle_button_up(core::pointi loc, platform::key_state keys) override;
		void on_mouse_right_button_down(core::pointi loc, platform::key_state keys) override;
		void on_mouse_right_button_up(core::pointi loc, platform::key_state keys) override;
		void focus_changed(bool has_focus, const ui::control_ptr& child) override;
		void activate(bool active) override;

		platform::drop_effect drag_over(const platform::clipboard_data& data, platform::key_state keys,
		                                core::pointi loc) override;
		platform::drop_effect drag_drop(platform::clipboard_data& data, platform::key_state keys,
		                                core::pointi loc) override;

		void visit_elements(const std::function<void(const ui::element_ptr&, core::pointi)>& visit) override
		{
			visit(_root, {});
		}

	private:
		bool screen_point(core::pointi loc, int& x, int& y) const;
	};

	class dui_app final : public ui::app, public std::enable_shared_from_this<dui_app>
	{
		platform::setting_file_ptr _settings;

	public:
		dui_app()
		{
			g_application = this;
		}

		~dui_app() override
		{
			if (g_application == this) g_application = nullptr;
		}

		bool pre_init() override { return true; }
		bool parse_command_line(std::string_view command_line) override;
		int run_console_mode() override { return 0; }
		bool load_settings(const platform::setting_file_ptr& store) override;
		bool init(std::string_view command_line) override;
		bool owns_loop() const override { return true; }
		int main_loop() override;
		void idle() override {}
		bool can_exit() override { return true; }
		void exit() override;
		void final_exit() override {}
		void system_event(platform::os_event_type event) override;

		bool wants_gpu() const override { return true; }
		bool key_down(char32_t key, platform::key_state keys) override;
		bool text_input(std::string_view) override { return false; }
		ui::focus_mode focus_mode() const override { return ui::focus_mode::none; }
		void track_menu(const ui::frame_ptr& parent, core::recti button_bounds,
		                const std::vector<ui::command_ptr>& commands) override
		{
			parent->track_menu(button_bounds, commands);
		}

	private:
		static machine_key translate_key(char32_t key);
		static std::vector<std::string> command_line_args(std::string_view command_line);
		int run();
		int run_offscreen();
		void release_stale_keys();
	};

	std::string unquote(std::string_view value)
	{
		if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
		                          (value.front() == '\'' && value.back() == '\'')))
		{
			value.remove_prefix(1);
			value.remove_suffix(1);
		}
		return std::string(value);
	}

	std::vector<std::string> dui_app::command_line_args(const std::string_view command_line)
	{
		std::vector<std::string> result;
		for (const auto part : core::split(command_line, true)) result.emplace_back(unquote(part));
		return result;
	}

	picture_layout compute_layout()
	{
		picture_layout layout;
		if (!g_emulator) return layout;

		layout.width = g_emulator->width();
		layout.height = g_emulator->height();
		if (layout.width <= 0 || layout.height <= 0 || g_client.cx <= 0 || g_client.cy <= 0)
			return layout;

		layout.scale_x = std::max(1, g_client.cx / layout.width);
		layout.scale_y = std::max(1, g_client.cy / layout.height);
		while (layout.scale_x > layout.scale_y && layout.scale_x > 1) --layout.scale_x;
		if (layout.scale_y > layout.scale_x * 2) layout.scale_y = layout.scale_x * 2;

		layout.x = (g_client.cx - layout.width * layout.scale_x) / 2;
		layout.y = (g_client.cy - layout.height * layout.scale_y) / 2;
		return layout;
	}

	// Rebuilt only when the layout changes: the letterbox is cleared once and
	// then left alone, because the picture covers the rest of every frame.
	void rebuild_surface(const picture_layout& layout)
	{
		if (g_client.cx <= 0 || g_client.cy <= 0) return;
		if (!g_surface) g_surface = std::make_shared<core::surface>();
		g_surface->alloc(g_client, core::texture_format::RGB);
		std::memset(g_surface->pixels(), 0, g_surface->size());
		g_texture.reset();

		const int first = std::max(0, layout.x);
		const int last = std::min(g_client.cx, layout.x + layout.width * layout.scale_x);

		g_columns.clear();
		for (int dx = first; dx < last; ++dx)
			g_columns.push_back((dx - layout.x) / layout.scale_x);
	}

	// Draws the current frame into the surface. False when there is nothing to
	// show yet.
	bool compose()
	{
		if (!g_emulator || g_client.cx <= 0 || g_client.cy <= 0) return false;

		const auto layout = compute_layout();
		if (layout.width <= 0 || layout.height <= 0) return false;

		if (layout != g_layout || !g_surface || g_surface->dimensions() != g_client)
		{
			g_layout = layout;
			rebuild_surface(layout);
		}

		const uint32_t* source = g_emulator->framebuffer();
		if (source == nullptr || g_columns.empty()) return false;

		const int first_x = std::max(0, layout.x);
		const int first_y = std::max(0, layout.y);
		const int last_y = std::min(g_client.cy, layout.y + layout.height * layout.scale_y);

		for (int dy = first_y; dy < last_y; ++dy)
		{
			const uint32_t* row = source +
				static_cast<size_t>((dy - layout.y) / layout.scale_y) * layout.width;
			uint32_t* out = reinterpret_cast<uint32_t*>(g_surface->pixels_line(dy)) + first_x;
			for (const int sx : g_columns) *out++ = row[sx];
		}
		return true;
	}

	void screen_element::layout(render::measure_context& mc, const core::recti bounds_in)
	{
		element::layout(mc, bounds_in);
		const auto extent = bounds_in.extent();
		if (g_client != extent || g_screen_origin != bounds_in.top_left())
		{
			g_client = extent;
			g_screen_origin = bounds_in.top_left();
			g_layout = {};
			g_texture.reset();
		}
	}

	void screen_element::render(render::draw_context& dc, const core::pointi element_offset) const
	{
		if (!g_surface || g_surface->empty()) return;

		const auto dst = bounds.offset(element_offset);
		if (!g_texture || g_texture->dimensions() != g_surface->dimensions()) g_texture = dc.create_texture();
		if (g_texture)
		{
			const auto updated = g_texture->update(g_surface);
			if (updated != render::texture_update_result::failed)
			{
				dc.draw_texture(g_texture, dst, 1.0f, render::texture_sampler::point);
				return;
			}
			g_texture.reset();
		}

		// A software context can still present the CPU surface when a device
		// refuses a texture, which keeps the offscreen check meaningful.
		render::draw_surface(dc, g_surface, dst, 1.0f, render::texture_sampler::point);
	}

	// Through the same letterbox and scale the picture is drawn
	// with, so the pointer lands where the user sees it.
	bool dui_frame_host::screen_point(const core::pointi loc, int& x, int& y) const
	{
		if (g_client.is_empty()) return false;
		const auto screen = core::recti(g_screen_origin, g_client);
		const auto inside = screen.contains(loc);
		const auto local_x = loc.x - g_screen_origin.x;
		const auto local_y = loc.y - g_screen_origin.y;
		x = std::clamp((local_x - g_layout.x) / std::max(1, g_layout.scale_x), 0,
		               std::max(0, g_layout.width - 1));
		y = std::clamp((local_y - g_layout.y) / std::max(1, g_layout.scale_y), 0,
		               std::max(0, g_layout.height - 1));
		return inside;
	}

	void dui_frame_host::on_window_layout(render::measure_context& mc, const core::sizei extent,
	                                      const bool is_minimized)
	{
		if (!is_minimized) _root->layout(mc, core::recti(extent));
	}

	void dui_frame_host::on_window_paint(render::draw_context& dc)
	{
		compose();
		_root->render(dc, {});
	}

	void dui_frame_host::on_window_destroy()
	{
		release_all_keys();
		g_running = false;
	}

	void dui_frame_host::on_mouse_move(const core::pointi loc, bool)
	{
		if (!g_emulator) return;
		int x = 0, y = 0;
		screen_point(loc, x, y);
		g_emulator->mouse_move(x, y);
	}

	void dui_frame_host::on_mouse_left_button_down(const core::pointi loc, platform::key_state)
	{
		int x = 0, y = 0;
		if (screen_point(loc, x, y) && g_emulator) g_emulator->mouse_button(0, true);
	}

	void dui_frame_host::on_mouse_left_button_up(const core::pointi, platform::key_state)
	{
		if (g_emulator) g_emulator->mouse_button(0, false);
	}

	void dui_frame_host::on_mouse_middle_button_down(const core::pointi loc, platform::key_state)
	{
		int x = 0, y = 0;
		if (screen_point(loc, x, y) && g_emulator) g_emulator->mouse_button(2, true);
	}

	void dui_frame_host::on_mouse_middle_button_up(const core::pointi, platform::key_state)
	{
		if (g_emulator) g_emulator->mouse_button(2, false);
	}

	void dui_frame_host::on_mouse_right_button_down(const core::pointi loc, platform::key_state)
	{
		int x = 0, y = 0;
		if (screen_point(loc, x, y) && g_emulator) g_emulator->mouse_button(1, true);
	}

	void dui_frame_host::on_mouse_right_button_up(const core::pointi, platform::key_state)
	{
		if (g_emulator) g_emulator->mouse_button(1, false);
	}

	void dui_frame_host::focus_changed(const bool has_focus, const ui::control_ptr&)
	{
		if (!has_focus) release_all_keys();
	}

	void dui_frame_host::activate(const bool active)
	{
		if (!active) release_all_keys();
	}

	platform::drop_effect dui_frame_host::drag_over(const platform::clipboard_data& data, platform::key_state,
	                                                core::pointi)
	{
		return data.has_drop_files() ? platform::drop_effect::link : platform::drop_effect::none;
	}

	platform::drop_effect dui_frame_host::drag_drop(platform::clipboard_data& data, platform::key_state,
	                                                core::pointi)
	{
		if (!data.has_drop_files()) return platform::drop_effect::none;
		open_path(data.first_path());
		return platform::drop_effect::link;
	}

	void start_audio()
	{
		if (g_audio || !g_emulator || !platform::sound_init()) return;

		g_audio = platform::create_audio_stream(sound_sample_rate, 1, audio_blocks);
		if (g_audio) g_emulator->set_audio_enabled(true);
	}

	void stop_audio()
	{
		if (g_audio)
		{
			g_audio->stop();
			g_audio.reset();
		}
		if (g_emulator) g_emulator->set_audio_enabled(false);
	}

	// Submits one frame of audio. The device consumes at a fixed rate, so
	// waiting for room in the queue is what paces emulation to real time.
	void submit_audio()
	{
		if (!g_audio || !g_audio->can_write() || !g_emulator) return;
		g_emulator->generate_audio(g_audio_block.data(), samples_per_frame);
		g_audio->write(g_audio_block);
	}

	void report(const std::string_view message)
	{
		platform::message_box(nullptr, message, "dui",
		                      {platform::message_box_buttons::ok, platform::message_box_icon::warning});
	}

	// Mounts a folder as the emulated drive and brings the shell up in it.
	bool mount(const core::folder_path& directory)
	{
		if (!g_host) return false;
		const auto absolute = platform::canonical_path(directory);
		const auto path = std::string(absolute.text().sv());
		if (!g_host->set_root(path.c_str()))
		{
			report("Cannot use that folder as the emulated drive.");
			return false;
		}

		stop_audio();
		g_emulator->start_shell();
		if (g_sound_on) start_audio();
		g_paused = false;
		update_title();
		return true;
	}

	// Accepts either a program or a folder, which is what a drop can be.
	void open_path(const core::file_path& path)
	{
		if (platform::is_directory(path))
		{
			// file_path treats a directory without a trailing separator as a
			// final component; rebuild the complete spelling to avoid mounting
			// its parent.
			mount(core::folder_path(path.str()));
			return;
		}

		if (!platform::exists(path))
		{
			report("Cannot open that path.");
			return;
		}

		const auto folder = path.folder();
		if (!mount(folder.is_empty() ? platform::current_directory() : folder)) return;
		const auto program = std::string(path.name().sv());
		if (!g_emulator->run_program(program.c_str(), "")) report("That file is not a DOS program.");
		update_title();
	}

	void open_program()
	{
		const std::vector<platform::file_dialog_filter> filters = {
			{"DOS programs", "*.exe;*.com"},
			{"All files", "*.*"},
		};
		std::vector<core::file_path> paths;
		if (platform::prompt_for_open_paths(nullptr, paths, filters, false) && !paths.empty()) open_path(paths.front());
	}

	void open_folder()
	{
		core::folder_path folder;
		if (platform::browse_for_folder(nullptr, folder)) mount(folder);
	}

	void toggle_pause()
	{
		g_paused = !g_paused;
		update_title();
	}

	void toggle_sound()
	{
		g_sound_on = !g_sound_on;
		if (g_sound_on) start_audio();
		else stop_audio();
	}

	ui::command_ptr make_command(std::string_view text, std::function<void()> invoke = {},
	                             char32_t key = 0, uint32_t key_state = 0, bool checkable = false,
	                             bool checked = false)
	{
		auto command = std::make_shared<ui::command>();
		command->text = text;
		command->invoke = std::move(invoke);
		command->checkable = checkable;
		command->checked = checked;
		if (key != 0)
		{
			command->kba.push_back({key, key_state});
			command->keyboard_accelerator_text = platform::format_keyboard_accelerator(command->kba);
		}
		return command;
	}

	std::vector<ui::command_ptr> build_menus()
	{
		auto file = make_command("&File");
		file->menu = []
		{
			std::vector<ui::command_ptr> children;
			children.push_back(make_command("&Open Program...", open_program, 'O',
			                               platform::keyboard_accelerator_t::control));
			children.push_back(make_command("Open &Folder...", open_folder));
			children.push_back(std::make_shared<ui::command>());
			children.push_back(make_command("E&xit", [] { if (g_frame) g_frame->close(); }));
			return children;
		};

		auto machine_menu = make_command("&Machine");
		machine_menu->menu = []
		{
			std::vector<ui::command_ptr> children;
			children.push_back(make_command("&Reset", []
			{
				if (!g_emulator) return;
				// Back to a prompt the user can see and type at, as opening a
				// folder does: a paused machine would keep showing the old
				// picture under a title that says command prompt.
				g_emulator->start_shell();
				g_paused = false;
				update_title();
			}, 'R', platform::keyboard_accelerator_t::control));
			children.push_back(make_command("&Pause", toggle_pause, platform::keys::F9, 0, true, g_paused));
			return children;
		};

		auto view = make_command("&View");
		view->menu = []
		{
			return std::vector<ui::command_ptr>{
				make_command("&Sound", toggle_sound, 0, 0, true, g_sound_on),
			};
		};

		auto help = make_command("&Help");
		help->menu = []
		{
			return std::vector<ui::command_ptr>{make_command("&About dui", []
			{
				platform::message_box(nullptr,
				                      "dui\n\nA DOS program emulator built on libdos: 8086/80186, VGA/EGA/CGA, AdLib and "
				                      "PC speaker, with DOS and BIOS services mapped rather than emulated.\n\n"
				                      "https://github.com/ZacWalk/libdos",
				                      "About dui", {platform::message_box_buttons::ok,
				                                     platform::message_box_icon::none});
			})};
		};

		return {std::move(file), std::move(machine_menu), std::move(view), std::move(help)};
	}

	void update_title()
	{
		if (!g_frame || !g_emulator) return;
		const machine_state& s = g_emulator->state();
		const char* what = "no program";
		if (s.shell.program[0] != 0) what = s.shell.program;
		else if (s.shell.active) what = "command prompt";

		g_frame->window_text(std::format("dui - {} - mode {:02X}h {}x{}{}", what, s.vga.mode,
		                                s.vga.width, s.vga.height, g_paused ? " - paused" : ""));
	}

	void present()
	{
		if (g_frame && compose()) g_frame->redraw_now();
	}

	machine_key dui_app::translate_key(const char32_t key)
	{
		if (key == platform::keys::ESCAPE) return machine_key::escape;
		if (key == platform::keys::F1) return machine_key::f1;
		if (key == platform::keys::F2) return machine_key::f2;
		if (key == platform::keys::F3) return machine_key::f3;
		if (key == platform::keys::F4) return machine_key::f4;
		if (key == platform::keys::F5) return machine_key::f5;
		if (key == platform::keys::F6) return machine_key::f6;
		if (key == platform::keys::F7) return machine_key::f7;
		if (key == platform::keys::F8) return machine_key::f8;
		if (key == platform::keys::F9) return machine_key::f9;
		if (key == platform::keys::F10) return machine_key::f10;
		if (key == platform::keys::F11) return machine_key::f11;
		if (key == platform::keys::F12) return machine_key::f12;
		if (key == platform::keys::BACK) return machine_key::backspace;
		if (key == platform::keys::TAB) return machine_key::tab;
		if (key == platform::keys::RETURN) return machine_key::enter;
		if (key == platform::keys::SHIFT || key == platform::keys::LSHIFT || key == platform::keys::RSHIFT)
			return machine_key::left_shift;
		if (key == platform::keys::CONTROL || key == platform::keys::LCONTROL || key == platform::keys::RCONTROL)
			return machine_key::left_ctrl;
		if (key == platform::keys::ALT || key == platform::keys::LALT || key == platform::keys::RALT)
			return machine_key::left_alt;
		if (key == platform::keys::CAPITAL) return machine_key::caps_lock;
		if (key == platform::keys::SPACE) return machine_key::space;
		if (key == platform::keys::PRIOR) return machine_key::page_up;
		if (key == platform::keys::NEXT) return machine_key::page_down;
		if (key == platform::keys::END) return machine_key::end;
		if (key == platform::keys::HOME) return machine_key::home;
		if (key == platform::keys::LEFT) return machine_key::left;
		if (key == platform::keys::UP) return machine_key::up;
		if (key == platform::keys::RIGHT) return machine_key::right;
		if (key == platform::keys::DOWN) return machine_key::down;
		if (key == platform::keys::INSERT) return machine_key::insert;
		if (key == platform::keys::DEL) return machine_key::del;
		if (key == platform::keys::NUMLOCK) return machine_key::num_lock;
		if (key == platform::keys::SCROLL) return machine_key::scroll_lock;
		if (key == platform::keys::OEM_MINUS) return machine_key::minus;
		if (key == platform::keys::OEM_PLUS) return machine_key::equals;
		if (key == platform::keys::OEM_1) return machine_key::semicolon;
		if (key == platform::keys::OEM_2) return machine_key::slash;
		if (key == platform::keys::OEM_3) return machine_key::backtick;
		if (key == platform::keys::OEM_4) return machine_key::left_bracket;
		if (key == platform::keys::OEM_5) return machine_key::backslash;
		if (key == platform::keys::OEM_6) return machine_key::right_bracket;
		if (key == platform::keys::OEM_7) return machine_key::quote;
		if (key == platform::keys::OEM_COMMA) return machine_key::comma;
		if (key == platform::keys::OEM_PERIOD) return machine_key::period;
		if (key == platform::keys::MULTIPLY) return machine_key::keypad_star;
		if (key == platform::keys::ADD) return machine_key::keypad_plus;
		if (key == platform::keys::SUBTRACT) return machine_key::keypad_minus;
		if (key == platform::keys::DIVIDE) return machine_key::keypad_slash;
		if (key == platform::keys::DECIMAL) return machine_key::keypad_period;

		// The digit row runs 1..9 then 0, which is the order of its scancodes.
		if (key >= '0' && key <= '9')
			return machine_key(int(machine_key::num1) + ((key - '0' + 9) % 10));

		if (key >= 'A' && key <= 'Z')
		{
			static constexpr machine_key letters[26] = {
				machine_key::a, machine_key::b, machine_key::c, machine_key::d, machine_key::e, machine_key::f,
				machine_key::g, machine_key::h, machine_key::i, machine_key::j, machine_key::k, machine_key::l,
				machine_key::m, machine_key::n, machine_key::o, machine_key::p, machine_key::q, machine_key::r,
				machine_key::s, machine_key::t, machine_key::u, machine_key::v, machine_key::w, machine_key::x,
				machine_key::y, machine_key::z,
			};
			return letters[key - 'A'];
		}

		if (key >= platform::keys::numpad0 && key <= platform::keys::numpad9)
			return machine_key(int(machine_key::keypad0) + int(key - platform::keys::numpad0));

		return machine_key::none;
	}

	void press_key(const machine_key key, const char32_t source)
	{
		if (!g_emulator) return;
		g_emulator->key_down(key);
		if (key != machine_key::none)
		{
			g_keys_down[size_t(key)] = true;
			g_key_sources[size_t(key)] = source;
		}
	}

	void release_key(const machine_key key)
	{
		if (!g_emulator || key == machine_key::none) return;
		g_emulator->key_up(key);
		g_keys_down[size_t(key)] = false;
		g_key_sources[size_t(key)] = 0;
	}

	// The window no longer hears about the keys it has, so it lets go of them.
	// A focus change can also come while the window is being torn down.
	void release_all_keys()
	{
		if (!g_emulator) return;
		for (size_t i = 0; i < g_keys_down.size(); ++i)
			if (g_keys_down[i]) release_key(machine_key(i));
	}

	void dui_app::release_stale_keys()
	{
		for (size_t i = 0; i < g_keys_down.size(); ++i)
		{
			const auto source = g_key_sources[i];
			if (g_keys_down[i] && source != 0 && !platform::is_key_down_async(source))
				release_key(machine_key(i));
		}
	}


	bool dui_app::parse_command_line(const std::string_view command_line)
	{
		g_headless = false;
		g_target.clear();
		for (const auto& arg : command_line_args(command_line))
		{
			if (core::icmp(arg, "-test") == 0 || core::icmp(arg, "/test") == 0)
				g_headless = true;
			else if (g_target.empty())
				g_target = arg;
		}

		// -test is an offscreen GUI run, not platform::run's console branch:
		// init still has to build the frame and exercise its paint path.
		return false;
	}

	bool dui_app::load_settings(const platform::setting_file_ptr& store)
	{
		(void)store;
		_settings = std::make_shared<lazy_ini_settings>(
			platform::known_path(platform::known_folder::running_app_folder));
		return true;
	}

	core::file_path command_line_path(const std::string_view value)
	{
		core::file_path path(value);
		if (!core::is_path(value) && value.find_first_of("\\/") == std::string_view::npos)
		{
			const auto in_current = platform::current_directory().combine_file(value);
			if (!platform::is_directory(path) && platform::exists(in_current)) path = in_current;
		}
		return path;
	}

	void position_default_window(const platform::setting_file_ptr& settings)
	{
		if (!g_frame || platform::read_placement(settings, platform::placement_setting_name())) return;
		// The initial client size is a logical 1280x800 design size. Scale it
		// after the frame knows the monitor DPI, then centre the resulting outer
		// rectangle without disturbing a saved placement.
		constexpr core::sizei design_client{1280, 800};
		const auto scale = g_frame->scale_factor();
		const auto current = g_frame->window_bounds();
		const core::sizei chrome{
			std::max(0, current.width() - design_client.cx),
			std::max(0, current.height() - design_client.cy),
		};
		const core::sizei client{core::round(design_client.cx * scale), core::round(design_client.cy * scale)};
		const auto outer = client + chrome;
		const auto screen = platform::primary_screen_size();
		const int left = std::max(0, (screen.cx - outer.cx) / 2);
		const int top = std::max(0, (screen.cy - outer.cy) / 2);
		g_frame->window_bounds({left, top, left + outer.cx, top + outer.cy}, false);
	}

	bool dui_app::init(const std::string_view command_line)
	{
		parse_command_line(command_line);
		g_running = true;
		g_paused = false;
		g_sound_on = !g_headless;
		g_client = {};
		g_screen_origin = {};
		g_layout = {};
		g_surface.reset();
		g_texture.reset();
		g_keys_down.fill(false);
		g_key_sources.fill(0);

		g_emulator = std::make_unique<machine>();
		g_host = std::make_unique<win32_host>();
		g_emulator->set_host(g_host.get());

		g_menus = build_menus();
		auto menu_bar = ui::make_menu_bar(g_menus);
		menu_bar->flex = ui::flex_item::stretch;
		auto screen = std::make_shared<screen_element>();
		g_elements = std::make_shared<ui::elements>(std::vector<ui::element_ptr>{menu_bar, screen});
		g_elements->padding(0);
		g_elements->flex_container.direction = ui::flex_direction::column;
		g_elements->flex_container.wrap = ui::flex_wrap::no_wrap;
		g_elements->flex_container.align_items = ui::flex_align::stretch;

		g_reactor = std::make_shared<dui_frame_host>(g_elements);
		ui::frame_style style;
		style.hardware_accelerated = true;
		style.drawn_caption = false;
		style.client_size = {1280, 800};
		style.offscreen = g_headless;
		style.can_focus = true;
		style.can_drop = true;
		style.resizable = true;
		g_frame = ui::create_app_frame(shared_from_this(), _settings, g_reactor, style);
		if (!g_frame) return false;
		position_default_window(_settings);
		g_frame->window_text("dui");

		// A self-check is over in a moment; opening the sound device would only add
		// a second of silence to it.
		if (g_headless)
		{
			platform::attach_console();
			g_sound_on = false;
		}

		// A program or a folder on the command line, or the current directory so
		// the user can look around from the prompt before starting anything - which
		// is also where one that cannot be opened leaves them, rather than at a
		// blank window that ignores the keyboard.
		if (g_target.empty())
		{
			mount(platform::current_directory());
		}
		else
		{
			open_path(command_line_path(g_target));
			if (g_host->root()[0] == 0) mount(platform::current_directory());
		}
		return true;
	}

	int dui_app::run()
	{
		constexpr double period = 1.0 / 60.0;
		double last = platform::monotonic_seconds();
		int title_counter = 0;

		while (g_running)
		{
			if (!platform::pump_events()) break;
			if (!g_running || !g_emulator) break;
			release_stale_keys();

			// EXIT at the prompt is the only thing that ends the session; a
			// program terminating just returns to the shell.
			if (g_emulator->exited())
			{
				if (g_frame) g_frame->close();
				continue;
			}

			if (g_paused)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
				continue;
			}

			// With sound on, waiting for room in the audio queue paces emulation
			// to exactly real time; without it, the wall clock does.
			if (g_audio)
			{
				if (!g_audio->can_write())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
			}
			else
			{
				// A fixed step, not "one frame since we last finished": resetting
				// the reference to the actual time accumulates sleep granularity.
				const double now = platform::monotonic_seconds();
				if (now - last < period)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				last += period;
				if (now - last > period * 4) last = now;
			}

			g_emulator->exec(g_emulator->cycles_per_frame());
			submit_audio();
			present();

			if (++title_counter >= 30)
			{
				title_counter = 0;
				update_title();
			}
		}
		return 0;
	}

	// Stands in for a graphics context in the self-check and keeps the texture
	// a repaint hands it.
	class recording_texture final : public render::texture
	{
	public:
		core::const_surface_ptr surface;

		render::texture_update_result update(const render::video_frame_ptr&) override
		{
			return render::texture_update_result::failed;
		}

		render::texture_update_result update(const core::const_surface_ptr& value) override
		{
			const auto result = surface ? render::texture_update_result::tex_updated
			                           : render::texture_update_result::tex_created;
			surface = value;
			if (surface)
			{
				_dimensions = surface->dimensions();
				_src_extent = _dimensions;
				_format = surface->format();
				_orientation = surface->orientation();
				_cs = surface->color_space();
			}
			return surface ? result : render::texture_update_result::failed;
		}

		render::texture_update_result update(core::sizei dims, core::texture_format format,
		                                     core::orientation orientation, const uint8_t* pixels,
		                                     size_t stride, size_t buffer_size) override
		{
			(void)dims;
			(void)format;
			(void)orientation;
			(void)pixels;
			(void)stride;
			(void)buffer_size;
			return render::texture_update_result::failed;
		}

		bool is_valid() const override { return surface != nullptr; }
	};

	struct recording_draw_context final : ui::test::fake_draw_context
	{
		render::texture_ptr drawn;
		core::recti drawn_bounds;

		using ui::test::fake_draw_context::draw_texture;

		render::texture_ptr create_texture() override
		{
			return std::make_shared<recording_texture>();
		}

		void draw_texture(const render::texture_ptr& texture, core::recti dst, float,
		                 render::texture_sampler) override
		{
			drawn = texture;
			drawn_bounds = dst;
		}
	};

	bool lit_surface(const core::const_surface_ptr& surface)
	{
		if (!surface || surface->empty()) return false;
		const auto* pixels = reinterpret_cast<const uint32_t*>(surface->pixels());
		const auto count = surface->size() / sizeof(uint32_t);
		return std::any_of(pixels, pixels + count, [](const uint32_t p) { return (p & 0xffffff) != 0; });
	}

	// Headless check of everything the window contributes: that a mode has a
	// size, that the scaled picture fits inside the client area, and that
	// pixels actually reach the surface that gets blitted.
	int dui_app::run_offscreen()
	{
		constexpr int frames = 120;
		int failures = 0;

		auto check = [&failures](const bool ok, const char* what)
		{
			if (!ok)
			{
				++failures;
				platform::write_stdout(std::format("FAIL: {}\n", what));
			}
		};

		for (int i = 0; i < frames; ++i)
		{
			if (!platform::pump_events()) break;
			g_emulator->exec(g_emulator->cycles_per_frame());
			present();
		}

		check(g_client.cx > 0 && g_client.cy > 0, "the content reported a client area");
		check(g_layout.width > 0 && g_layout.height > 0, "the machine reported a mode size");
		check(g_layout.scale_x >= 1 && g_layout.scale_y >= 1, "the picture scale is a whole number");
		check(g_layout.scale_y <= g_layout.scale_x * 2, "a pixel is no more than twice as tall as wide");
		check(g_layout.x >= 0 && g_layout.y >= 0, "the picture fits inside the content area");
		check(g_surface && g_surface->dimensions() == g_client, "the surface covers the content area");
		// The prompt has drawn by now, so an all-black surface would mean the
		// framebuffer never reached it.
		check(lit_surface(g_surface), "the framebuffer reached the surface");

		// A repaint has to draw the picture through the render context: a
		// present outside the paint path would be lost on the next repaint.
		g_texture.reset();
		recording_draw_context dc;
		g_reactor->on_window_paint(dc);
		const auto recorded = std::dynamic_pointer_cast<recording_texture>(dc.drawn);
		check(recorded && recorded->surface && recorded->surface->dimensions() == g_client &&
		      lit_surface(recorded->surface),
		      "a repaint draws the picture through the render context");

		// Ctrl+O opens a dialog, which then gets Ctrl's release. Losing the
		// focus lets go of what is held, so the next key types itself rather
		// than arriving as Ctrl+key - which the prompt would drop.
		key_down(platform::keys::CONTROL, {});
		g_reactor->focus_changed(false, nullptr);
		key_down('A', {});
		// Exercise the same asynchronous physical-key poll used by the real loop;
		// the synthetic headless press is not held by this desktop keyboard.
		release_stale_keys();
		for (int i = 0; i < 10; ++i) g_emulator->exec(g_emulator->cycles_per_frame());
		char screen[4096] = {};
		g_emulator->text_screen(screen, sizeof(screen));
		check(std::string_view(screen).find(">a") != std::string_view::npos,
		      "a key held when the window loses the focus is let go");

		// A program sees a button held until its release arrives, and the
		// middle button is the driver's third.
		const auto buttons = [] { return g_emulator->state().bios.mouse.buttons; };
		const core::pointi click{g_screen_origin.x + 1, g_screen_origin.y + 1};
		g_reactor->on_mouse_right_button_down(click, {});
		g_reactor->on_mouse_middle_button_down(click, {});
		check(buttons() == 6, "the right and middle buttons reach the machine");
		g_reactor->on_mouse_right_button_up(click, {});
		g_reactor->on_mouse_middle_button_up(click, {});
		check(buttons() == 0, "the right and middle buttons are let go");

		// The close box and Alt+F4 end the session through the frame.
		if (g_frame) g_frame->close();
		check(!g_running, "a close message ends the session");

		platform::write_stdout(std::format("dui: {} (client {}x{}, mode {}x{} at {}x{})\n",
		                                   failures == 0 ? "PASS" : "FAIL", g_client.cx, g_client.cy,
		                                   g_layout.width, g_layout.height, g_layout.scale_x, g_layout.scale_y));

		return failures == 0 ? 0 : 1;
	}

	bool dui_app::key_down(const char32_t key, const platform::key_state keys)
	{
		// Alt/F10 belong to the menu bar and the window's own close handling; feeding
		// them to DOS would turn Alt+F4 into a guest F4 press instead of exiting.
		if (keys.alt || key == platform::keys::ALT || key == platform::keys::F10) return false;
		// The old reactor ignored auto-repeated F9 key-downs, so do not toggle
		// pause again while the key is held.
		if (key == platform::keys::F9 && keys.repeat) return true;
		if (ui::dispatch_accelerator(g_menus, key, keys)) return true;
		const auto machine = translate_key(key);
		if (machine == machine_key::none) return false;
		press_key(machine, key);
		return true;
	}

	int dui_app::main_loop()
	{
		return g_headless ? run_offscreen() : run();
	}

	void dui_app::exit()
	{
		release_all_keys();
		stop_audio();
		platform::sound_shutdown();
		g_texture.reset();
		g_frame.reset();
		g_reactor.reset();
		g_elements.reset();
		g_menus.clear();
		g_emulator.reset();
		g_host.reset();
	}

	void dui_app::system_event(const platform::os_event_type event)
	{
		if (event == platform::os_event_type::graphics_device_lost)
		{
			g_texture.reset();
			if (g_frame) g_frame->invalidate();
		}
		else if (event == platform::os_event_type::system_suspending)
		{
			stop_audio();
		}
		else if (event == platform::os_event_type::system_resumed && g_sound_on)
		{
			start_audio();
		}
	}
}

int main(int argc, char** argv)
{
	core::set_app_name("dui");
	auto app = std::make_shared<dui_app>();
	return platform::run(app, argc, argv);
}
