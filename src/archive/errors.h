#pragma once

#include <stdexcept>
#include <string>

namespace fv::archive {

// The archive tries to write outside its folder or holds something the extraction refuses. The
// message is written for the user ("Archive contains an unsafe path: ../x").
class ArchiveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The file is not a readable zip or tar.gz, worded as Python's zipfile.BadZipFile and
// tarfile.ReadError word it ("File is not a zip file", "not a gzip file", "unexpected end of
// data").
class BadArchive : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The cancel token was cancelled between two members.
class ExtractCanceled : public std::runtime_error {
public:
    ExtractCanceled() : std::runtime_error("") {}
};

} // namespace fv::archive
