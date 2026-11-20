#pragma once

#include <array>

#include <QIcon>
#include <QPixmap>

namespace fv::ui {

// Sizes Windows asks for across Explorer, the taskbar, alt-tab and tooltips.
inline constexpr std::array<int, 7> kAppIconSizes{16, 24, 32, 48, 64, 128, 256};

// The app mark at size x size pixels on a transparent background: a blue-to-cyan rounded
// square with a white microphone, a filled capsule below 32 px (an outline closes up there) and
// an outlined capsule with stem and base from 32 px. Every coordinate is a fraction of size.
QPixmap renderAppPixmap(int size);

// The application icon: the mark drawn at every size of kAppIconSizes, built once. No bundled
// .ico is read; the mark is drawn in code at every size.
QIcon appIcon();

} // namespace fv::ui
