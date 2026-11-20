#pragma once

// Where each feature's tab, side panel, menu item and tray item goes. Features register with
// these constants, so the order is OpenWhisper's whichever feature installs first. Gaps of 100
// leave room for OpenWhisper_CPP-only items. A separator constant marks the separator that sits at
// that position.

namespace fv::order {

// The main window's tabs (ui_qt/widgets/tabbed_content.py). Meeting Mode stays last: the
// theme's ::tab:last rules grey it on unsupported platforms.
namespace tab {
inline constexpr int quick_record = 100;
inline constexpr int upload_file = 200;
inline constexpr int walkthrough = 300;
inline constexpr int meeting_mode = 400;
} // namespace tab

// The context side panel, one shown at a time (ui_qt/widgets/history_sidebar.py's modes):
// History for Quick Record and Upload File, Walkthroughs for Walkthrough, Past Meetings for
// Meeting Mode.
namespace side_panel {
inline constexpr int history = 100;
inline constexpr int past_meetings = 200;
inline constexpr int walkthroughs = 300;
} // namespace side_panel

// The title bar's File menu (ui_qt/main_window.py, _setup_menu).
namespace file_menu {
inline constexpr int settings = 100;
inline constexpr int guided_setup = 200;
inline constexpr int downloads = 300;
inline constexpr int hotkeys = 400;
inline constexpr int separator_before_tray = 500;
inline constexpr int minimize_to_tray = 600;
inline constexpr int exit = 700;
} // namespace file_menu

// The View menu.
namespace view_menu {
inline constexpr int side_panel = 100; // "History", or "Past Meetings" on the Meeting tab
inline constexpr int history_calendar = 200;
inline constexpr int compact_mode = 300;
inline constexpr int host_mode = 400;
inline constexpr int separator_before_windows = 500;
inline constexpr int scratchpad = 600;
inline constexpr int stats = 700;
inline constexpr int separator_before_dashboard = 800;
inline constexpr int open_meeting_dashboard = 900;
} // namespace view_menu

// The Help menu.
namespace help_menu {
inline constexpr int check_for_updates = 100;
inline constexpr int about = 200;
} // namespace help_menu

// The system tray menu (ui_qt/system_tray.py, _setup_menu).
namespace tray {
inline constexpr int show = 100;
inline constexpr int hide = 200;
inline constexpr int separator_before_recording = 300;
inline constexpr int toggle_recording = 400; // "Start Recording" / "Stop Recording"
inline constexpr int copy_original = 500;    // "Copy original of last dictation"
inline constexpr int walkthrough = 600;      // "Start Walkthrough" / "Finish Walkthrough"
inline constexpr int take_screenshot = 700;
inline constexpr int language = 800; // submenu
inline constexpr int separator_before_meeting = 900;
inline constexpr int meeting = 1000; // "Start Meeting" / "End Meeting"
inline constexpr int open_meeting_dashboard = 1100;
inline constexpr int separator_before_windows = 1200;
inline constexpr int scratchpad = 1300;
inline constexpr int stats = 1400;
inline constexpr int settings = 1500;
inline constexpr int separator_before_exit = 1600;
inline constexpr int update = 1700; // "Update to {v}…", visible only while one waits
inline constexpr int exit = 1800;
} // namespace tray

// Settings pages follow the rail order of appkit/destinations.h (destinations::rail).

} // namespace fv::order
