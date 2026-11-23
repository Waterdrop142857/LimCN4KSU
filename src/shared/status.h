// Fixed protocol codes shared by bybusctl, bybusd, and the runtime receipt.
// The string set is frozen by 20-protocol.md §4; adding a code requires a schema bump.
#pragma once

#include <string>

namespace bybus::shared {

enum class Code {
    Ok,
    Usage,
    UnknownCommand,
    InvalidArgument,
    GameMissing,
    TextMissing,
    GameMissingText,
    ArchiveInvalid,
    ArchiveLimit,
    ArchivePathUnsafe,
    ArchiveDuplicate,
    SourceMissing,
    SourceChanged,
    FontInvalid,
    SnapshotInvalid,
    SnapshotStale,
    SnapshotBusy,
    UnsupportedRuntime,
    ZygiskMissing,
    NetworkUnavailable,
    NetworkRejected,
    JobMissing,
    JobCancelled,
    IoError,
    InternalError,
};

const char *code_text(Code code);
bool code_from_text(const std::string &text, Code *out);

// Short Chinese operator-facing text. Never contains paths, text bodies or
// identifiers; the UI maps codes itself and only logs this for bybusctl users.
const char *code_message(Code code);

} // namespace bybus::shared
