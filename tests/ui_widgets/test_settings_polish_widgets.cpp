// The shared Settings building blocks (test_settings_polish.py): switches on setting tiles,
// compact buttons chosen by property, segmented bars, labels that break long values and button
// rows that wrap instead of overlapping.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QRect>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest/QTest>

#include "ui_theme/style_props.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace {

const QString kCommit = QStringLiteral("d90ca5fe260221311c53c58e660288d3deb8d356");
const QString kFolder =
    QStringLiteral("C:") +
    QStringLiteral(
        R"(\Users\Big D\Documents\openwhisper-models\faster-whisper-large-v3-turbo-ct2\model.bin)");

void pump(int times = 5) {
    for (int i = 0; i < times; ++i) {
        QApplication::processEvents();
    }
}

// _narrow: a label fixed at `width` and shown.
std::unique_ptr<fv::WrappedLabel> narrow(const QString& value, bool breaking, int width = 130) {
    auto label = std::make_unique<fv::WrappedLabel>(value, nullptr, breaking);
    label->setFixedWidth(width);
    label->show();
    pump(1);
    return label;
}

std::string themeSheet() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "src" / "ui_theme" / "resources" / "theme.qss";
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// _link_row: a ButtonRow of three neutral buttons in a column `width` px wide. The column is fixed
// as Downloads' inspector is: a top-level widget cannot be resized below its layout's minimum,
// which would hide the very squeeze under test.
struct LinkRow {
    std::unique_ptr<QWidget> column = std::make_unique<QWidget>();
    fv::ButtonRow* row = nullptr;
    std::vector<fv::Button*> buttons;

    explicit LinkRow(int width) {
        std::vector<QPushButton*> made;
        for (const char* label : {"Hugging Face ↗", "Original ↗", "License ↗"}) {
            auto* button = new fv::Button(QString::fromUtf8(label));
            fv::neutralButton(button);
            button->setMinimumWidth(0); // compact buttons clear their floor, as in Settings
            buttons.push_back(button);
            made.push_back(button);
        }
        row = new fv::ButtonRow(made);
        column->setFixedWidth(width);
        auto* layout = new QVBoxLayout(column.get());
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(row);
        column->show();
        pump();
    }

    std::vector<std::pair<QString, QString>> overlaps() const {
        std::vector<std::pair<QString, QString>> found;
        for (std::size_t a = 0; a < buttons.size(); ++a) {
            for (std::size_t b = a + 1; b < buttons.size(); ++b) {
                if (buttons[a]->isVisible() && buttons[b]->isVisible() &&
                    buttons[a]->geometry().intersects(buttons[b]->geometry())) {
                    found.emplace_back(buttons[a]->text(), buttons[b]->text());
                }
            }
        }
        return found;
    }
};

} // namespace

TEST_CASE("test_setting_tiles_use_switches_that_say_what_they_toggle", "[widget]") {
    fv::SettingTile tile(QStringLiteral("Paste into the active window"),
                         QStringLiteral("Types the transcript."));
    QAbstractButton* box = tile.checkbox();
    REQUIRE(box != nullptr);
    CHECK(box->isCheckable());
    CHECK(box->accessibleName() == QStringLiteral("Paste into the active window"));
    CHECK(tile.property(fv::ui::prop::checked).toBool() == false);
    box->setChecked(true);
    CHECK(tile.property(fv::ui::prop::checked).toBool() == true);
    box->click();
    CHECK_FALSE(box->isChecked());
    CHECK(tile.property(fv::ui::prop::checked).toBool() == false);
}

TEST_CASE("test_compact_buttons_are_chosen_by_property_and_keep_their_own_name", "[widget]") {
    fv::Button secondary(QStringLiteral("Forget host"));
    fv::neutralButton(&secondary);
    fv::PrimaryButton primary(QStringLiteral("Use for dictation"));
    fv::compactPrimaryButton(&primary);
    primary.setObjectName(QStringLiteral("remoteUseButton")); // what callers do to find it
    CHECK(secondary.property(fv::ui::prop::tone).toString() == QStringLiteral("neutral"));
    CHECK(secondary.minimumHeight() == 34);
    CHECK(primary.property(fv::ui::prop::tone).toString() == QStringLiteral("primary"));
    CHECK(primary.objectName() == QStringLiteral("remoteUseButton"));
    // A property selector ranks below #id rules, so the theme's own primary and danger buttons
    // are never overridden by these.
    const auto theme = themeSheet();
    REQUIRE_FALSE(theme.empty());
    CHECK(theme.contains(R"(QPushButton[tone="neutral"])"));
    CHECK(theme.contains(R"(QPushButton[tone="primary"])"));
    CHECK(theme.find("QPushButton#primaryButton") < theme.find(R"(QPushButton[tone="primary"])"));
}

TEST_CASE("test_segmented_bar_details_can_change_while_shown", "[widget]") {
    fv::SegmentedBar bar({{QStringLiteral("Use another computer"), QStringLiteral("Not paired")},
                          {QStringLiteral("Share this computer"), QStringLiteral("Off")}});
    bar.show();
    pump(1);
    QSignalSpy clicks(&bar, &fv::SegmentedBar::activated);
    bar.setDetail(0, QStringLiteral("Paired · jed"));
    REQUIRE(bar.buttons()[0]->detailLabel() != nullptr);
    CHECK(bar.buttons()[0]->detailLabel()->text() == QStringLiteral("Paired · jed"));
    bar.setCurrentIndex(1); // programmatic: not a person's click
    CHECK(clicks.isEmpty());
    CHECK(bar.currentIndex() == 1);
    bar.buttons()[0]->click();
    REQUIRE(clicks.count() == 1);
    CHECK(clicks.at(0).at(0).toInt() == 0);
    CHECK(bar.currentIndex() == 0);
}

TEST_CASE("test_segmented_bar_wraps_its_text_when_not_even_one_choice_fits", "[widget]") {
    QWidget page;
    auto* layout = new QVBoxLayout(&page);
    auto* bar = new fv::SegmentedBar({{QStringLiteral("Open a session"), QString()},
                                      {QStringLiteral("Quick answer"), QString()}},
                                     true);
    layout->addWidget(bar);
    page.setFixedWidth(500);
    page.show();
    pump();
    const int oneLine = bar->buttons()[0]->height();
    const int natural = bar->buttons()[0]->naturalWidth();

    page.setFixedWidth(natural / 2);
    pump();
    for (auto* button : bar->buttons()) {
        CHECK(button->width() <= bar->width());
        CHECK(button->height() > oneLine); // the text went onto more lines
        CHECK(button->height() >= button->minimumSizeHint().height());
    }

    page.setFixedWidth(500);
    pump();
    CHECK(bar->buttons()[0]->height() == oneLine);
    CHECK(bar->buttons()[1]->height() == oneLine);
    CHECK(bar->buttons()[0]->y() == bar->buttons()[1]->y()); // side by side again
}

TEST_CASE("test_wrapped_label_can_break_a_long_value_and_still_reports_it_unchanged", "[widget]") {
    for (const QString& value : {kCommit, kFolder}) {
        CAPTURE(value.toStdString());
        auto plain = narrow(value, false);
        auto breaking = narrow(value, true);
        const int line = breaking->fontMetrics().height();
        // Without the option the widest word demands more than the label has: it gets clipped.
        CHECK(plain->minimumSizeHint().width() > 130);
        CHECK(breaking->minimumSizeHint().width() <= 130);
        CHECK(breaking->heightForWidth(130) >= 2 * line);
        CHECK(breaking->text() == value);
    }
}

TEST_CASE("test_wrapped_label_leaves_prose_and_breakable_names_alone", "[widget]") {
    for (const QString& value :
         {QStringLiteral("Multilingual transcription, translation, and language identification"),
          QStringLiteral("Systran/faster-whisper-tiny")}) {
        CAPTURE(value.toStdString());
        auto plain = narrow(value, false);
        auto breaking = narrow(value, true);
        CHECK(breaking->heightForWidth(130) == plain->heightForWidth(130));
        CHECK(breaking->text() == value);
    }
}

TEST_CASE("test_copying_a_long_value_gives_back_the_original_text", "[widget]") {
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        SKIP("writes the clipboard: only the offscreen platform keeps it to this process");
    }
    fv::WrappedLabel label(kCommit, nullptr, true);
    label.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label.setFixedWidth(130);
    label.show();
    pump(1);
    label.setFocus();
    QTest::keyClick(&label, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(&label, Qt::Key_C, Qt::ControlModifier);
    CHECK(label.selectedText() == kCommit);
    CHECK(QApplication::clipboard()->text() == kCommit);
}

TEST_CASE("test_clean_up_never_rewrites_text_that_did_not_come_from_the_label", "[widget]") {
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        SKIP("writes the clipboard: only the offscreen platform keeps it to this process");
    }
    fv::WrappedLabel label(kCommit, nullptr, true);
    label.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label.show();
    pump(1);
    const QString elsewhere =
        QStringLiteral("from") + QChar(0x200B) + QStringLiteral("another app");
    QApplication::clipboard()->setText(elsewhere);
    // A copy with nothing selected copies nothing; the clean-up that follows it must leave
    // another app's text alone.
    label.setFocus();
    QTest::keyClick(&label, Qt::Key_C, Qt::ControlModifier);
    CHECK(QApplication::clipboard()->text() == elsewhere);
}

TEST_CASE("test_button_row_shares_one_line_while_the_labels_fit", "[widget]") {
    LinkRow wide(900);
    const auto& buttons = wide.buttons;
    CHECK(buttons[0]->y() == buttons[1]->y());
    CHECK(buttons[1]->y() == buttons[2]->y());
    CHECK(wide.overlaps().empty());
}

TEST_CASE("test_button_row_wraps_instead_of_letting_buttons_overlap", "[widget]") {
    for (const int width : {150, 180, 268, 330}) {
        CAPTURE(width);
        LinkRow narrowRow(width);
        CHECK(narrowRow.overlaps().empty());
        for (auto* button : narrowRow.buttons) {
            CAPTURE(button->text().toStdString());
            CHECK(button->width() >= button->sizeHint().width());
            CHECK(narrowRow.row->rect().contains(button->geometry()));
        }
    }
}

TEST_CASE("test_button_row_puts_the_short_row_first_and_follows_the_labels", "[widget]") {
    LinkRow links(268);
    auto* repo = links.buttons[0];
    auto* origin = links.buttons[1];
    auto* license = links.buttons[2];
    // The main link sits alone on top, the other two share the row under it.
    CHECK(repo->y() < origin->y());
    CHECK(origin->y() == license->y());
    CHECK(repo->width() == links.row->width());

    repo->setText(QStringLiteral("Repository ↗"));
    license->hide();
    links.row->refresh();
    pump();
    CHECK(repo->y() == origin->y());
    CHECK(links.overlaps().empty());
}
