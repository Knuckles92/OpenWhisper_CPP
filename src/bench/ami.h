#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// benchmarks/meeting_mode/ami.py: the AMI Meeting Corpus meetings the meeting benchmark runs,
// where their files live, and the manual reference transcripts. Nothing is downloaded: the
// corpus (ami_public_manual_1.6.2.zip extracted to <data>/annotations and the headset mixes in
// <data>/audio) is prepared by hand, as openwhisper-cpp-bench contacts no host.
namespace fv::bench {

// One public meeting selected for the dogfood suite.
struct MeetingSpec {
    std::string meeting_id;
    std::string description;

    // "<id>.Mix-Headset.wav", the official headset-mix filename.
    std::string audio_filename() const;
};

// The ten naturally occurring IN10xx research meetings (DEFAULT_MEETINGS).
const std::vector<MeetingSpec>& default_meetings();

// select_meetings: the curated meetings named by `ids` (trimmed, upper-cased, blanks skipped),
// all of them when none is named. Throws std::invalid_argument("Unknown curated AMI meeting
// id(s): A, B") for an id outside the list.
std::vector<MeetingSpec> select_meetings(const std::vector<std::string>& ids);

// One manually transcribed AMI word with meeting-clock timing.
struct ReferenceWord {
    std::string text;
    double start_s = 0.0;
    double end_s = 0.0;
    std::string speaker;

    friend bool operator==(const ReferenceWord&, const ReferenceWord&) = default;
};

// parse_reference_words: <annotations>/words/<meeting>.<speaker>.words.xml read in file-name
// order and merged by (start, end, source order). Only <w> elements (any namespace) count;
// punctuation (punc="true"), empty text after NFKC and strip, and words without numeric
// starttime and endtime are skipped, as are laughs, gaps and other events. Throws
// std::runtime_error naming the folder when no file matches, std::invalid_argument when no
// usable word remains.
std::vector<ReferenceWord> parse_reference_words(const std::filesystem::path& annotations_dir,
                                                 std::string_view meeting_id);

// <data>/annotations and <data>/audio/<id>.Mix-Headset.wav.
std::filesystem::path annotation_root(const std::filesystem::path& data_dir);
std::filesystem::path audio_path(const std::filesystem::path& data_dir,
                                 std::string_view meeting_id);

} // namespace fv::bench
