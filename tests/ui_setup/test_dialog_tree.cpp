// The setup dialog's widget tree under the application theme, dark and light. There is no
// OpenWhisper counterpart to take a reference from, so this checks what the tree must hold
// (the theme's own object names, the buttons and their texts) and that the theme changes colours
// and nothing else; it does not compare pixels.

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QLabel>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "screenshot/screenshot.h"
#include "setup_support.h"
#include "testkit/testkit.h"
#include "ui_setup/runtime_setup_dialog.h"

using namespace fvtest;
using fv::Json;

namespace {

struct Node {
    std::string objectName;
    std::string text;
    std::string className;
    bool visible = false;
};

void collect(const Json& tree, std::vector<Node>& out) {
    Node node;
    node.objectName = tree.value("objectName", "");
    node.text = tree.value("text", "");
    node.className = tree.value("class", "");
    node.visible = tree.value("visible", false);
    out.push_back(node);
    for (const Json& child : tree.at("children")) {
        collect(child, out);
    }
}

std::vector<Node> nodesOf(const Json& tree) {
    std::vector<Node> nodes;
    collect(tree, nodes);
    return nodes;
}

const Node* find(const std::vector<Node>& nodes, const std::string& objectName,
                 const std::string& text = {}) {
    for (const Node& node : nodes) {
        if (node.objectName == objectName && (text.empty() || node.text == text)) {
            return &node;
        }
    }
    return nullptr;
}

// A dialog that has looked at an OpenWhisper tree with a runtime and a model, shown offscreen.
Json dumpDialog(Fixture& fx, fv::RuntimeSetupDialog& dialog) {
    dialog.show();
    fx.settle();
    QCoreApplication::processEvents();
    return fv::screenshot::dump_tree(dialog);
}

// The pixel size theme.qss gives QLabel#headerLabel (18 px): proof the dialog is styled by the
// theme's rule, not by one of its own.
int headerPixels(const fv::RuntimeSetupDialog& dialog) {
    const auto* header = dialog.findChild<QLabel*>(QStringLiteral("headerLabel"));
    return header != nullptr ? header->font().pixelSize() : -1;
}

void checkTree(const Json& tree) {
    const auto nodes = nodesOf(tree);
    CHECK(tree.at("objectName") == "runtimeSetupDialog");
    CHECK(tree.at("visible") == true);

    const Node* header = find(nodes, "headerLabel");
    REQUIRE(header != nullptr);
    CHECK(header->text == "Set up speech recognition");
    CHECK(header->visible);

    const Node* use = find(nodes, "primaryButton", "Use these");
    REQUIRE(use != nullptr);
    CHECK(use->className == "PrimaryButton");
    CHECK(use->visible);

    for (const char* label :
         {"Choose a NeMo-Speech.cpp folder…", "Choose a model file…", "Later"}) {
        INFO(label);
        const Node* button = nullptr;
        for (const Node& node : nodes) {
            if (node.className == "Button" && node.text == label) {
                button = &node;
            }
        }
        REQUIRE(button != nullptr);
        CHECK(button->visible);
    }

    // The section headings and the explanations use the theme's own labels.
    int sections = 0;
    int infos = 0;
    for (const Node& node : nodes) {
        sections += node.objectName == "sectionLabel" ? 1 : 0;
        infos += node.objectName == "infoLabel" ? 1 : 0;
    }
    CHECK(sections == 3);
    CHECK(infos >= 6); // the intro, the offer, two explanations and the two rows
    CHECK(find(nodes, "animatedProgressBar") != nullptr);
    CHECK(find(nodes, "card") != nullptr);
}

// The committed tree for a theme: generated from this widget (there is no OpenWhisper
// counterpart) and reviewed by the lead, as the spec's "Reference screenshots" asks. With
// FIORIVOCE_WRITE_UI_SETUP_REFERENCES set the test writes the file instead of reading it; review
// the diff before committing it. Geometry and fonts are Windows' (the fonts of QT_QPA_FONTDIR), so
// the comparison runs there only.
void compareWithReference(Json tree, std::string_view theme) {
    // Where the window system put the top-level window is not the dialog's; its size is.
    tree["geometry"][0] = 0;
    tree["geometry"][1] = 0;
    const auto file = fv::testkit::source_root() / "tests" / "ui_setup" / "reference" /
                      ("runtime_setup_dialog-" + std::string(theme) + ".json");
    if (fv::testkit::get_env("FIORIVOCE_WRITE_UI_SETUP_REFERENCES")) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << tree.dump(1) << '\n';
    }
#ifndef _WIN32
    SKIP("The reference tree has Windows' font metrics");
#endif
    REQUIRE(std::filesystem::exists(file));
    const Json expected = Json::parse(readFile(file));
    const auto differences = fv::screenshot::compare_trees(expected, tree);
    for (const auto& difference : differences) {
        UNSCOPED_INFO(difference);
    }
    CHECK(differences.empty());
}

} // namespace

TEST_CASE("the setup dialog's tree is complete in the dark theme", "[ui_setup][widget]") {
    AppTheme theme(QStringLiteral("dark"));
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();
    fv::RuntimeSetupDialog dialog(std::make_shared<const fv::RuntimeSetupPorts>(fx.ports()));
    const Json tree = dumpDialog(fx, dialog);
    checkTree(tree);
    CHECK(headerPixels(dialog) == 18);
    compareWithReference(tree, "dark");
}

TEST_CASE("the setup dialog's tree is complete in the light theme", "[ui_setup][widget]") {
    AppTheme theme(QStringLiteral("light"));
    Fixture fx;
    fx.openwhisperRuntime();
    fx.openwhisperModel();
    fv::RuntimeSetupDialog dialog(std::make_shared<const fv::RuntimeSetupPorts>(fx.ports()));
    const Json tree = dumpDialog(fx, dialog);
    checkTree(tree);
    CHECK(headerPixels(dialog) == 18);
    compareWithReference(tree, "light");
}

TEST_CASE("the theme changes the dialog's colours and nothing in its tree", "[ui_setup][widget]") {
    Json dark;
    Json light;
    for (const bool isDark : {true, false}) {
        AppTheme theme(isDark ? QStringLiteral("dark") : QStringLiteral("light"));
        Fixture fx;
        fx.openwhisperRuntime();
        fx.openwhisperModel();
        fv::RuntimeSetupDialog dialog(std::make_shared<const fv::RuntimeSetupPorts>(fx.ports()));
        (isDark ? dark : light) = dumpDialog(fx, dialog);
    }
    CHECK(fv::screenshot::compare_trees(dark, light).empty());
}
