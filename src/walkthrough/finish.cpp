#include "walkthrough/finish.h"

#include <cctype>
#include <stdexcept>
#include <system_error>

#include "base/clock.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "walkthrough/detail/temp_folder.h"
#include "walkthrough/files.h"
#include "walkthrough/renderer.h"
#include "walkthrough/sections.h"

namespace fv::walkthrough {

namespace {

const log::Logger logger("services.walkthrough.finish");

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

std::pair<std::vector<std::string>, std::vector<Note>>
transcribe_sections(const std::filesystem::path& audio, const Storyboard& board,
                    const ClipTranscriber& transcribe, const BeforeEach& before_each) {
    const auto announce = [&](int index, int total) {
        if (before_each) {
            before_each(index, total);
        }
    };
    const detail::TemporaryFolder folder(ascii_lower(identity().product) + "-walkthrough-");
    std::vector<Clip> clips;
    std::vector<Clip> held;
    try {
        clips = split_recording(audio, board.cuts_s, folder.path(), board.notes_s);
        held = clip_stretches(audio, board.notes_s, folder.path());
    } catch (const std::exception& error) {
        // OSError, ValueError and wave.Error only, as Python catches.
        const bool expected = dynamic_cast<const std::system_error*>(&error) != nullptr ||
                              dynamic_cast<const std::invalid_argument*>(&error) != nullptr ||
                              dynamic_cast<const WaveError*>(&error) != nullptr;
        if (!expected) {
            throw;
        }
        // Every word above the first screenshot beats losing them.
        logger.warning("Could not cut the walkthrough audio; transcribing it whole");
        announce(1, 1);
        std::vector<std::string> texts{transcribe(audio)};
        texts.resize(board.shots.size() + 1);
        return {std::move(texts), {}};
    }
    const int total = static_cast<int>(clips.size() + held.size());
    std::vector<std::string> decoded;
    int index = 0;
    for (const auto* group : {&clips, &held}) {
        for (const auto& clip : *group) {
            announce(++index, total);
            decoded.push_back(clip.path ? py::strip(transcribe(*clip.path)) : std::string());
        }
    }
    std::vector<std::string> texts;
    for (const auto& pieces : board.sections) {
        std::string text;
        for (const int piece : pieces) {
            const std::string& words = decoded.at(static_cast<std::size_t>(piece));
            if (words.empty()) {
                continue;
            }
            if (!text.empty()) {
                text.push_back(' ');
            }
            text += words;
        }
        texts.push_back(std::move(text));
    }
    std::vector<Note> notes;
    for (std::size_t note = 0; note < board.notes_s.size(); ++note) {
        const std::string& text = decoded.at(clips.size() + note);
        if (!text.empty()) {
            notes.push_back({text, board.section_at(board.notes_s[note].first)});
        }
    }
    return {std::move(texts), std::move(notes)};
}

std::string marked_text(std::span<const std::string> texts, int shots) {
    std::vector<std::string> parts;
    for (std::size_t index = 0; index < texts.size(); ++index) {
        std::string text = py::strip(texts[index]);
        if (!text.empty()) {
            parts.push_back(std::move(text));
        }
        if (static_cast<int>(index) < shots) {
            parts.push_back("[Screenshot " + std::to_string(index + 1) + "]");
        }
    }
    std::string joined;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) {
            joined += "\n\n";
        }
        joined += parts[index];
    }
    return joined;
}

std::pair<DocumentFormat, PaperSize> document_settings(const Json& settings) {
    DocumentFormat format;
    format.title = keys::walkthrough_title.read(settings);
    format.details = keys::walkthrough_show_details.read(settings);
    format.about = keys::walkthrough_show_about.read(settings);
    format.captions = keys::walkthrough_show_captions.read(settings);
    const std::string page = keys::walkthrough_page_size.read(settings);
    PaperSize paper = PaperSize::Auto;
    if (page == "letter") {
        paper = PaperSize::Letter;
    } else if (page == "a4") {
        paper = PaperSize::A4;
    }
    return {std::move(format), paper};
}

WalkthroughResult build_documents(WalkthroughSession& session, std::span<const Shot> shots,
                                  std::span<const std::string> cleaned,
                                  std::span<const std::string> raw, double duration_s,
                                  std::span<const Note> notes, const Json& settings) {
    const auto held = [&](int index) {
        std::vector<std::string> texts;
        for (const auto& note : notes) {
            if (note.section == index) {
                texts.push_back(note.text);
            }
        }
        return texts;
    };
    if (cleaned.size() < shots.size() + 1) {
        throw std::out_of_range("Every screenshot and the words after the last need a text");
    }
    std::vector<Section> sections;
    for (std::size_t index = 0; index < shots.size(); ++index) {
        Section section;
        section.text = cleaned[index];
        section.shot = shots[index].path;
        section.kept_at_s = shots[index].kept_at_s;
        section.notes = held(static_cast<int>(index));
        sections.push_back(std::move(section));
    }
    Section last;
    last.text = cleaned[shots.size()];
    last.notes = held(static_cast<int>(shots.size()));
    sections.push_back(std::move(last));
    // The naive start read in the computer's zone, as the manifest's astimezone() does.
    const Walkthrough walkthrough{local_moment(system_clock().zone(), session.started_at()),
                                  duration_s, std::move(sections)};
    const auto folder = session.folder();
    const auto [format, paper] = document_settings(settings);
    const auto markdown_path = write_markdown(walkthrough, folder, format);
    auto& renderer = document_renderer();
    const auto pdf = renderer.write_pdf(walkthrough, folder, format, paper);
    renderer.write_docx(walkthrough, folder, format, paper);
    write_manifest(walkthrough, folder);
    std::error_code ignored;
    std::filesystem::remove(folder / path_from_utf8(DRAFT_NAME), ignored);
    const int count = static_cast<int>(shots.size());
    const std::string text = marked_text(cleaned, count);
    const std::string raw_text = marked_text(raw, count);
    WalkthroughResult result;
    result.folder = folder;
    result.pdf = pdf;
    result.markdown = markdown_path;
    result.clipboard_text = markdown(walkthrough, folder, true, format);
    result.history_text = text;
    if (raw_text != text) {
        result.history_raw = raw_text;
    }
    result.shots = count;
    result.duration_s = duration_s;
    if (!shots.empty()) {
        result.first_shot = shots.front().path;
    }
    return result;
}

std::string kept_note(const WalkthroughSession* session) {
    if (session == nullptr || session->shot_count() == 0) {
        return "";
    }
    return " — screenshots kept in " + path_to_utf8(session->folder());
}

} // namespace fv::walkthrough
