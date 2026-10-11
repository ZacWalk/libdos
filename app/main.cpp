// ovrwin by Zac Walker
//
// Binds the emulator to the shared platform layer: the window, the menu, the
// key mapping, the audio stream that paces it and the frame loop. The emulator
// itself never sees a window, and nothing here names an operating system.

#include "platform.h"

// The host is the one Windows-specific piece left, and it is shared with the
// CLI. platform.h is included first so its std::max survives windows.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../host/host-win32.h"

#include "../src/machine.h"
#include "../src/sound.h"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <vector>

namespace
{
	constexpr int menu_open = 100;
	constexpr int menu_open_folder = 101;
	constexpr int menu_exit = 102;
	constexpr int menu_reset = 110;
	constexpr int menu_pause = 111;
	constexpr int menu_sound = 120;
	constexpr int menu_about = 140;

	constexpr int audio_blocks = 4;
	constexpr int samples_per_frame = sound_sample_rate / 60;

	pf::window_frame_ptr g_frame;
	std::shared_ptr<pf::frame_reactor> g_reactor;
	std::unique_ptr<machine> g_emulator;
	std::unique_ptr<win32_host> g_host;
	pf::audio_stream_ptr g_audio;
	std::array<int16_t, samples_per_frame> g_audio_block = {};

	bool g_running = true;
	bool g_paused = false;
	bool g_sound_on = true;

	// What the machine has been told is held down. A key let go while another
	// window has the focus - a dialog a shortcut such as Ctrl+O opened, or
	// another application - is released there, so without this it would stay
	// down in the machine and turn every later key into Ctrl+key.
	std::array<bool, 256> g_keys_down = {};

	pf::isize g_client;

	// Where the picture sits in the window, at a whole-number scale. A
	// fractional factor has to drop or double individual rows of pixels, which
	// is what makes an emulated screen look uneven; an integer factor cannot.
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

	// The window-sized surface everything is composed into, so the letterbox
	// and the picture arrive in one blit and nothing flickers. A bitmap, so a
	// repaint can hand it to the paint context as well.
	pf::bitmap g_surface;

	// Source column for each visible destination column, so scaling costs no
	// divide per pixel.
	std::vector<int> g_columns;

	picture_layout compute_layout()
	{
		picture_layout layout;
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
		g_surface.width = g_client.cx;
		g_surface.height = g_client.cy;
		g_surface.pixels.assign(static_cast<size_t>(g_client.cx) * g_client.cy, 0);

		const int first = std::max(0, layout.x);
		const int last = std::min(g_client.cx, layout.x + layout.width * layout.scale_x);

		g_columns.clear();
		for (int dx = first; dx < last; ++dx)
			g_columns.push_back((dx - layout.x) / layout.scale_x);
	}

	// Draws the current frame into the surface. False when there is nothing
	// to show yet.
	bool compose()
	{
		if (!g_emulator || g_client.cx <= 0 || g_client.cy <= 0) return false;

		const auto layout = compute_layout();
		if (layout.width <= 0 || layout.height <= 0) return false;

		if (layout != g_layout || g_surface.pixels.size() != static_cast<size_t>(g_client.cx) * g_client.cy)
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
			uint32_t* out = g_surface.pixels.data() + static_cast<size_t>(dy) * g_client.cx + first_x;
			for (const int sx : g_columns) *out++ = row[sx];
		}
		return true;
	}

	// The frame loop's way to the screen: straight to the window, between
	// paints. A repaint goes through handle_paint instead.
	void present()
	{
		if (g_frame && compose()) g_frame->present_pixels(g_surface.pixels.data(), g_client.cx, g_client.cy);
	}

	void update_title()
	{
		const machine_state& s = g_emulator->state();
		const char* what = "no program";
		if (s.shell.program[0] != 0) what = s.shell.program;
		else if (s.shell.active) what = "command prompt";

		g_frame->set_text(std::format("ovrwin - {} - mode {:02X}h {}x{}{}", what, s.vga.mode,
		                              s.vga.width, s.vga.height, g_paused ? " - paused" : ""));
	}

	void start_audio()
	{
		if (g_audio || !pf::sound_init()) return;

		g_audio = pf::create_audio_stream(sound_sample_rate, 1, audio_blocks);
		if (g_audio) g_emulator->set_audio_enabled(true);
	}

	void stop_audio()
	{
		if (!g_audio) return;
		g_audio->stop();
		g_audio.reset();
		g_emulator->set_audio_enabled(false);
	}

	// Submits one frame of audio. The device consumes at a fixed rate, so
	// waiting for room in the queue is what paces emulation to real time.
	void submit_audio()
	{
		if (!g_audio || !g_audio->can_write()) return;
		g_emulator->generate_audio(g_audio_block.data(), samples_per_frame);
		g_audio->write(g_audio_block);
	}

	void report(const std::string_view message)
	{
		g_frame->message_box(message, "ovrwin", pf::msg_box_style::ok | pf::msg_box_style::icon_warning);
	}

	// Mounts a folder as the emulated drive and brings the shell up in it.
	bool mount(const pf::file_path& directory)
	{
		if (!g_host->set_root(directory.c_str()))
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

	// Accepts either a program or a folder, which is what a drop can be. False
	// when nothing was mounted, so whatever was running before still is.
	bool open_path(const pf::file_path& path)
	{
		if (pf::is_directory(path)) return mount(path);

		if (!path.exists())
		{
			report("Cannot open that path.");
			return false;
		}

		const auto folder = path.folder();
		if (!mount(folder.empty() ? pf::file_path{"."} : folder)) return false;
		if (!g_emulator->run_program(path.name().c_str(), "")) report("That file is not a DOS program.");
		update_title();
		return true;
	}

	void open_program()
	{
		// Embedded nulls separate the filter pairs, so the length is explicit.
		constexpr std::string_view filters("DOS programs\0*.exe;*.com\0All files\0*.*\0\0", 39);
		const auto path = pf::open_file_path("Open Program", filters);
		if (!path.empty()) open_path(path);
	}

	void open_folder()
	{
		const auto folder = pf::pick_folder_path("Choose the folder to use as the emulated drive");
		if (!folder.empty()) mount(folder);
	}

	void toggle_pause()
	{
		g_paused = !g_paused;
		update_title();
	}

	void press_key(const machine_key key)
	{
		g_emulator->key_down(key);
		if (key != machine_key::none) g_keys_down[size_t(key)] = true;
	}

	void release_key(const machine_key key)
	{
		g_emulator->key_up(key);
		if (key != machine_key::none) g_keys_down[size_t(key)] = false;
	}

	// The window no longer hears about the keys it has, so it lets go of them.
	// A focus change can also come while the window is being torn down.
	void release_all_keys()
	{
		if (!g_emulator) return;
		for (size_t i = 0; i < g_keys_down.size(); ++i)
			if (g_keys_down[i]) release_key(machine_key(i));
	}

	void toggle_sound()
	{
		g_sound_on = !g_sound_on;
		if (g_sound_on) start_audio();
		else stop_audio();
	}

	std::vector<pf::menu_command> build_menu()
	{
		using pf::key_binding;
		using pf::menu_command;
		namespace key_mod = pf::key_mod;

		menu_command file;
		file.text = "&File";
		file.children.emplace_back("&Open Program...", menu_open, open_program, nullptr, nullptr,
		                           key_binding{'O', key_mod::ctrl});
		file.children.emplace_back("Open &Folder...", menu_open_folder, open_folder);
		file.children.emplace_back();
		file.children.emplace_back("E&xit", menu_exit, [] { g_frame->close(); });

		menu_command machine_menu;
		machine_menu.text = "&Machine";
		machine_menu.children.emplace_back("&Reset", menu_reset, []
		{
			// Back to a prompt the user can see and type at, as opening a
			// folder does: a paused machine would keep showing the old
			// picture under a title that says "command prompt".
			g_emulator->start_shell();
			g_paused = false;
			update_title();
		}, nullptr, nullptr, key_binding{'R', key_mod::ctrl});
		machine_menu.children.emplace_back("&Pause", menu_pause, toggle_pause, nullptr,
		                                   [] { return g_paused; },
		                                   key_binding{pf::platform_key::F9});

		menu_command view;
		view.text = "&View";
		view.children.emplace_back("&Sound", menu_sound, toggle_sound, nullptr,
		                           [] { return g_sound_on; });

		menu_command help;
		help.text = "&Help";
		help.children.emplace_back("&About ovrwin", menu_about, []
		{
			g_frame->message_box(
				"ovrwin\n\nA DOS program emulator: 8086/80186, VGA/EGA/CGA, AdLib and "
				"PC speaker, with DOS and BIOS services mapped rather than emulated.\n\n"
				"https://github.com/ZacWalk/ovrwin",
				"About ovrwin", pf::msg_box_style::ok);
		});

		return {std::move(file), std::move(machine_menu), std::move(view), std::move(help)};
	}

	// The platform reports one code per physical key, with no left/right and no
	// keypad distinction, so both sides of a modifier map to the left one -
	// which is what keeps every press paired with its release.
	machine_key translate_key(const unsigned int key)
	{
		// Qualified, not a using-directive: windows.h has an Escape of its own.
		namespace pk = pf::platform_key;

		switch (key)
		{
		case pk::Escape: return machine_key::escape;
		case pk::F1: return machine_key::f1;
		case pk::F2: return machine_key::f2;
		case pk::F3: return machine_key::f3;
		case pk::F4: return machine_key::f4;
		case pk::F5: return machine_key::f5;
		case pk::F6: return machine_key::f6;
		case pk::F7: return machine_key::f7;
		case pk::F8: return machine_key::f8;
		case pk::F10: return machine_key::f10;
		case pk::F11: return machine_key::f11;
		case pk::F12: return machine_key::f12;
		case pk::Back: return machine_key::backspace;
		case pk::Tab: return machine_key::tab;
		case pk::Return: return machine_key::enter;
		case pk::Shift: return machine_key::left_shift;
		case pk::Control: return machine_key::left_ctrl;
		case pk::Alt: return machine_key::left_alt;
		case pk::CapsLock: return machine_key::caps_lock;
		case pk::Space: return machine_key::space;
		case pk::Prior: return machine_key::page_up;
		case pk::Next: return machine_key::page_down;
		case pk::End: return machine_key::end;
		case pk::Home: return machine_key::home;
		case pk::Left: return machine_key::left;
		case pk::Up: return machine_key::up;
		case pk::Right: return machine_key::right;
		case pk::Down: return machine_key::down;
		case pk::Insert: return machine_key::insert;
		case pk::Delete: return machine_key::del;
		case pk::NumLock: return machine_key::num_lock;
		case pk::ScrollLock: return machine_key::scroll_lock;
		case pk::Minus: return machine_key::minus;
		case pk::Equals: return machine_key::equals;
		case pk::Semicolon: return machine_key::semicolon;
		case pk::Slash: return machine_key::slash;
		case pk::Backtick: return machine_key::backtick;
		case pk::LeftBracket: return machine_key::left_bracket;
		case pk::Backslash: return machine_key::backslash;
		case pk::RightBracket: return machine_key::right_bracket;
		case pk::Quote: return machine_key::quote;
		case pk::Comma: return machine_key::comma;
		case pk::Period: return machine_key::period;
		case pk::Multiply: return machine_key::keypad_star;
		case pk::Add: return machine_key::keypad_plus;
		case pk::Subtract: return machine_key::keypad_minus;
		case pk::Divide: return machine_key::keypad_slash;
		case pk::Decimal: return machine_key::keypad_period;
		default: break;
		}

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

		if (key >= pk::Numpad0 && key <= pk::Numpad9)
			return machine_key(int(machine_key::keypad0) + int(key - pk::Numpad0));

		return machine_key::none;
	}

	struct app_reactor final : pf::frame_reactor
	{
		uint32_t handle_message(pf::window_frame_ptr window, const pf::message_type message,
		                        const pf::message_params& params) override
		{
			switch (message)
			{
			case pf::message_type::create:
			{
				// The process is DPI aware, so the window size is in real
				// pixels: scale it to come up the same physical size
				// everywhere, big enough for the common modes at a
				// whole-number scale.
				const auto scale = window->get_dpi_scale();
				const auto screen = pf::platform_screen_size();
				const int cx = static_cast<int>(1280 * scale);
				const int cy = static_cast<int>(800 * scale);
				const int left = std::max(0, (screen.cx - cx) / 2);
				const int top = std::max(0, (screen.cy - cy) / 2);
				window->move_window({left, top, left + cx, top + cy});
				window->accept_drop_files(true);
				return 0;
			}

			case pf::message_type::erase_background:
				return 1; // the frame loop paints every pixel

			case pf::message_type::close:
				// The platform leaves WM_CLOSE to the reactor; without this the
				// close box and Alt+F4 do nothing and only EXIT or File > Exit
				// end the session.
				window->close();
				return 0;

			case pf::message_type::destroy:
				g_running = false;
				return 0;

			case pf::message_type::kill_focus:
				release_all_keys();
				return 0;

			case pf::message_type::drop_files:
				if (!params.dropped_paths.empty()) open_path(params.dropped_paths.front());
				return 0;

			default:
				return 0;
			}
		}

		uint32_t handle_keyboard(pf::window_frame_ptr, const pf::keyboard_message_type message,
		                         const pf::keyboard_params& params) override
		{
			if (message == pf::keyboard_message_type::key_down)
			{
				if (params.vk == pf::platform_key::F9)
				{
					if (!params.repeat) toggle_pause();
					return 0;
				}
				press_key(translate_key(params.vk));
			}
			else if (message == pf::keyboard_message_type::key_up)
			{
				release_key(translate_key(params.vk));
			}
			return 0;
		}

		uint32_t handle_mouse(pf::window_frame_ptr, const pf::mouse_message_type message,
		                      const pf::mouse_params& params) override
		{
			switch (message)
			{
			case pf::mouse_message_type::mouse_move:
			{
				// Through the same letterbox and scale the picture is drawn
				// with, so the pointer lands where the user sees it.
				const int x = (params.point.x - g_layout.x) / std::max(1, g_layout.scale_x);
				const int y = (params.point.y - g_layout.y) / std::max(1, g_layout.scale_y);
				g_emulator->mouse_move(std::clamp(x, 0, std::max(0, g_layout.width - 1)),
				                       std::clamp(y, 0, std::max(0, g_layout.height - 1)));
				return 0;
			}
			case pf::mouse_message_type::left_button_down:
				g_emulator->mouse_button(0, true);
				return 0;
			case pf::mouse_message_type::left_button_up:
				g_emulator->mouse_button(0, false);
				return 0;
			case pf::mouse_message_type::right_button_down:
				g_emulator->mouse_button(1, true);
				return 0;
			default:
				return 0;
			}
		}

		void handle_paint(pf::window_frame_ptr&, pf::draw_context& dc) override
		{
			// The platform paints into a back buffer and then copies it over
			// the window, so a present_pixels from here would be painted over:
			// black until the next frame, and for good while paused.
			if (compose()) dc.draw_bitmap(0, 0, g_surface);
			else dc.fill_solid_rect(dc.clip_rect(), pf::color_t(0, 0, 0));
		}

		void handle_size(pf::window_frame_ptr&, const pf::isize extent, pf::measure_context&) override
		{
			g_client = extent;
		}
	};

	int run()
	{
		constexpr double period = 1.0 / 60.0;
		double last = pf::platform_get_time();
		int title_counter = 0;

		while (g_running)
		{
			if (!pf::platform_events()) break;
			if (!g_running) break;

			// EXIT at the prompt is the only thing that ends the session; a
			// program terminating just returns to the shell.
			if (g_emulator->exited())
			{
				g_frame->close();
				continue;
			}

			if (g_paused)
			{
				pf::platform_sleep(16);
				continue;
			}

			// With sound on, waiting for room in the audio queue paces
			// emulation to exactly real time; without it, the wall clock does.
			if (g_audio)
			{
				if (!g_audio->can_write())
				{
					pf::platform_sleep(1);
					continue;
				}
			}
			else
			{
				// A fixed step, not "one frame since we last finished":
				// resetting the reference to the actual time accumulates the
				// sleep granularity and lands at about 57 unevenly spaced
				// frames a second.
				const double now = pf::platform_get_time();
				if (now - last < period)
				{
					pf::platform_sleep(1);
					continue;
				}
				last += period;
				if (now - last > period * 4) last = now; // too far behind to catch up
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

		stop_audio();
		pf::sound_shutdown();
		return 0;
	}

	// Stands in for the paint context in the self-check and keeps whatever
	// bitmap a repaint hands it.
	struct recording_draw_context final : pf::draw_context
	{
		pf::irect clip;
		const pf::bitmap* drawn = nullptr;

		pf::irect clip_rect() const override { return clip; }
		void fill_solid_rect(const pf::irect&, pf::color_t) override {}
		void fill_solid_rect(int, int, int, int, pf::color_t) override {}
		void draw_text(int, int, const pf::irect&, std::string_view, const pf::font&, pf::color_t,
		               pf::color_t) override {}
		pf::isize measure_text(std::string_view, const pf::font&) const override { return {}; }
		void draw_text_h(int, int, std::string_view, pf::font_handle, pf::color_t) override {}
		pf::isize measure_text_h(std::string_view, pf::font_handle) const override { return {}; }
		void draw_lines(std::span<const pf::ipoint>, pf::color_t) override {}
		void draw_solid_line(pf::ipoint, pf::ipoint, pf::color_t, int) override {}
		void draw_ellipse(int, int, int, int, pf::color_t, int) override {}
		void fill_ellipse(int, int, int, int, pf::color_t) override {}
		void set_clip_rect(const pf::irect&) override {}
		void clear_clip_rect() override {}
		void draw_bitmap(int, int, const pf::bitmap& bmp) override { drawn = &bmp; }
		void draw_bitmap(const pf::irect&, const pf::bitmap& bmp) override { drawn = &bmp; }
	};

	// Headless check of everything the window contributes: that a mode has a
	// size, that the scaled picture fits inside the client area, and that
	// pixels actually reach the surface that gets blitted.
	int run_offscreen()
	{
		constexpr int frames = 120;
		int failures = 0;

		auto check = [&failures](const bool ok, const char* what)
		{
			if (!ok)
			{
				++failures;
				pf::write_stdout(std::format("FAIL: {}\n", what));
			}
		};

		const auto lit = [](const uint32_t p) { return (p & 0xffffff) != 0; };

		for (int i = 0; i < frames; ++i)
		{
			if (!pf::platform_events()) break;
			g_emulator->exec(g_emulator->cycles_per_frame());
			present();
		}

		check(g_client.cx > 0 && g_client.cy > 0, "the window reported a client area");
		check(g_layout.width > 0 && g_layout.height > 0, "the machine reported a mode size");
		check(g_layout.scale_x >= 1 && g_layout.scale_y >= 1, "the picture scale is a whole number");
		check(g_layout.scale_y <= g_layout.scale_x * 2, "a pixel is no more than twice as tall as wide");
		check(g_layout.x >= 0 && g_layout.y >= 0, "the picture fits inside the client area");
		check(g_surface.pixels.size() == static_cast<size_t>(g_client.cx) * g_client.cy &&
		      g_surface.width == g_client.cx && g_surface.height == g_client.cy,
		      "the surface covers the client area");

		// The prompt has drawn by now, so an all-black surface would mean the
		// framebuffer never reached it.
		check(std::any_of(g_surface.pixels.begin(), g_surface.pixels.end(), lit),
		      "the framebuffer reached the surface");

		// A repaint has to draw the picture through the paint context: the
		// platform copies its back buffer over the window afterwards, so a
		// frame blitted around it is lost - which left a paused window black.
		recording_draw_context dc;
		dc.clip = pf::irect(0, 0, g_client.cx, g_client.cy);
		pf::window_frame_ptr frame = g_frame;
		g_reactor->handle_paint(frame, dc);
		check(dc.drawn != nullptr && dc.drawn->width == g_client.cx && dc.drawn->height == g_client.cy &&
		      std::any_of(dc.drawn->pixels.begin(), dc.drawn->pixels.end(), lit),
		      "a repaint draws the picture through the paint context");

		// Ctrl+O opens a dialog, which then gets Ctrl's release. Losing the
		// focus lets go of what is held, so the next key types itself rather
		// than arriving as Ctrl+key - which the prompt would drop.
		pf::keyboard_params control;
		control.vk = pf::platform_key::Control;
		g_reactor->handle_keyboard(frame, pf::keyboard_message_type::key_down, control);
		g_reactor->handle_message(frame, pf::message_type::kill_focus, pf::message_params{});
		pf::keyboard_params letter;
		letter.vk = 'A';
		g_reactor->handle_keyboard(frame, pf::keyboard_message_type::key_down, letter);
		g_reactor->handle_keyboard(frame, pf::keyboard_message_type::key_up, letter);
		for (int i = 0; i < 10; ++i) g_emulator->exec(g_emulator->cycles_per_frame());
		char screen[4096] = {};
		g_emulator->text_screen(screen, sizeof(screen));
		check(std::string_view(screen).find(">a") != std::string_view::npos,
		      "a key held when the window loses the focus is let go");

		// The close box and Alt+F4 arrive as nothing but a close message, so
		// that is how the self-check ends the session too.
		g_reactor->handle_message(g_frame, pf::message_type::close, pf::message_params{});
		check(!g_running, "a close message ends the session");
		if (g_running) g_frame->close();

		pf::write_stdout(std::format("ovrwin app: {} (client {}x{}, mode {}x{} at {}x{})\n",
		                             failures == 0 ? "PASS" : "FAIL", g_client.cx, g_client.cy,
		                             g_layout.width, g_layout.height, g_layout.scale_x, g_layout.scale_y));

		return failures == 0 ? 0 : 1;
	}
}

app_init_result app_init(const pf::window_frame_ptr& main_frame,
                         const std::span<const std::string_view> params)
{
	pf::config_set_app_name("ovrwin");

	bool headless = false;
	std::string_view target;

	for (const auto& param : params)
	{
		if (pf::icmp(param, "-test") == 0 || pf::icmp(param, "/test") == 0) headless = true;
		else if (target.empty()) target = param;
	}

	g_frame = main_frame;
	g_emulator = std::make_unique<machine>();
	g_host = std::make_unique<win32_host>();
	g_emulator->set_host(g_host.get());

	g_reactor = std::make_shared<app_reactor>();
	main_frame->set_reactor(g_reactor);
	main_frame->set_menu(build_menu());
	main_frame->set_text("ovrwin");

	// A self-check is over in a moment; opening the sound device would only add
	// a second of silence to it.
	if (headless)
	{
		pf::attach_console();
		g_sound_on = false;
	}

	// A program or a folder on the command line, or the current directory so
	// the user can look around from the prompt before starting anything - which
	// is also where one that cannot be opened leaves them, rather than at a
	// blank window that ignores the keyboard.
	if (target.empty() || !open_path(pf::file_path{pf::unquote(target)}))
		mount(pf::current_directory());

	app_init_result result;
	result.offscreen_gui = headless;
	result.main_loop = headless ? run_offscreen : run;
	return result;
}

void app_idle()
{
}

void app_destroy()
{
	stop_audio();
	g_emulator.reset();
	g_host.reset();
	g_reactor.reset();
	g_frame.reset();
}
