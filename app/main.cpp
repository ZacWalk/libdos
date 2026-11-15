// dui by Zac Walker, built on libdesktop's four-library application layer
//
// Runs the emulator in a ui::shell, which brings the window, the menu bar and
// the keys; the screen is a surface element, and the frame loop is dui's own,
// paced by the audio stream. The emulator itself never sees a window, and
// nothing here names an operating system.

#include "libdesktop/core.h"
#include "libdesktop/platform.h"
#include "libdesktop/render.h"
#include "libdesktop/ui.h"
#include "libdesktop/ui/shell.h"
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
	constexpr int frames_per_second = 60;
	constexpr int audio_blocks = 4;
	constexpr int samples_per_frame = sound_sample_rate / frames_per_second;

	ui::frame_ptr g_frame;
	std::shared_ptr<ui::surface_element> g_screen;
	std::unique_ptr<machine> g_emulator;
	std::unique_ptr<win32_host> g_host;
	platform::audio_stream_ptr g_audio;
	std::array<int16_t, samples_per_frame> g_audio_block = {};

	bool g_paused = false;
	bool g_sound_on = true;

	// What the machine has been told is held down. A key let go while another
	// window has the focus - a dialog, a shortcut such as Ctrl+O, or another
	// application - is released there, so without this it would stay down in
	// the machine and turn every later key into Ctrl+key.
	std::array<bool, 256> g_keys_down = {};

	// The content area the picture was last composed for.
	core::sizei g_client;

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

	// What shows of the scaled screen. The content's surface element keeps it in
	// a texture that each frame updates in place, so a paused or uncovered window
	// is drawn from the same complete frame.
	core::surface_ptr g_picture;

	bool g_headless = false;
	std::string g_target;
	// Source column for each visible destination column, so scaling costs no
	// divide per pixel.
	std::vector<int> g_columns;

	picture_layout compute_layout(core::sizei client);
	bool compose(core::sizei client);
	void release_key(machine_key key);
	void release_all_keys();
	void report(std::string_view message);
	void update_title();
	void open_path(const core::file_path& path);
	void toggle_pause();
	void toggle_sound();
	std::vector<ui::command_ptr> build_menus();

	class dui_app final : public ui::shell
	{
	public:
		dui_app() : shell(make_options()) {}

		bool parse_command_line(std::string_view command_line) override;
		void system_event(platform::os_event_type event) override;

		std::vector<ui::command_ptr> make_menus() override { return build_menus(); }
		ui::element_ptr make_content() override;
		bool start(std::string_view command_line) override;
		int main_loop() override;
		void present() override;
		bool key(char32_t key, platform::key_state keys) override;
		bool key_up(char32_t key, platform::key_state keys) override;
		void stop() override;

		void on_window_paint(render::draw_context& dc) override;
		void on_mouse_move(core::pointi loc, bool is_tracking) override;
		void on_mouse_left_button_down(core::pointi loc, platform::key_state keys) override;
		void on_mouse_left_button_up(core::pointi loc, platform::key_state keys) override;
		void on_mouse_middle_button_down(core::pointi loc, platform::key_state keys) override;
		void on_mouse_middle_button_up(core::pointi loc, platform::key_state keys) override;
		void on_mouse_right_button_down(core::pointi loc, platform::key_state keys) override;
		void on_mouse_right_button_up(core::pointi loc, platform::key_state keys) override;
		void on_mouse_left_button_double_click(core::pointi, platform::key_state) override {}
		void focus_changed(bool has_focus, const ui::control_ptr& child) override;
		void activate(bool active) override;

		platform::drop_effect drag_over(const platform::clipboard_data& data, platform::key_state keys,
		                                core::pointi loc) override;
		platform::drop_effect drag_drop(platform::clipboard_data& data, platform::key_state keys,
		                                core::pointi loc) override;

	private:
		static ui::shell_options make_options();
		static machine_key translate_key(char32_t key);
		static std::vector<std::string> command_line_args(std::string_view command_line);
		bool screen_point(core::pointi loc, int& x, int& y) const;
		int run();
		int run_offscreen();
	};

	std::string unquote(std::string_view value)
	{
		if (value.size() >= 2 &&
		    ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
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

	picture_layout compute_layout(const core::sizei client)
	{
		picture_layout layout;
		if (!g_emulator) return layout;

		layout.width = g_emulator->width();
		layout.height = g_emulator->height();
		if (layout.width <= 0 || layout.height <= 0 || client.cx <= 0 || client.cy <= 0) return layout;

		layout.scale_x = std::max(1, client.cx / layout.width);
		layout.scale_y = std::max(1, client.cy / layout.height);
		while (layout.scale_x > layout.scale_y && layout.scale_x > 1) --layout.scale_x;
		if (layout.scale_y > layout.scale_x * 2) layout.scale_y = layout.scale_x * 2;

		layout.x = (client.cx - layout.width * layout.scale_x) / 2;
		layout.y = (client.cy - layout.height * layout.scale_y) / 2;
		return layout;
	}

	// Rebuilt only when the layout changes. The picture is what the content area
	// shows of the scaled screen: all of it, or its middle in a window too small
	// for the screen at one to one.
	void rebuild_picture(const picture_layout& layout)
	{
		const int first_x = std::max(0, layout.x);
		const int last_x = std::min(g_client.cx, layout.x + layout.width * layout.scale_x);
		const int first_y = std::max(0, layout.y);
		const int last_y = std::min(g_client.cy, layout.y + layout.height * layout.scale_y);

		if (!g_picture) g_picture = std::make_shared<core::surface>();
		g_picture->alloc({last_x - first_x, last_y - first_y}, core::texture_format::RGB);

		g_columns.clear();
		for (int dx = first_x; dx < last_x; ++dx) g_columns.push_back((dx - layout.x) / layout.scale_x);
	}

	// Draws the current frame into the picture. False when there is nothing to
	// show yet.
	bool compose(const core::sizei client)
	{
		if (!g_emulator || client.cx <= 0 || client.cy <= 0) return false;

		const auto layout = compute_layout(client);
		if (layout.width <= 0 || layout.height <= 0) return false;

		if (layout != g_layout || client != g_client || !g_picture)
		{
			g_layout = layout;
			g_client = client;
			rebuild_picture(layout);
		}

		const uint32_t* source = g_emulator->framebuffer();
		if (source == nullptr || g_columns.empty()) return false;

		const int first_y = std::max(0, layout.y);
		const int rows = g_picture->dimensions().cy;

		for (int py = 0; py < rows; ++py)
		{
			const uint32_t* row = source +
			                      static_cast<size_t>((first_y + py - layout.y) / layout.scale_y) * layout.width;
			uint32_t* out = reinterpret_cast<uint32_t*>(g_picture->pixels_line(py));
			for (const int sx : g_columns) *out++ = row[sx];
		}
		return true;
	}

	// The picture comes scaled already - each axis by its own whole number, which
	// no single pixel aspect expresses - and never has room for a second multiple,
	// so by whole multiples the element draws it one to one, centred on black
	// where the layout places it.
	ui::element_ptr dui_app::make_content()
	{
		g_screen = std::make_shared<ui::surface_element>(ui::surface_element::style{.whole_multiples = true});
		return g_screen;
	}

	// Each frame is the same picture, so the element updates its texture in place
	// and presents the picture's region again, painting nothing.
	void dui_app::present()
	{
		if (compose(content_bounds().extent())) g_screen->present(g_picture, frame());
	}

	// A repaint composes the frame again at the content's size now, so a window
	// being sized shows the picture at its new scale. No frame is given: the
	// paint under way draws it.
	void dui_app::on_window_paint(render::draw_context& dc)
	{
		if (compose(content_bounds().extent())) g_screen->present(g_picture, nullptr);
		shell::on_window_paint(dc);
	}

	// Through the same letterbox and scale the picture is drawn
	// with, so the pointer lands where the user sees it.
	bool dui_app::screen_point(const core::pointi loc, int& x, int& y) const
	{
		const auto screen = content_bounds();
		if (screen.is_empty()) return false;
		x = std::clamp((loc.x - screen.left - g_layout.x) / std::max(1, g_layout.scale_x), 0,
		               std::max(0, g_layout.width - 1));
		y = std::clamp((loc.y - screen.top - g_layout.y) / std::max(1, g_layout.scale_y), 0,
		               std::max(0, g_layout.height - 1));
		return screen.contains(loc);
	}

	// The pointer over the screen is the machine's alone, so none of it goes to
	// the shell's element host: the content has no controller for it to find,
	// and a press or a double click that finds none paints the whole window again.
	void dui_app::on_mouse_move(const core::pointi loc, bool)
	{
		if (!g_emulator) return;
		int x = 0, y = 0;
		screen_point(loc, x, y);
		g_emulator->mouse_move(x, y);
	}

	void dui_app::on_mouse_left_button_down(const core::pointi loc, platform::key_state)
	{
		int x = 0, y = 0;
		if (screen_point(loc, x, y) && g_emulator) g_emulator->mouse_button(0, true);
	}

	void dui_app::on_mouse_left_button_up(const core::pointi, platform::key_state)
	{
		if (g_emulator) g_emulator->mouse_button(0, false);
	}

	void dui_app::on_mouse_middle_button_down(const core::pointi loc, platform::key_state)
	{
		int x = 0, y = 0;
		if (screen_point(loc, x, y) && g_emulator) g_emulator->mouse_button(2, true);
	}

	void dui_app::on_mouse_middle_button_up(const core::pointi, platform::key_state)
	{
		if (g_emulator) g_emulator->mouse_button(2, false);
	}

	void dui_app::on_mouse_right_button_down(const core::pointi loc, platform::key_state)
	{
		int x = 0, y = 0;
		if (screen_point(loc, x, y) && g_emulator) g_emulator->mouse_button(1, true);
	}

	void dui_app::on_mouse_right_button_up(const core::pointi, platform::key_state)
	{
		if (g_emulator) g_emulator->mouse_button(1, false);
	}

	void dui_app::focus_changed(const bool has_focus, const ui::control_ptr& child)
	{
		shell::focus_changed(has_focus, child);
		if (!has_focus) release_all_keys();
	}

	void dui_app::activate(const bool active)
	{
		shell::activate(active);
		if (!active) release_all_keys();
	}

	platform::drop_effect dui_app::drag_over(const platform::clipboard_data& data, platform::key_state, core::pointi)
	{
		return data.has_drop_files() ? platform::drop_effect::link : platform::drop_effect::none;
	}

	platform::drop_effect dui_app::drag_drop(platform::clipboard_data& data, platform::key_state, core::pointi)
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

	ui::command_ptr make_command(std::string_view text, std::function<void()> invoke = {}, char32_t key = 0,
	                             uint32_t key_state = 0, bool checkable = false, bool checked = false)
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

	// Child menus are built when opened, so checked state is current.
	std::vector<ui::command_ptr> build_menus()
	{
		auto file = make_command("&File");
		file->menu = []
		{
			std::vector<ui::command_ptr> children;
			children.push_back(
				make_command("&Open Program...", open_program, 'O', platform::keyboard_accelerator_t::control));
			children.push_back(make_command("Open &Folder...", open_folder));
			children.push_back(std::make_shared<ui::command>());
			children.push_back(make_command("E&xit", []
			{
				if (g_frame) g_frame->close();
			}));
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
				platform::message_box(
					nullptr,
					"dui\n\nA DOS program emulator built on libdos: 8086/80186, VGA/EGA/CGA, AdLib and "
					"PC speaker, with DOS and BIOS services mapped rather than emulated.\n\n"
					"https://github.com/ZacWalk/libdos",
					"About dui", {platform::message_box_buttons::ok, platform::message_box_icon::none});
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

		g_frame->window_text(std::format("dui - {} - mode {:02X}h {}x{}{}", what, s.vga.mode, s.vga.width, s.vga.height,
		                                 g_paused ? " - paused" : ""));
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
		if (key >= '0' && key <= '9') return machine_key(int(machine_key::num1) + ((key - '0' + 9) % 10));

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

	void press_key(const machine_key key)
	{
		if (!g_emulator) return;
		g_emulator->key_down(key);
		if (key != machine_key::none) g_keys_down[size_t(key)] = true;
	}

	void release_key(const machine_key key)
	{
		if (!g_emulator || key == machine_key::none) return;
		g_emulator->key_up(key);
		g_keys_down[size_t(key)] = false;
	}

	// The window no longer hears about the keys it has, so it lets go of them.
	// A focus change can also come while the window is being torn down.
	void release_all_keys()
	{
		if (!g_emulator) return;
		for (size_t i = 0; i < g_keys_down.size(); ++i)
			if (g_keys_down[i]) release_key(machine_key(i));
	}

	bool dui_app::parse_command_line(const std::string_view command_line)
	{
		g_headless = false;
		g_target.clear();
		for (const auto& arg : command_line_args(command_line))
		{
			if (core::icmp(arg, "-test") == 0 || core::icmp(arg, "/test") == 0) g_headless = true;
			else if (g_target.empty()) g_target = arg;
		}

		// -test is an offscreen GUI run, not platform::run's console branch:
		// the shell still has to make the window and exercise its paint path.
		initial_options().offscreen = g_headless;
		return false;
	}

	ui::shell_options dui_app::make_options()
	{
		ui::shell_options options;
		options.client_size = {1280, 800};
		// Any size: a window too small for the picture crops it.
		options.minimum_client = {};
		options.gpu = true;
		options.accepts_drops = true;
		// Realtime, so a held key never repeats a menu command. The frames are
		// paced by main_loop, at this rate when the sound is off.
		options.steps_per_second = frames_per_second;
		// A program's music plays on while the window is minimized.
		options.run_minimized = true;
		// dui keeps no settings: the window opens centred every time.
		options.remember_placement = false;
		return options;
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

	bool dui_app::start(std::string_view)
	{
		g_frame = frame();
		g_paused = false;
		g_sound_on = !g_headless;
		g_client = {};
		g_layout = {};
		g_picture.reset();
		g_keys_down.fill(false);

		g_emulator = std::make_unique<machine>();
		g_host = std::make_unique<win32_host>();
		g_emulator->set_host(g_host.get());

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
		constexpr double period = 1.0 / frames_per_second;
		double last = platform::monotonic_seconds();
		int title_counter = 0;

		while (platform::pump_events())
		{
			if (!g_emulator) break;

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
			// A minimized window shows nothing; the paint that restores it
			// composes the frame of the moment.
			if (!is_minimized()) present();

			if (++title_counter >= 30)
			{
				title_counter = 0;
				update_title();
			}
		}
		return 0;
	}

	bool lit_surface(const core::const_surface_ptr& surface)
	{
		if (!surface || surface->empty()) return false;
		const auto* pixels = reinterpret_cast<const uint32_t*>(surface->pixels());
		const auto count = surface->size() / sizeof(uint32_t);
		return std::any_of(pixels, pixels + count, [](const uint32_t p) { return (p & 0xffffff) != 0; });
	}

	// Headless check of everything the window contributes: that a mode has a
	// size, that the scaled picture fits inside the client area, that pixels
	// actually reach the picture that gets drawn, and that a frame updates it
	// in place.
	int dui_app::run_offscreen()
	{
		constexpr int frames = 120;
		int failures = 0;
		int checks = 0;
		int rendered = 0;

		auto check = [&](const bool ok, const char* what)
		{
			++checks;
			if (!ok) ++failures;
			platform::write_stdout(std::format("{}: {}\n", ok ? "PASS" : "FAIL", what));
		};

		for (int i = 0; i < frames; ++i)
		{
			if (!platform::pump_events()) break;
			g_emulator->exec(g_emulator->cycles_per_frame());
			present();
			++rendered;
		}

		check(rendered == frames, "the offscreen run completes every emulated frame");
		check(g_client.cx > 0 && g_client.cy > 0, "the content reported a client area");
		check(g_layout.width > 0 && g_layout.height > 0, "the machine reported a mode size");
		check(g_layout.scale_x >= 1 && g_layout.scale_y >= 1, "the picture scale is a whole number");
		check(g_layout.scale_y <= g_layout.scale_x * 2, "a pixel is no more than twice as tall as wide");
		check(g_layout.x >= 0 && g_layout.y >= 0, "the picture fits inside the content area");
		const core::sizei scaled{g_layout.width * g_layout.scale_x, g_layout.height * g_layout.scale_y};
		check(g_picture && g_picture->dimensions() == scaled, "the picture is the mode at its scale");
		// The prompt has drawn by now, so an all-black picture would mean the
		// framebuffer never reached it.
		check(lit_surface(g_picture), "the framebuffer reached the picture");

		// A repaint has to draw the picture through the render context: a
		// present outside the paint path would be lost on the next repaint.
		g_screen->dispatch_event({ui::element_event_type::free_graphics_resources});
		ui::test::recording_draw_context dc;
		on_window_paint(dc);
		const auto drawn = dc.textures.empty() ? ui::test::recording_draw_context::texture_draw{} : dc.textures.back();
		check(drawn.texture && drawn.texture->surface == g_picture && lit_surface(drawn.texture->surface),
		      "a repaint draws the picture through the render context");
		const auto content = content_bounds();
		check(drawn.bounds == core::recti(core::pointi{content.left + g_layout.x, content.top + g_layout.y}, scaled),
		      "the picture is drawn unscaled where the layout puts it");

		// The next frame goes into the same texture, and only the picture's
		// region is presented again: nothing is painted for it.
		const uint64_t paints = render::ui_perf.paints;
		const uint64_t redraws = render::ui_perf.redraws;
		present();
		platform::pump_events();
		check(drawn.texture && drawn.texture->updates == 2 && render::ui_perf.redraws == redraws + 1 &&
		          render::ui_perf.paints == paints,
		      "a frame updates the picture in place without painting the window");

		// Ctrl+O opens a dialog, which then gets Ctrl's release. Losing the
		// focus lets go of what is held, so the next key types itself rather
		// than arriving as Ctrl+key - which the prompt would drop.
		key_down(platform::keys::CONTROL, {});
		focus_changed(false, nullptr);
		key_down('A', {});
		key_up('A', {});
		for (int i = 0; i < 10; ++i) g_emulator->exec(g_emulator->cycles_per_frame());
		char screen[4096] = {};
		g_emulator->text_screen(screen, sizeof(screen));
		check(std::string_view(screen).find(">a") != std::string_view::npos,
		      "a key held when the window loses the focus is let go");
		check(!g_keys_down[size_t(machine_key::a)], "a key's release reaches the machine");

		// A program sees a button held until its release arrives, and the
		// middle button is the driver's third.
		const auto buttons = []
		{
			return g_emulator->state().bios.mouse.buttons;
		};
		const core::pointi click{content.left + 1, content.top + 1};
		on_mouse_right_button_down(click, {});
		on_mouse_middle_button_down(click, {});
		check(buttons() == 6, "the right and middle buttons reach the machine");
		on_mouse_right_button_up(click, {});
		on_mouse_middle_button_up(click, {});
		check(buttons() == 0, "the right and middle buttons are let go");

		// The close box and Alt+F4 end the session through the frame.
		if (g_frame) g_frame->close();
		check(!platform::pump_events(), "a close message ends the session");

		platform::write_stdout(std::format("dui: {} checks, {} failed, {} frames (client {}x{}, mode {}x{} at {}x{})\n",
		                                   checks, failures, rendered, g_client.cx, g_client.cy, g_layout.width,
		                                   g_layout.height, g_layout.scale_x, g_layout.scale_y));

		return failures == 0 ? 0 : 1;
	}

	bool dui_app::key(const char32_t key, const platform::key_state keys)
	{
		// Alt/F10 belong to the menu bar and the window's own close handling; feeding
		// them to DOS would turn Alt+F4 into a guest F4 press instead of exiting.
		if (keys.alt || key == platform::keys::ALT || key == platform::keys::F10) return false;
		const auto machine = translate_key(key);
		if (machine == machine_key::none) return false;
		press_key(machine);
		return true;
	}

	bool dui_app::key_up(const char32_t key, const platform::key_state keys)
	{
		const auto machine = translate_key(key);
		if (machine != machine_key::none && g_keys_down[size_t(machine)]) release_key(machine);
		return shell::key_up(key, keys);
	}

	// dui keeps its own loop rather than the shell's fixed step. With sound on, a
	// frame is due when the audio device has room for one - the device's clock,
	// which a wall-clock step drifts from until the queue runs dry or overflows.
	int dui_app::main_loop()
	{
		return g_headless ? run_offscreen() : run();
	}

	// The screen's texture and the frame must not outlive the renderer, which
	// shuts down once the shell has let go of its own.
	void dui_app::stop()
	{
		release_all_keys();
		stop_audio();
		platform::sound_shutdown();
		g_screen.reset();
		g_frame.reset();
		g_emulator.reset();
		g_host.reset();
	}

	void dui_app::system_event(const platform::os_event_type event)
	{
		shell::system_event(event);
		if (event == platform::os_event_type::system_suspending)
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
