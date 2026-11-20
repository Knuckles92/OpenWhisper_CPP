#pragma once

// How a console server learns that the user stopped it: Ctrl+C or Ctrl+Break (or the console
// closing) on Windows, SIGINT or SIGTERM elsewhere.
namespace fv::cli::detail {

// Called before the server starts any thread: from here on those signals no longer end the
// process at once, they end wait_for_interrupt().
void prepare_interrupt();
// Waits until one of them arrives.
void wait_for_interrupt();

} // namespace fv::cli::detail
