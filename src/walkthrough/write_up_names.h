#pragma once

#include <string_view>

// The names an AI write-up of a walkthrough is saved under and the words it starts from
// (services/walkthrough/write_up.py:40-59, 128).
namespace fv::walkthrough::write_up {

inline constexpr std::string_view MARKDOWN_NAME = "write-up.md";
inline constexpr std::string_view PDF_NAME = "write-up.pdf";
inline constexpr std::string_view DOCX_NAME = "write-up.docx";

// What the agent is asked for unless Settings has the user's own words.
inline constexpr std::string_view DEFAULT_INSTRUCTIONS =
    "Here are some screenshots and a transcript of me talking through them. "
    "Please organize it into a simple but good-looking walkthrough document of "
    "the content. Give it a short title that says what it's about. Tidy my "
    "wording into clear writing, dropping filler and false starts but keeping "
    "everything I meant. Use whatever structure suits the content, such as "
    "steps, sections or short paragraphs, and put each screenshot where it "
    "helps most.";

// Where the write-up's agent is chosen.
inline constexpr std::string_view WRITE_UP_AGENT_SETTING = "Settings → Dictation → Walkthroughs";

} // namespace fv::walkthrough::write_up
