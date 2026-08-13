// ovrwin by Zac Walker
//
// Win32 host: a window, a DIB blit, waveOut audio and the host key mapping.
// Everything Windows-specific lives here; the emulator itself never sees a
// window handle.

#include "../src/machine.h"
#include "../src/debug.h"
#include "../host/host-win32.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	constexpr int menu_open = 100;
	constexpr int menu_open_folder = 101;
	constexpr int menu_exit = 102;
	constexpr int menu_reset = 110;
	constexpr int menu_pause = 111;
	constexpr int menu_sound = 120;
	constexpr int menu_about = 140;

	constexpr int audio_buffers = 4;
	constexpr int samples_per_frame = sound_sample_rate / 60;

	HWND window = nullptr;
	HMENU menu_bar = nullptr;
	machine* emulator = nullptr;
	win32_host* host = nullptr;

	bool running = true;
	bool paused = false;
	bool sound_on = true;

	HWAVEOUT wave = nullptr;
	WAVEHDR headers[audio_buffers] = {};
	int16_t audio[audio_buffers][samples_per_frame] = {};
	int next_buffer = 0;

	BITMAPINFO bitmap_info = {};

	// Everything is composed into this off-screen surface and presented with a
	// single BitBlt; drawing the letterbox and the picture straight to the
	// window flickers.
	HDC back_dc = nullptr;
	HBITMAP back_bitmap = nullptr;
	HBITMAP back_previous = nullptr;
	int back_width = 0;
	int back_height = 0;

	void release_back_buffer()
	{
		if (back_dc == nullptr) return;
		SelectObject(back_dc, back_previous);
		DeleteDC(back_dc);
		DeleteObject(back_bitmap);
		back_dc = nullptr;
		back_bitmap = nullptr;
		back_width = 0;
		back_height = 0;
	}

	bool ensure_back_buffer(HDC dc, int width, int height)
	{
		if (back_dc != nullptr && back_width == width && back_height == height) return true;
		release_back_buffer();

		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;

		void* bits = nullptr;
		back_bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (back_bitmap == nullptr) return false;
		back_dc = CreateCompatibleDC(dc);
		if (back_dc == nullptr)
		{
			DeleteObject(back_bitmap);
			back_bitmap = nullptr;
			return false;
		}
		back_previous = static_cast<HBITMAP>(SelectObject(back_dc, back_bitmap));
		back_width = width;
		back_height = height;
		return true;
	}

	// Without this Windows scales the whole window as a bitmap on a high-DPI
	// display, which is what makes the picture and the menus look soft.
	void enable_dpi_awareness()
	{
		using set_awareness_context = BOOL(WINAPI*)(HANDLE);
		if (const HMODULE user32 = GetModuleHandleW(L"user32.dll"))
		{
			const auto set_context = reinterpret_cast<set_awareness_context>(
				GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
			// -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
			if (set_context != nullptr && set_context(reinterpret_cast<HANDLE>(-4))) return;
		}
		SetProcessDPIAware();
	}

	machine_key translate_key(WPARAM key, LPARAM flags)
	{
		const bool extended = (flags & 0x01000000) != 0;
		switch (key)
		{
		case VK_ESCAPE: return machine_key::escape;
		case VK_F1: return machine_key::f1;
		case VK_F2: return machine_key::f2;
		case VK_F3: return machine_key::f3;
		case VK_F4: return machine_key::f4;
		case VK_F5: return machine_key::f5;
		case VK_F6: return machine_key::f6;
		case VK_F7: return machine_key::f7;
		case VK_F8: return machine_key::f8;
		case VK_F10: return machine_key::f10;
		case VK_F11: return machine_key::f11;
		case VK_F12: return machine_key::f12;
		case VK_BACK: return machine_key::backspace;
		case VK_TAB: return machine_key::tab;
		case VK_RETURN: return extended ? machine_key::keypad_enter : machine_key::enter;
		case VK_SHIFT: return machine_key::left_shift;
		case VK_LSHIFT: return machine_key::left_shift;
		case VK_RSHIFT: return machine_key::right_shift;
		case VK_CONTROL: return extended ? machine_key::right_ctrl : machine_key::left_ctrl;
		case VK_MENU: return extended ? machine_key::right_alt : machine_key::left_alt;
		case VK_CAPITAL: return machine_key::caps_lock;
		case VK_SPACE: return machine_key::space;
		case VK_PRIOR: return machine_key::page_up;
		case VK_NEXT: return machine_key::page_down;
		case VK_END: return machine_key::end;
		case VK_HOME: return machine_key::home;
		case VK_LEFT: return machine_key::left;
		case VK_UP: return machine_key::up;
		case VK_RIGHT: return machine_key::right;
		case VK_DOWN: return machine_key::down;
		case VK_INSERT: return machine_key::insert;
		case VK_DELETE: return machine_key::del;
		case VK_NUMLOCK: return machine_key::num_lock;
		case VK_SCROLL: return machine_key::scroll_lock;
		case VK_OEM_MINUS: return machine_key::minus;
		case VK_OEM_PLUS: return machine_key::equals;
		case VK_OEM_1: return machine_key::semicolon;
		case VK_OEM_2: return machine_key::slash;
		case VK_OEM_3: return machine_key::backtick;
		case VK_OEM_4: return machine_key::left_bracket;
		case VK_OEM_5: return machine_key::backslash;
		case VK_OEM_6: return machine_key::right_bracket;
		case VK_OEM_7: return machine_key::quote;
		case VK_OEM_COMMA: return machine_key::comma;
		case VK_OEM_PERIOD: return machine_key::period;
		case VK_MULTIPLY: return machine_key::keypad_star;
		case VK_ADD: return machine_key::keypad_plus;
		case VK_SUBTRACT: return machine_key::keypad_minus;
		case VK_DIVIDE: return machine_key::keypad_slash;
		case VK_DECIMAL: return machine_key::keypad_period;
		default: break;
		}
		if (key >= '0' && key <= '9') return machine_key(int(machine_key::num1) + ((key - '0' + 9) % 10));
		if (key >= 'A' && key <= 'Z')
		{
			static const machine_key letters[26] = {
				machine_key::a, machine_key::b, machine_key::c, machine_key::d, machine_key::e, machine_key::f,
				machine_key::g, machine_key::h, machine_key::i, machine_key::j, machine_key::k, machine_key::l,
				machine_key::m, machine_key::n, machine_key::o, machine_key::p, machine_key::q, machine_key::r,
				machine_key::s, machine_key::t, machine_key::u, machine_key::v, machine_key::w, machine_key::x,
				machine_key::y, machine_key::z,
			};
			return letters[key - 'A'];
		}
		if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
			return machine_key(int(machine_key::keypad0) + int(key - VK_NUMPAD0));
		return machine_key::none;
	}

	void update_title()
	{
		char text[MAX_PATH + 128] = {};
		const machine_state& s = emulator->state();
		const char* what = "no program";
		if (s.shell.program[0] != 0) what = s.shell.program;
		else if (s.shell.active) what = "command prompt";
		snprintf(text, sizeof(text), "ovrwin - %s - mode %02Xh %dx%d%s", what,
		         s.vga.mode, s.vga.width, s.vga.height, paused ? " - paused" : "");
		SetWindowTextA(window, text);
	}

	void start_audio()
	{
		if (wave != nullptr) return;
		WAVEFORMATEX format = {};
		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = 1;
		format.nSamplesPerSec = sound_sample_rate;
		format.wBitsPerSample = 16;
		format.nBlockAlign = 2;
		format.nAvgBytesPerSec = sound_sample_rate * 2;
		if (waveOutOpen(&wave, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
		{
			wave = nullptr;
			return;
		}
		for (int i = 0; i < audio_buffers; ++i)
		{
			headers[i] = {};
			headers[i].lpData = reinterpret_cast<LPSTR>(audio[i]);
			headers[i].dwBufferLength = sizeof(audio[i]);
			waveOutPrepareHeader(wave, &headers[i], sizeof(WAVEHDR));
			headers[i].dwFlags |= WHDR_DONE;
		}
		emulator->set_audio_enabled(true);
	}

	void stop_audio()
	{
		if (wave == nullptr) return;
		waveOutReset(wave);
		for (auto& header : headers) waveOutUnprepareHeader(wave, &header, sizeof(WAVEHDR));
		waveOutClose(wave);
		wave = nullptr;
		emulator->set_audio_enabled(false);
	}

	// Submits one frame of audio. The sound card consumes at a fixed rate, so
	// waiting for a free buffer is what paces the emulation to real time.
	void submit_audio()
	{
		if (wave == nullptr) return;
		WAVEHDR& header = headers[next_buffer];
		if ((header.dwFlags & WHDR_DONE) == 0) return;
		emulator->generate_audio(audio[next_buffer], samples_per_frame);
		header.dwFlags &= ~WHDR_DONE;
		waveOutWrite(wave, &header, sizeof(WAVEHDR));
		next_buffer = (next_buffer + 1) % audio_buffers;
	}

	bool audio_ready()
	{
		if (wave == nullptr) return true;
		return (headers[next_buffer].dwFlags & WHDR_DONE) != 0;
	}

	void paint(HDC dc)
	{
		RECT client = {};
		GetClientRect(window, &client);
		const int client_width = client.right - client.left;
		const int client_height = client.bottom - client.top;
		const int width = emulator->width();
		const int height = emulator->height();
		if (width <= 0 || height <= 0) return;

		// Whole-number scaling only. A fractional factor has to drop or double
		// individual rows of pixels, which is what makes an emulated screen
		// look uneven; an integer factor cannot. The two axes get their own
		// factor, bounded so a pixel is at least square and at most twice as
		// tall as it is wide - roughly its shape on the display a 200-line
		// mode was written for.
		int scale_x = client_width / width;
		int scale_y = client_height / height;
		if (scale_x < 1) scale_x = 1;
		if (scale_y < 1) scale_y = 1;
		while (scale_x > scale_y && scale_x > 1) --scale_x;
		if (scale_y > scale_x * 2) scale_y = scale_x * 2;

		const int draw_width = width * scale_x;
		const int draw_height = height * scale_y;
		const int x = (client_width - draw_width) / 2;
		const int y = (client_height - draw_height) / 2;

		bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmap_info.bmiHeader.biWidth = width;
		bitmap_info.bmiHeader.biHeight = -height; // top-down
		bitmap_info.bmiHeader.biPlanes = 1;
		bitmap_info.bmiHeader.biBitCount = 32;
		bitmap_info.bmiHeader.biCompression = BI_RGB;

		if (!ensure_back_buffer(dc, client_width, client_height)) return;

		// Only the letterbox needs clearing; the picture covers the rest.
		const RECT bars[4] = {
			{0, 0, client_width, y},
			{0, y + draw_height, client_width, client_height},
			{0, y, x, y + draw_height},
			{x + draw_width, y, client_width, y + draw_height},
		};
		HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		for (const RECT& bar : bars)
			if (bar.right > bar.left && bar.bottom > bar.top) FillRect(back_dc, &bar, black);

		SetStretchBltMode(back_dc, COLORONCOLOR);
		StretchDIBits(back_dc, x, y, draw_width, draw_height, 0, 0, width, height,
		              emulator->framebuffer(), &bitmap_info, DIB_RGB_COLORS, SRCCOPY);
		BitBlt(dc, 0, 0, client_width, client_height, back_dc, 0, 0, SRCCOPY);
	}

	void report(const char* message)
	{
		MessageBoxA(window, message, "ovrwin", MB_OK | MB_ICONWARNING);
	}

	// Mounts a folder as the emulated drive and brings the shell up in it.
	bool mount(const char* directory)
	{
		if (!host->set_root(directory))
		{
			report("Cannot use that folder as the emulated drive.");
			return false;
		}
		stop_audio();
		emulator->start_shell();
		if (sound_on) start_audio();
		paused = false;
		update_title();
		return true;
	}

	// Accepts either a program or a folder, which is what a drop can be.
	void open_path(const char* path)
	{
		const DWORD attributes = GetFileAttributesA(path);
		if (attributes == INVALID_FILE_ATTRIBUTES)
		{
			report("Cannot open that path.");
			return;
		}

		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			mount(path);
			return;
		}

		char directory[MAX_PATH] = {};
		snprintf(directory, sizeof(directory), "%s", path);
		char* last = nullptr;
		for (char* p = directory; *p != 0; ++p) if (*p == '\\' || *p == '/') last = p;
		const char* name = path;
		if (last != nullptr)
		{
			*last = 0;
			name = path + (last - directory) + 1;
		}
		else
		{
			snprintf(directory, sizeof(directory), ".");
		}

		if (!mount(directory)) return;
		if (!emulator->run_program(name, "")) report("That file is not a DOS program.");
		update_title();
	}

	void open_program()
	{
		char path[MAX_PATH] = {};
		OPENFILENAMEA dialog = {};
		dialog.lStructSize = sizeof(dialog);
		dialog.hwndOwner = window;
		dialog.lpstrFilter = "DOS programs\0*.exe;*.com\0All files\0*.*\0";
		dialog.lpstrFile = path;
		dialog.nMaxFile = MAX_PATH;
		dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
		if (GetOpenFileNameA(&dialog)) open_path(path);
	}

	void open_folder()
	{
		BROWSEINFOA browse = {};
		char display[MAX_PATH] = {};
		browse.hwndOwner = window;
		browse.pszDisplayName = display;
		browse.lpszTitle = "Choose the folder to use as the emulated drive";
		browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
		LPITEMIDLIST chosen = SHBrowseForFolderA(&browse);
		if (chosen == nullptr) return;

		char folder[MAX_PATH] = {};
		if (SHGetPathFromIDListA(chosen, folder)) mount(folder);
		CoTaskMemFree(chosen);
	}

	LRESULT CALLBACK window_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
	{
		switch (message)
		{
		case WM_DESTROY:
			release_back_buffer();
			running = false;
			PostQuitMessage(0);
			return 0;

		case WM_PAINT:
		{
			PAINTSTRUCT paint_info = {};
			HDC dc = BeginPaint(handle, &paint_info);
			paint(dc);
			EndPaint(handle, &paint_info);
			return 0;
		}

		case WM_SIZE:
			InvalidateRect(handle, nullptr, FALSE);
			return 0;

		case 0x02e0: // WM_DPICHANGED
		{
			const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
			SetWindowPos(handle, nullptr, suggested->left, suggested->top,
			             suggested->right - suggested->left, suggested->bottom - suggested->top,
			             SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}

		case WM_ERASEBKGND:
			return 1;

		case WM_COMMAND:
			switch (LOWORD(wparam))
			{
			case menu_open: open_program(); return 0;
			case menu_open_folder: open_folder(); return 0;
			case menu_exit: DestroyWindow(handle); return 0;
			case menu_reset:
				emulator->start_shell();
				update_title();
				return 0;
			case menu_pause:
				paused = !paused;
				CheckMenuItem(menu_bar, menu_pause, paused ? MF_CHECKED : MF_UNCHECKED);
				update_title();
				return 0;
			case menu_sound:
				sound_on = !sound_on;
				CheckMenuItem(menu_bar, menu_sound, sound_on ? MF_CHECKED : MF_UNCHECKED);
				if (sound_on) start_audio();
				else stop_audio();
				return 0;
			case menu_about:
				MessageBoxA(handle,
				            "ovrwin\n\nA DOS program emulator: 8086/80186, VGA/EGA/CGA, AdLib and "
				            "PC speaker, with DOS and BIOS services mapped rather than emulated.\n\n"
				            "https://github.com/ZacWalk/ovrwin",
				            "About ovrwin", MB_OK | MB_ICONINFORMATION);
				return 0;
			default: break;
			}
			return 0;

		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if (wparam == VK_F9)
			{
				SendMessageA(handle, WM_COMMAND, menu_pause, 0);
				return 0;
			}
			emulator->key_down(translate_key(wparam, lparam));
			return 0;

		case WM_KEYUP:
		case WM_SYSKEYUP:
			emulator->key_up(translate_key(wparam, lparam));
			return 0;

		case WM_MOUSEMOVE:
		{
			RECT client = {};
			GetClientRect(handle, &client);
			const int width = client.right - client.left;
			const int height = client.bottom - client.top;
			if (width > 0 && height > 0)
				emulator->mouse_move(GET_X_LPARAM(lparam) * emulator->width() / width,
				                     GET_Y_LPARAM(lparam) * emulator->height() / height);
			return 0;
		}

		case WM_LBUTTONDOWN: emulator->mouse_button(0, true); return 0;
		case WM_LBUTTONUP: emulator->mouse_button(0, false); return 0;
		case WM_RBUTTONDOWN: emulator->mouse_button(1, true); return 0;
		case WM_RBUTTONUP: emulator->mouse_button(1, false); return 0;
		case WM_MBUTTONDOWN: emulator->mouse_button(2, true); return 0;
		case WM_MBUTTONUP: emulator->mouse_button(2, false); return 0;

		case WM_DROPFILES:
		{
			char path[MAX_PATH] = {};
			DragQueryFileA(reinterpret_cast<HDROP>(wparam), 0, path, MAX_PATH);
			DragFinish(reinterpret_cast<HDROP>(wparam));
			SetForegroundWindow(handle);
			open_path(path);
			return 0;
		}

		default:
			break;
		}
		return DefWindowProcA(handle, message, wparam, lparam);
	}

	HMENU build_menu()
	{
		HMENU bar = CreateMenu();
		HMENU file = CreatePopupMenu();
		AppendMenuA(file, MF_STRING, menu_open, "&Open Program...\tCtrl+O");
		AppendMenuA(file, MF_STRING, menu_open_folder, "Open &Folder...");
		AppendMenuA(file, MF_SEPARATOR, 0, nullptr);
		AppendMenuA(file, MF_STRING, menu_exit, "E&xit");
		AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), "&File");

		HMENU machine_menu = CreatePopupMenu();
		AppendMenuA(machine_menu, MF_STRING, menu_reset, "&Reset");
		AppendMenuA(machine_menu, MF_STRING | MF_UNCHECKED, menu_pause, "&Pause\tF9");
		AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(machine_menu), "&Machine");

		HMENU view = CreatePopupMenu();
		AppendMenuA(view, MF_STRING | MF_CHECKED, menu_sound, "&Sound");
		AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), "&View");

		HMENU help = CreatePopupMenu();
		AppendMenuA(help, MF_STRING, menu_about, "&About ovrwin");
		AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), "&Help");
		return bar;
	}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR command_line, int show)
{
	enable_dpi_awareness();

	machine emulator_instance;
	win32_host host_instance;
	emulator = &emulator_instance;
	host = &host_instance;
	emulator->set_host(host);

	WNDCLASSA window_class = {};
	window_class.lpfnWndProc = window_procedure;
	window_class.hInstance = instance;
	window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
	window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	window_class.lpszClassName = "ovrwin";
	RegisterClassA(&window_class);

	menu_bar = build_menu();

	// Now that the process is DPI aware the window size is in real pixels, so
	// scale the default so it comes up the same physical size everywhere. The
	// client is sized to show the common modes at a whole-number scale.
	const HDC screen = GetDC(nullptr);
	const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
	if (screen != nullptr) ReleaseDC(nullptr, screen);

	RECT wanted = {0, 0, MulDiv(1280, dpi, 96), MulDiv(800, dpi, 96)};
	AdjustWindowRect(&wanted, WS_OVERLAPPEDWINDOW, TRUE);

	window = CreateWindowExA(WS_EX_ACCEPTFILES, "ovrwin", "ovrwin", WS_OVERLAPPEDWINDOW,
	                         CW_USEDEFAULT, CW_USEDEFAULT, wanted.right - wanted.left,
	                         wanted.bottom - wanted.top, nullptr, menu_bar, instance, nullptr);
	if (window == nullptr) return 1;

	ACCEL accelerators[] = {
		{FVIRTKEY | FCONTROL, 'O', menu_open},
		{FVIRTKEY | FCONTROL, 'R', menu_reset},
	};
	HACCEL accelerator_table = CreateAcceleratorTable(accelerators, 2);

	DragAcceptFiles(window, TRUE);
	ShowWindow(window, show);

	if (command_line != nullptr && command_line[0] != 0)
	{
		char path[MAX_PATH] = {};
		snprintf(path, sizeof(path), "%s", command_line);
		if (path[0] == '"')
		{
			char* end = strchr(path + 1, '"');
			if (end != nullptr) *end = 0;
			open_path(path + 1);
		}
		else
		{
			open_path(path);
		}
	}
	else
	{
		// With nothing to run, mount the current directory and let the user
		// look around from the prompt.
		char here[MAX_PATH] = {};
		GetCurrentDirectoryA(MAX_PATH, here);
		mount(here);
	}

	MSG message = {};
	LARGE_INTEGER frequency = {};
	LARGE_INTEGER last = {};
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&last);

	while (running)
	{
		while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
		{
			if (message.message == WM_QUIT)
			{
				running = false;
				break;
			}
			if (!TranslateAcceleratorA(window, accelerator_table, &message))
			{
				TranslateMessage(&message);
				DispatchMessageA(&message);
			}
		}
		if (!running) break;

		// EXIT at the prompt is the only thing that ends the session; a program
		// terminating just returns to the shell.
		if (emulator->exited())
		{
			DestroyWindow(window);
			continue;
		}

		if (paused)
		{
			Sleep(16);
			continue;
		}

		// With sound on, waiting for a free wave buffer paces emulation to
		// exactly real time; without it, fall back to the wall clock.
		if (wave != nullptr && !audio_ready())
		{
			Sleep(1);
			continue;
		}
		if (wave == nullptr)
		{
			// A fixed step, not "one frame since we last finished": resetting the
			// reference to the actual time accumulates the Sleep granularity and
			// lands at about 57 unevenly spaced frames a second.
			const LONGLONG period = frequency.QuadPart / 60;
			LARGE_INTEGER now = {};
			QueryPerformanceCounter(&now);
			if (now.QuadPart - last.QuadPart < period)
			{
				Sleep(1);
				continue;
			}
			last.QuadPart += period;
			if (now.QuadPart - last.QuadPart > period * 4) last = now; // too far behind to catch up
		}

		emulator->exec(emulator->cycles_per_frame());
		submit_audio();
		InvalidateRect(window, nullptr, FALSE);

		static int title_counter = 0;
		if (++title_counter >= 30)
		{
			title_counter = 0;
			update_title();
		}
	}

	stop_audio();
	DestroyAcceleratorTable(accelerator_table);
	return 0;
}
