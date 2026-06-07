#include <cassert>
#include <generator>
#include <optional>
#include <print>
#include <stdexcept>
#include <string.h>
#include <string>
#include <variant>

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

using namespace std::string_literals;

template<class... Ts>
struct match : Ts...
{
	using Ts::operator()...;
};

template <typename T, typename... Fs>
constexpr decltype(auto) operator | (T const &v, match<Fs...> const &match)
{
	using std::visit;
    return visit(match, v);
}

template <typename T, typename... Fs>
constexpr decltype(auto) operator | (T &v, match<Fs...> const &match)
{
	using std::visit;
    return visit(match, v);
}


template<typename IntType>
IntType left_shift_fill_lsb(IntType value, int shift_amount)
{
	IntType fill = 0;
	if (value & 1)
		fill = (1 << shift_amount) - 1;

	return (value << shift_amount) | fill;
}


namespace xcb {

char const* connection_error_description(int error)
{
	switch (error)
	{
		case XCB_CONN_ERROR: return "socket error, pipe error or other stream error";
		case XCB_CONN_CLOSED_EXT_NOTSUPPORTED: return "extension not supported";
		case XCB_CONN_CLOSED_MEM_INSUFFICIENT: return "insufficient memory";
		case XCB_CONN_CLOSED_REQ_LEN_EXCEED: return "request rejected by server due to being too large";
		case XCB_CONN_CLOSED_PARSE_ERR: return "parsing display string failed";
		case XCB_CONN_CLOSED_INVALID_SCREEN: return "server does not have a screen matching the display";
		default: return "unknown error";
	}
}

struct Connection
{
	explicit Connection(char const *display_name = nullptr) :
		inner{xcb_connect(display_name, &preferred_screen_idx)}
	{
		if (int error = xcb_connection_has_error(inner))
			throw std::runtime_error("connecting to X server failed: "s + connection_error_description(error));

		xcb_intern_atom_cookie_t *c = xcb_ewmh_init_atoms(inner, &ewmh);
		if (not xcb_ewmh_init_atoms_replies(&ewmh, c, nullptr))
			throw std::runtime_error("initializing EWMH failed");
	}

	~Connection()
	{
		xcb_disconnect(inner);
	}

	xcb_screen_t preferred_screen()
	{
		xcb_setup_t const *setup = xcb_get_setup(inner);
		xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(setup);

		// screen_iter.rem refers to the number of remaining screens
		if (screen_iter.rem < preferred_screen_idx)
			throw std::runtime_error("server provided invalid preferred screen");

		for (int screen_idx = 0; screen_idx < preferred_screen_idx; ++screen_idx)
			xcb_screen_next(&screen_iter);

		return *screen_iter.data;
	}

	xcb_atom_t retrieve_atom(std::string_view atom_name)
	{
		xcb_intern_atom_cookie_t cookie = xcb_intern_atom(inner, 0, atom_name.length(), atom_name.data());
		xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(inner, cookie, nullptr);
		if (not reply)
			throw std::runtime_error("retrieving atom \"" + std::string(atom_name) + "\" failed");

		xcb_atom_t atom = reply->atom;
		free(reply);

		return atom;
	}

	// The cookie must come from one of the xcb_{...}_checked() functions
	void throw_on_error(xcb_void_cookie_t cookie, std::string const &msg)
	{
		xcb_generic_error_t *err = xcb_request_check(inner, cookie);
		if (err)
			throw std::runtime_error(msg + ": " + std::to_string(err->error_code));
	}

	xcb_connection_t *inner;
	// Only used to store a bunch of atoms, could do it myself and avoid linking against libxcb-ewmh
	xcb_ewmh_connection_t ewmh;
	int preferred_screen_idx;
};


struct SignalEvent
{
	int signum;
};

using Event = std::variant<xcb_generic_event_t*, SignalEvent>;

struct EventHandler
{
	constexpr static int signals_to_handle[] = {SIGINT, SIGTERM};

	explicit EventHandler(Connection &conn) :
		conn{conn}
	{
		// Block signals in signals_to_handle so we can read them via signalfd
		sigset_t sset;
		sigemptyset(&sset);
		for(int signum: signals_to_handle)
			sigaddset(&sset, signum);

		if (pthread_sigmask(SIG_SETMASK, &sset, nullptr) != 0)
			throw std::runtime_error("pthread_sigmask failed");

		int signal_fd = signalfd(-1, &sset, SFD_NONBLOCK | SFD_CLOEXEC);
		if (signal_fd == -1)
			throw std::runtime_error("signalfd failed");

		int conn_fd = xcb_get_file_descriptor(conn.inner);
		poll_fds = {
			.signals = {.fd = signal_fd, .events = POLLIN, .revents = 0},
			.xcb = {.fd = conn_fd, .events = POLLIN, .revents = 0},
		};
	}

	// There is usually only one EventHandler for the entire runtime of the application so we don't
	// bother unblocking signals and closing file descriptors in the destructor

	void request_window_events(xcb_window_t window, uint32_t events)
	{
		xcb_change_window_attributes(conn.inner, window, XCB_CW_EVENT_MASK, &events);
		xcb_flush(conn.inner);
	}

	std::generator<Event> events()
	{
		for (;;)
		{
			wait_for_events();

			if (poll_fds.signals.revents & POLLIN)
			{
				for (;;)
				{
					struct signalfd_siginfo fdsi;
					ssize_t bytes_read = read(poll_fds.signals.fd, &fdsi, sizeof(fdsi));
					if (bytes_read == -1)
					{
						if (errno == EAGAIN)
							break;
						else
							throw std::runtime_error("reading from signalfd failed");
					}
					assert(bytes_read == sizeof(fdsi));
					co_yield SignalEvent(fdsi.ssi_signo);
				}
			}

			if (poll_fds.xcb.revents & POLLIN)
			{
				while (xcb_generic_event_t *event = xcb_poll_for_event(conn.inner))
				{
					co_yield event;
					free(event);
				}
			}
		}
	}

	void wait_for_events()
	{
		for (;;)
		{
			// Wait for either signalfd or xcb to become ready
			int result = poll((struct pollfd*)&poll_fds, PollFds::count, -1);
			if (result == -1)
			{
				if (errno == EINTR)
					continue;

				throw std::runtime_error("poll failed");
			}
			else
				break;
		}
	}

	struct PollFds
	{
		constexpr static size_t count = 2;
		struct pollfd signals;
		struct pollfd xcb;
	};

	Connection &conn;
	PollFds poll_fds;
};

struct CreateWindowOptions
{
	xcb_window_t parent;
	xcb_visualid_t visual;
	uint32_t background_pixel;
	int16_t x, y;
	uint16_t width, height;
	bool bypass_window_manager = false;
};

xcb_window_t create_window(Connection &conn, CreateWindowOptions opts)
{
	// Background is used by xcb_clear_area()
	uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
	uint32_t value_list[] = {
		opts.background_pixel,
		opts.bypass_window_manager,
	};

	xcb_window_t window = xcb_generate_id(conn.inner);
	xcb_void_cookie_t c = xcb_create_window_checked(
		conn.inner,
		XCB_COPY_FROM_PARENT, // depth
		window,
		opts.parent,
		opts.x, opts.y,
		opts.width, opts.height,
		0, // border width
		XCB_WINDOW_CLASS_INPUT_OUTPUT,
		opts.visual,
		value_mask,
		value_list
	);
	conn.throw_on_error(c, "creating window failed");

	return window;
}

// Configuring the properites of a window is very flexible by using a list of key-value pairs. The
// keys are called *atoms* and are simple integer IDs represented as uint32_t. Given a name of an
// atom like WM_NAME (representing a window's title), the corresponding ID can be requested using
// xcb_intern_atom(). Once you have the ID, the value associated with the atom can be changed using
// xcb_change_property().
//
// The X protocol itself only defines a couple of atoms. (Actually, it only defines their IDs, but
// not their semantics.) More are defined by separate specifications:
// - ICCCM (Inter-Client Communication Conventions Manual)
//   - Atoms: WM_NAME, ...
//   - XCB utility functions: xcb/xcb_icccm.h
//   - Spec: https://xorg.freedesktop.org/archive/X11R7.7/doc/xorg-docs/icccm/icccm.html
// - EWMH (Extended Window Manager Hints)
//   - Spec: https://specifications.freedesktop.org/wm/latest/
//   - XCB utility functions: xcb/xcb_ewmh.h
// - And then there are those atoms that were intoduced by someone in ancient times and that remain
//   supported because no one proposed an alternative that caught on
//   - _MOTIF_WM_HINTS
//     - Allows various customizations, but the main thing seems to be hiding the title bar (which,
//       it seems, neither ICCCM nor EWMH supports)
//     - Every mention of this atom complains that there is no documentation for it
//       - https://stackoverflow.com/questions/13787553/detect-if-a-x11-window-has-decorations
//       - Gnome Metacity: xprops.h (https://github.com/GNOME/metacity/blob/master/src/include/xprops.h)
//       - Patch for i3: https://cr.i3wm.org/patch/379/
//
// You can use the `xprop` CLI tool to query the atoms of a window by clicking on it.

xcb_atom_t enable_delete_window_event(Connection &conn, xcb_window_t window)
{
	xcb_atom_t wm_protocols = conn.retrieve_atom("WM_PROTOCOLS");
	xcb_atom_t wm_delete_window_atom = conn.retrieve_atom("WM_DELETE_WINDOW");

	// Add WM_DELETE_WINDOW to WM_PROTOCOLS, expressing our interest in being notified when the
	// window is closed
	xcb_void_cookie_t change_cookie = xcb_change_property_checked(
		conn.inner,
		XCB_PROP_MODE_REPLACE,
		window,
		wm_protocols,          // What property (aka atom) we want to change.
		XCB_ATOM_ATOM,         // The type of the property we want to change (here, WM_PROTOCOLS is a
							   // property that itself stores properties).
		32,                    // Format of `data` (the last argument). Here, `data` contains 32-bit values.
		1,                     // Number of values contained in `data`.
		&wm_delete_window_atom // `data`: the value that we want the property to be changed to.
	);
	conn.throw_on_error(change_cookie, "add WM_DELETE_WINDOW to WM_PROTOCOLS failed");

	return wm_delete_window_atom;
}

void set_window_title(Connection &conn, xcb_window_t window, std::string_view title)
{
	xcb_void_cookie_t c = xcb_change_property_checked(
		conn.inner,
		XCB_PROP_MODE_REPLACE,
		window,
		XCB_ATOM_WM_NAME, // What property (aka atom) we want to change.
		XCB_ATOM_STRING,  // The type of the property we want to change.
		8,                // Format of `data` (the last argument). Here, `data` contains 8-bit chars.
		title.length(),   // Number of values contained in `data`.
		title.data()      // `data`: the value that we want the property to be changed to.
	);
	conn.throw_on_error(c, "changing window title failed");
}

// See X11 spec, "ConfigureWindow" request
enum class StackMode
{
	// "The window is placed at the top of the stack"
	ABOVE = XCB_STACK_MODE_ABOVE,
	// "The window is placed at the bottom of the stack"
	BELOW = XCB_STACK_MODE_BELOW,
	// "If any sibling occludes the window, then the window is placed at the top of the stack."
	TOP_IF = XCB_STACK_MODE_TOP_IF,
	// "If the window occludes any sibling, then the window is placed at the bottom of the stack."
	BOTTOM_IF = XCB_STACK_MODE_BOTTOM_IF,
	// "If any sibling occludes the window, then the window is placed at the top of the stack.
	// Otherwise, if the window occludes any sibling, then the window is placed at the bottom of the
	// stack."
	OPPOSITE = XCB_STACK_MODE_OPPOSITE,
};

void set_window_stack_mode(Connection &conn, xcb_window_t window, StackMode mode)
{
	xcb_void_cookie_t c = xcb_configure_window_checked(
		conn.inner,
		window,
		XCB_CONFIG_WINDOW_STACK_MODE,
		(uint32_t const*)&mode
	);
	conn.throw_on_error(c, "changing window stack mode failed");
}

struct WindowDecoration
{
	const static WindowDecoration NONE;

	bool title_bar = true;
	bool border = true;
};

const WindowDecoration WindowDecoration::NONE = {
	.title_bar = false,
	.border = false,
};

void set_window_decoration(Connection &conn, xcb_window_t window, WindowDecoration decor)
{
	// See xprops.h of Gnome Metacity
	enum Motif_WMHints_Flags
	{
		FUNCTIONS   = 1 << 0,
		DECORATIONS = 1 << 1,
		INPUT_MODE  = 1 << 2,
		STATUS      = 1 << 3,
	};

	enum Motif_WMHints_Decor
	{
		ALL      = 1 << 0,
		BORDER   = 1 << 1,
		RESIZEH  = 1 << 2,
		TITLE    = 1 << 3,
		MENU     = 1 << 4,
		MINIMIZE = 1 << 5,
		MAXIMIZE = 1 << 6,
	};

	// After some experimentation it seems to work as follows:
	// - If Motif_WMHints_Decor::ALL is set, then setting any other decoration bit is substractive.
	//   Thus, ALL | BORDER means "show all decorations except for the border"
	// - If Motif_WMHints_Decor::ALL is not set, then setting any other decoration bit is additive.
	//   Thus, specifying only BORDER means "show only the border but not any other decoration"
	uint32_t decor_prop = Motif_WMHints_Decor::ALL;
	if (not decor.border)
		decor_prop |= Motif_WMHints_Decor::BORDER;
	if (not decor.title_bar)
		decor_prop |= Motif_WMHints_Decor::TITLE;

	uint32_t motif_wm_hints[5] = {
		// The first element specifies which property we want to modify.
		// (I guess the values for all the other properties are ignored.)
		Motif_WMHints_Flags::DECORATIONS,
		0,          // Property: functions
		decor_prop, // Property: decorations
		0,          // Property: input mode
		0,          // Property: status mode
	};

	xcb_atom_t _MOTIF_WM_HINTS = conn.retrieve_atom("_MOTIF_WM_HINTS");
	xcb_void_cookie_t set_window_hints_cookie = xcb_change_property_checked(
		conn.inner,
		XCB_PROP_MODE_REPLACE,
		window,
		_MOTIF_WM_HINTS,           // What property (aka atom) we want to change.
		XCB_ATOM_INTEGER,          // The type of the property we want to change.
		32,                        // Format of `data` (the last argument). Here, `data` contains 32-bit integers.
		std::size(motif_wm_hints), // Number of values contained in `data`.
		motif_wm_hints             // `data`: the value that we want the property to be changed to.
	);
	conn.throw_on_error(set_window_hints_cookie, "changing _MOTIF_WM_HINTS failed");
}

// See EWMH spec, "_NET_WM_WINDOW_TYPE" property:
// https://specifications.freedesktop.org/wm/latest/ar01s05.html#id-1.6.7
enum class WindowType
{
	DESKTOP,
	DOCK,
	TOOLBAR,
	MENU,
	UTILITY,
	SPLASH,
	DIALOG,
	DROPDOWN_MENU,
	POPUP_MENU,
	TOOLTIP,
	NOTIFICATION,
	COMBO,
	DND,
	NORMAL,
};

void set_window_type(Connection &conn, xcb_window_t window, WindowType type)
{
	uint32_t window_type_atom;
	switch (type)
	{
		case WindowType::DESKTOP:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_DESKTOP;
			break;
		case WindowType::DOCK:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_DOCK;
			break;
		case WindowType::TOOLBAR:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_TOOLBAR;
			break;
		case WindowType::MENU:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_MENU;
			break;
		case WindowType::UTILITY:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_UTILITY;
			break;
		case WindowType::SPLASH:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_SPLASH;
			break;
		case WindowType::DIALOG:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_DIALOG;
			break;
		case WindowType::DROPDOWN_MENU:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_DROPDOWN_MENU;
			break;
		case WindowType::POPUP_MENU:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_POPUP_MENU;
			break;
		case WindowType::TOOLTIP:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_TOOLTIP;
			break;
		case WindowType::NOTIFICATION:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_NOTIFICATION;
			break;
		case WindowType::COMBO:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_COMBO;
			break;
		case WindowType::DND:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_DND;
			break;
		case WindowType::NORMAL:
			window_type_atom = conn.ewmh._NET_WM_WINDOW_TYPE_NORMAL;
			break;
	}

	xcb_void_cookie_t c = xcb_change_property_checked(
		conn.inner,
		XCB_PROP_MODE_REPLACE,
		window,
		conn.ewmh._NET_WM_WINDOW_TYPE, // What property (aka atom) we want to change.
		XCB_ATOM_ATOM,                 // The type of the property we want to change.
		32,                            // Format of `data` (the last argument). Here, `data` contain a 32-bit value.
		1,                             // Number of values contained in `data`.
		&window_type_atom              // `data`: the value that we want the property to be changed to.
	);
	conn.throw_on_error(c, "changing _NET_WM_WINDOW_TYPE failed");
}

// See EWMH spec, "_NET_WM_STATE" property:
// https://specifications.freedesktop.org/wm/latest/ar01s05.html#id-1.6.8
enum class WindowState
{
	MODAL,
	STICKY,
	MAXIMIZED_VERT,
	MAXIMIZED_HORZ,
	SHADED,
	SKIP_TASKBAR,
	SKIP_PAGER,
	HIDDEN,
	FULLSCREEN,
	ABOVE,
	BELOW,
	DEMANDS_ATTENTION,
	FOCUSED,
};

void set_window_state(
	Connection &conn,
	xcb_window_t root_window,
	xcb_window_t target_window,
	WindowState state,
	bool enable_state
)
{
	uint32_t window_state_atom = 0;
	switch (state)
	{
		case WindowState::MODAL:
			window_state_atom = conn.ewmh._NET_WM_STATE_MODAL;
			break;
		case WindowState::STICKY:
			window_state_atom = conn.ewmh._NET_WM_STATE_STICKY;
			break;
		case WindowState::MAXIMIZED_VERT:
			window_state_atom = conn.ewmh._NET_WM_STATE_MAXIMIZED_VERT;
			break;
		case WindowState::MAXIMIZED_HORZ:
			window_state_atom = conn.ewmh._NET_WM_STATE_MAXIMIZED_HORZ;
			break;
		case WindowState::SHADED:
			window_state_atom = conn.ewmh._NET_WM_STATE_SHADED;
			break;
		case WindowState::SKIP_TASKBAR:
			window_state_atom = conn.ewmh._NET_WM_STATE_SKIP_TASKBAR;
			break;
		case WindowState::SKIP_PAGER:
			window_state_atom = conn.ewmh._NET_WM_STATE_SKIP_PAGER;
			break;
		case WindowState::HIDDEN:
			window_state_atom = conn.ewmh._NET_WM_STATE_HIDDEN;
			break;
		case WindowState::FULLSCREEN:
			window_state_atom = conn.ewmh._NET_WM_STATE_FULLSCREEN;
			break;
		case WindowState::ABOVE:
			window_state_atom = conn.ewmh._NET_WM_STATE_ABOVE;
			break;
		case WindowState::BELOW:
			window_state_atom = conn.ewmh._NET_WM_STATE_BELOW;
			break;
		case WindowState::DEMANDS_ATTENTION:
			window_state_atom = conn.ewmh._NET_WM_STATE_DEMANDS_ATTENTION;
			break;
		case WindowState::FOCUSED:
			assert("WindowState::FOCUSED cannot be set by clients");
			break;
	}

	xcb_client_message_event_t *event = (xcb_client_message_event_t*)malloc(32);
	event->response_type = XCB_CLIENT_MESSAGE;
	event->format = 32;
	event->sequence = 0; // I assume
	event->window = target_window;
	event->type = conn.ewmh._NET_WM_STATE;
	event->data.data32[0] = enable_state ? XCB_EWMH_WM_STATE_ADD : XCB_EWMH_WM_STATE_REMOVE;
	event->data.data32[1] = window_state_atom;
	event->data.data32[2] = 0; // this slot can be used to set another window state at the same time
	event->data.data32[3] = 0;
	event->data.data32[4] = 0;

	xcb_void_cookie_t c = xcb_send_event_checked(
		conn.inner,
		false,
		root_window,
		// I'm using the same event mask as [here](https://github.com/psychon/x11rb/discussions/929),
		// but I've no idea why these events specifically. It's not mentioned in any documentation
		// I've found, but apparently they are needed or the event is not delivered.
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
		(char const*)event
	);
	conn.throw_on_error(c, "changing _NET_WM_STATE failed");
}

uint32_t allocate_color(
	Connection &conn,
	xcb_colormap_t colormap,
	uint8_t red,
	uint8_t green,
	uint8_t blue
)
{
	xcb_alloc_color_cookie_t color_cookie = xcb_alloc_color(
		conn.inner,
		colormap,
		// Scale colors to uint16_t
		left_shift_fill_lsb(uint16_t(red), 8),
		left_shift_fill_lsb(uint16_t(green), 8),
		left_shift_fill_lsb(uint16_t(blue), 8)
	);
	xcb_alloc_color_reply_t *color_reply = xcb_alloc_color_reply(conn.inner, color_cookie, nullptr);
	if (not color_reply)
		throw std::runtime_error("allocating color failed");

	uint32_t color = color_reply->pixel;
	free(color_reply);

	return color;
}


struct DrawableInfo
{
	int width;
	int height;
	int depth;
};

DrawableInfo get_drawable_info(Connection &conn, xcb_drawable_t drawable)
{
	xcb_get_geometry_cookie_t cookie = xcb_get_geometry(conn.inner, drawable);
	xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(conn.inner, cookie, nullptr);
	if (not reply)
		throw std::runtime_error("xcb_get_geometry_reply failed");

	DrawableInfo info = {
		.width = reply->width,
		.height = reply->height,
		.depth = reply->depth,
	};
	free(reply);

	return info;
}

struct Renderer
{
	Renderer(Connection &conn, xcb_screen_t const &screen, xcb_drawable_t target) :
		conn{conn},
		target{target},
		target_info{get_drawable_info(conn, target)},
		gc{xcb_generate_id (conn.inner)}
	{
		xcb_create_gc(conn.inner, gc, target, XCB_GC_FOREGROUND, &screen.black_pixel);
	}

	void set_foreground_color(uint32_t color_id)
	{
		xcb_void_cookie_t c = xcb_change_gc_checked(conn.inner, gc, XCB_GC_FOREGROUND, &color_id);
		conn.throw_on_error(c, "setting foreground color failed");
	}

	void resize(int new_width, int new_height)
	{
		target_info.width = new_width;
		target_info.height = new_height;
	}

	void fill_rectangle(xcb_rectangle_t const &rect)
	{
		xcb_poly_fill_rectangle(conn.inner, target, gc, 1, &rect);
	}

	void clear()
	{
		xcb_clear_area(
			conn.inner, 0, target,
			0, 0, // x, y
			target_info.width, target_info.height
		);
	}

	void clear_wait()
	{
		xcb_void_cookie_t c = xcb_clear_area_checked(
			conn.inner, 0, target,
			0, 0, // x, y
			target_info.width, target_info.height
		);
		// Wait until we receive an answer from the server indicating that xcb_clear_area() has taken
		// effect. xcb_flush() is not enough as it only ensures that the request has been send but does
		// not wait until it has been processed.
		xcb_request_check(conn.inner, c);
	}

	Connection &conn;
	xcb_drawable_t target;
	DrawableInfo target_info;
	xcb_gcontext_t gc;
};

} // namespace: xcb


constexpr int smile_char_width = 75;
constexpr int smile_char_height = 26;
constexpr char smile[] =
	R"(                          oooo$$$$$$$$$$$$oooo                             )"
	R"(                      oo$$$$$$$$$$$$$$$$$$$$$$$$o                          )"
	R"(                   oo$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$o         o$   $$ o$    )"
	R"(   o $ oo        o$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$o       $$ $$ $$o$   )"
	R"(oo $ $ "$      o$$$$$$$$$    $$$$$$$$$$$$$    $$$$$$$$$o       $$$o$$o$    )"
	R"("$$$$$$o$     o$$$$$$$$$      $$$$$$$$$$$      $$$$$$$$$$o    $$$$$$$$     )"
	R"(  $$$$$$$    $$$$$$$$$$$      $$$$$$$$$$$      $$$$$$$$$$$$$$$$$$$$$$$     )"
	R"(  $$$$$$$$$$$$$$$$$$$$$$$    $$$$$$$$$$$$$    $$$$$$$$$$$$$$  """$$$       )"
	R"(   "$$$""""$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$     "$$$      )"
	R"(    $$$   o$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$     "$$$o    )"
	R"(   o$$"   $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$       $$$o   )"
	R"(   $$$    $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$" "$$$$$$ooooo$$$$o )"
	R"(  o$$$oooo$$$$$  $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$   o$$$$$$$$$$$$$$$$$)"
	R"(  $$$$$$$$"$$$$   $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$     $$$$""""""""      )"
	R"( """"       $$$$    "$$$$$$$$$$$$$$$$$$$$$$$$$$$$"      o$$$               )"
	R"(            "$$$o     """$$$$$$$$$$$$$$$$$$"$$"         $$$                )"
	R"(              $$$o          "$$""$$$$$$""""           o$$$                 )"
	R"(               $$$$o                                o$$$"                  )"
	R"(                "$$$$o      o$$$$$$o"$$$$o        o$$$$                    )"
	R"(                  "$$$$$oo     ""$$$$o$$$$$o   o$$$$""                     )"
	R"(                     ""$$$$$oooo  "$$$o$$$$$$$$$"""                        )"
	R"(                        ""$$$$$$$oo $$$$$$$$$$                             )"
	R"(                                """"$$$$$$$$$$$                            )"
	R"(                                    $$$$$$$$$$$$                           )"
	R"(                                     $$$$$$$$$$"                           )"
	R"(                                      "$$$""""                             )";

static_assert(std::size(smile) == smile_char_width * smile_char_height + 1); // +1 for null terminator

void render(xcb::Connection &conn, xcb::Renderer renderer)
{
	renderer.clear();

	const int cell_margin = 5;
	// Make the smile occupy 60% of the screen width
	const int smile_pixel_width = renderer.target_info.width * 0.6;
	// When rendered as ASCII, the smile is roughly square. However, monospace glyphs are taller
	// than they are wide so we need to adjust the height
	const int smile_pixel_height = smile_pixel_width * 0.7;
	const int cell_width = std::max(1, (smile_pixel_width - (cell_margin * (smile_char_width-1))) / smile_char_width);
	const int cell_height = std::max(1, (smile_pixel_height - (cell_margin * (smile_char_height-1))) / smile_char_height);

	// Top-left pixel coordinates of the smile
	const int top_left_x = (renderer.target_info.width - smile_pixel_width) / 2;
	const int top_left_y = (renderer.target_info.height - smile_pixel_height) / 2;

	for (int ycell = 0; ycell < smile_char_height; ++ycell)
	{
		for (int xcell = 0; xcell < smile_char_width; ++xcell)
		{
			if (smile[ycell * smile_char_width + xcell] != ' ')
			{
				xcb_rectangle_t rect{
					.x = int16_t(top_left_x + xcell * (cell_width + cell_margin)),
					.y = int16_t(top_left_y + ycell * (cell_height + cell_margin)),
					.width = uint16_t(cell_width),
					.height = uint16_t(cell_height),
				};
				renderer.fill_rectangle(rect);
			}
		}
	}

	xcb_flush(conn.inner);
}

std::optional<std::string_view> next_arg(char** &argv)
{
	if (not *argv)
		return std::nullopt;

	return *argv++;
}

// i3wm ignores any operation affecting the stacking order
// - _NET_WM_STATE_{ABOVE|BELOW}: https://github.com/i3/i3/issues/4265
// - XCB_CONFIG_WINDOW_STACK_MODE is ignored

int main(int, char *argv[])
{
	// Instead of drawing the smile directly onto the desktop, draw it into a newly created window
	bool arg_create_window = false;
	// --- The following options only apply when using arg_create_window ---
	// Remove title bar and borders
	bool arg_no_decor = false;
	// Make the window fullscreen
	bool arg_fullscreen = false;
	// Request that the window is treated as a dock/panel. For example, i3 automatically positions
	// these kind of windows automatically at the top or bottom of the screen at full width (like
	// i3bar)
	bool arg_dock = false;
	// Bypassing the window manager means the window won't have any decorations any cannot be
	// moved/resized
	bool arg_bypass_window_manager = false;


	next_arg(argv);
	while(std::optional<std::string_view> arg = next_arg(argv))
	{
		if (arg == "--window")
			arg_create_window = true;
		else if (arg == "--no-decor")
			arg_no_decor = true;
		else if (arg == "--fullscreen")
			arg_fullscreen = true;
		else if (arg == "--dock")
			arg_dock = true;
		else if (arg == "--bypass-wm")
			arg_bypass_window_manager = true;
		else
		{
			std::println(stderr, "Invalid option: {}", *arg);
			return 1;
		}
	}

	xcb::Connection conn;
	xcb::EventHandler handler(conn);
	xcb_screen_t screen = conn.preferred_screen();
	xcb_window_t window;
	std::optional<xcb_atom_t> wm_delete_window_atom;
	if (arg_create_window)
	{
		uint32_t background = allocate_color(conn, screen.default_colormap, 50, 50, 50);
		window = xcb::create_window(conn, {
			.parent = screen.root,
			.visual = screen.root_visual,
			.background_pixel = background,
			.x = 100,
			.y = 100,
			.width = 1000,
			.height = 1000,
			.bypass_window_manager = arg_bypass_window_manager
		});
		wm_delete_window_atom = xcb::enable_delete_window_event(conn, window);
		xcb::set_window_title(conn, window, "smile");

		if (arg_no_decor)
			xcb::set_window_decoration(conn, window, xcb::WindowDecoration::NONE);

		if (arg_dock)
			xcb::set_window_type(conn, window, xcb::WindowType::DOCK);

		xcb_map_window(conn.inner, window);

		// Setting a window state must be done after the window has been mapped or it will have no
		// effect
		if (arg_fullscreen)
			xcb::set_window_state(conn, screen.root, window, xcb::WindowState::FULLSCREEN, true);

		handler.request_window_events(
			screen.root,
			// Needed for the CreateNotify event (note that we are requesting the event on the
			// *parent* window)
			XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
		);
	}
	else
	{
		window = screen.root;
	}

	handler.request_window_events(window,
		XCB_EVENT_MASK_EXPOSURE |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY
	);

	xcb::Renderer renderer(conn, screen, window);
	uint32_t smile_color = allocate_color(conn, screen.default_colormap, 200, 200, 0);
	renderer.set_foreground_color(smile_color);
	render(conn, renderer);
	xcb_flush(conn.inner);
	for (xcb::Event event: handler.events())
	{
		bool cont = event | match
		{
			[&](xcb::SignalEvent e)
			{
				if (e.signum == SIGINT || e.signum == SIGTERM)
					return false;

				return true;
			},
			[&](xcb_generic_event_t const *e)
			{
				switch (e->response_type & 0x7f)
				{
					case XCB_EXPOSE:
					{
						render(conn, renderer);
					} break;

					case XCB_CREATE_NOTIFY:
					{
						// Window was created.
						// Requires XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY on the *parent* window.
					} break;

					case XCB_CONFIGURE_NOTIFY:
					{
						xcb_configure_notify_event_t *resize_event = (xcb_configure_notify_event_t *)e;
						if (resize_event->window == window)
							renderer.resize(resize_event->width, resize_event->height);
					} break;

					case XCB_CLIENT_MESSAGE:
					{
						xcb_client_message_event_t *client_event = (xcb_client_message_event_t *)e;
						if (wm_delete_window_atom and client_event->data.data32[0] == *wm_delete_window_atom)
						{
							xcb_destroy_window(conn.inner, window);
							return false;
						}
					} break;
				}

				return true;
			},
		};

		if (not cont)
			break;
	}

	// Remove our art from the desktop
	renderer.clear_wait();
}
