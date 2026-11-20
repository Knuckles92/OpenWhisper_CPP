#include "models/testing/fake_openwhisper.h"

#include <fstream>
#include <stdexcept>

#include "base/hash.h"
#include "base/utf8.h"
#include "models/catalog.h"
#include "models/data/speech_catalog.json.h"
#include "models/detail/files.h"
#include "pyport/json.h"

namespace fv::models::testing {

namespace fs = std::filesystem;

namespace {

void write_bytes(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw fs::filesystem_error("could not write the file", path,
                                   std::make_error_code(std::errc::io_error));
    }
}

// OpenWhisper's pin of the Python embeddable zip in its Windows runtime entries.
Json python_archive() {
    return Json{{"name", "python-3.12.10-embed-amd64.zip"},
                {"url", "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip"},
                {"sha256", "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"},
                {"size_bytes", 11133606},
                {"extract", "zip"}};
}

} // namespace

Json small_model_spec(std::string_view file_name, std::string_view content) {
    Json file;
    file["name"] = std::string(file_name);
    file["url"] = "https://example.invalid/" + std::string(file_name);
    file["sha256"] = sha256_hex(content);
    file["size_bytes"] = static_cast<std::int64_t>(content.size());
    Json spec;
    spec["repo"] = "test/model";
    spec["revision"] = "pinned";
    spec["files"] = Json::array({file});
    return spec;
}

fs::path write_openwhisper_runtime(const fs::path& ow_root, std::string_view component_id,
                                   std::string_view tag) {
    auto entry = catalog_entry(component_id, tag);
    if (!entry) {
        throw std::invalid_argument("no catalog entry for this component and platform");
    }
    const fs::path tree = ow_root / "components" / fv::base_utf8::path_from_utf8(component_id);
    fs::create_directories(tree);
    Json manifest = *entry;
    if (tag == platform::kWinAmd64) {
        const auto python_bytes =
            Json::parse(embedded::models_speech_catalog).at("python_embed_extracted_bytes");
        manifest["install_bytes"] =
            manifest["install_bytes"].get<std::int64_t>() + python_bytes.get<std::int64_t>();
        Json archives = Json::array({python_archive()});
        for (const Json& archive : manifest["archives"]) {
            archives.push_back(archive);
        }
        manifest["archives"] = archives;
        write_bytes(tree / "bin" / "nemo_speech_asr_c.dll", "runtime");
        write_bytes(tree / "bin" / "ggml.dll", "ggml");
        write_bytes(tree / "share" / "licenses" / "nemo-speech" / "LICENSE", "license");
        for (const std::string_view name :
             {"python.exe", "python312.dll", "python312.zip", "python312._pth", "_ctypes.pyd",
              "vcruntime140.dll", "LICENSE.txt"}) {
            write_bytes(tree / fv::base_utf8::path_from_utf8(name), "python");
        }
    } else if (tag == platform::kLinuxX86_64) {
        write_bytes(tree / "nemo-speech" / "lib" / "libnemo_speech_asr_c.so", "runtime");
    } else {
        write_bytes(tree / "nemo-speech" / "lib" / "libnemo_speech_asr_c.dylib", "runtime");
        write_bytes(tree / "nemo-speech" / "lib" / "libggml-metal.dylib", "metal");
    }
    py::DumpOptions options;
    options.indent = 2;
    detail::write_text(tree / "manifest.json", py::dumps(manifest, options));
    detail::write_text(tree / ".installed", detail::py_str(manifest.at("version")));
    return tree;
}

fs::path write_openwhisper_model(const fs::path& ow_root, std::string_view key, const Json& spec,
                                 const std::map<std::string, std::string>& contents) {
    const fs::path folder = ow_root / "speech-models" / fv::base_utf8::path_from_utf8(key);
    fs::create_directories(folder);
    for (const auto& [name, bytes] : contents) {
        write_bytes(folder / fv::base_utf8::path_from_utf8(name), bytes);
    }
    detail::write_text(folder / "installed.json", py::dumps(spec));
    return folder;
}

} // namespace fv::models::testing
