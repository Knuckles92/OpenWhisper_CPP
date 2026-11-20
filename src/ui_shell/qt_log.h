#pragma once

#include <string>
#include <string_view>

class QMessageLogContext;

namespace fv {

// bootstrap._qt_context_suffix: " (<file>:<line> <function>)" when Qt gave a non-empty file or
// function or a non-zero line (a null file or function reads "None", as Python printed it),
// else "".
std::string qtContextSuffix(const char* file, int line, const char* function);

// bootstrap._install_qt_message_handler: Qt's debug, info, warning, critical and fatal messages
// go to the logger "qt" at DEBUG, INFO, WARNING, ERROR and CRITICAL, each with its context
// suffix. Installed once per process; later calls do nothing. Logs "Qt message handler
// installed" when it installs.
void installQtMessageHandler();

} // namespace fv
