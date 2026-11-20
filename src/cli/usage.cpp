#include "cli/usage.h"

namespace fv::cli {

// Laid out as main.py's _USAGE: option names from column 2, descriptions from column 26, no
// line over 78 characters, so the text reads in an 80-column terminal.
std::string usage_text(std::string_view product, std::string_view env_prefix) {
    const std::string prefix(env_prefix);
    std::string text = "Usage: openwhisper-cpp-cli COMMAND [OPTION]...\n"
                       "       openwhisper-cpp-cli [OPTION]\n"
                       "\n"
                       "Command-line tools of ";
    text += product;
    text += ": the speech engine, and checks that need\n"
            "no window.\n"
            "\n"
            "Commands:\n"
            "  transcribe AUDIO.wav [TRANSCRIBE OPTION]...\n"
            "                          Transcribe a WAV file with Parakeet and print the\n"
            "                          text.\n"
            "\n"
            "Options:\n"
            "  -h, --help              Show this help and exit.\n"
            "  --version               Print the version and exit.\n"
            "  --diagnostics OUTPUT.json\n"
            "                          Write a sanitized support report to a new JSON file\n"
            "                          and exit. Nothing is uploaded.\n"
            "  --api [--database PATH] [--port N]\n"
            "                          Serve the read-only History API on 127.0.0.1 (port\n"
            "                          8766 by default) without opening the app. Set\n"
            "                          ";
    text += prefix + "API_TOKEN to a random token of at least\n";
    text += "                          32 characters first.\n"
            "  --channel               Run as a Claude Code channel (an MCP server over\n"
            "                          stdio) that forwards ";
    text += product;
    text += " actions into a\n"
            "                          running Claude Code session. ";
    text += product;
    text += " must be\n"
            "                          running with API & MCP on.\n"
            "  --self-test             Check that the data folder, the settings and the\n"
            "                          speech runtime are usable, run the workflow smoke,\n"
            "                          and exit.\n"
            "  --self-test --installed\n"
            "                          Check that every file the installed app needs is\n"
            "                          beside this program, and exit.\n"
            "  --workflow-smoke [REPORT.json]\n"
            "                          Run a headless check of the dictation pipeline,\n"
            "                          optionally saving its report, and exit.\n"
            "\n"
            "Transcribe options:\n"
            "  --model FILE            The model's .gguf file. Default: $";
    text += prefix + "MODEL.\n";
    text += "  --runtime DIR           An unpacked NeMo-Speech.cpp release.\n"
            "                          Default: $";
    text += prefix + "RUNTIME_DIR.\n";
    text += "  --gpu N                 Decode on the runtime's GPU number N (0 is the\n"
            "                          first). Default: CPU.\n"
            "  --language CODE         The spoken language, such as en. Default: detect it.\n"
            "  --timestamps            Print one segment per line with its start and end\n"
            "                          time.\n"
            "\n"
            "Environment:\n"
            "  ";
    text += prefix + "DATA_DIR=PATH\n"
                     "                          Keep settings, history and recordings in PATH.\n"
                     "  ";
    text += prefix + "LOG_LEVEL=LEVEL\n"
                     "                          Log at DEBUG, INFO (the default), WARNING or "
                     "ERROR.\n"
                     "  ";
    text += prefix + "RUNTIME_DIR=PATH\n"
                     "                          The speech runtime to use when none is given.\n";
    return text;
}

} // namespace fv::cli
