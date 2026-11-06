// Program clipboard of document objects (app-ux §8а, assembly.md §5б.2, FX7-ASM-008): copy in one window, paste in
// another window of the same program; the data also goes to the system clipboard under its own MIME type.
#pragma once

#include "doc/json_text.h"

#include <optional>

namespace construe::app {

inline constexpr const char* ELEMENT_MIME_TYPE = "application/x-construe-element";

class ElementClipboard {
public:
    // Data of api::copyElement: kept in the program and put into the system clipboard.
    static void put(const doc::Json& data);
    // Data to paste: the system clipboard if it holds construe data (copied last, possibly in another program
    // instance), otherwise the program's own copy; nullopt — nothing to paste.
    static std::optional<doc::Json> get();
    // The program's own copy (tests check it: the system clipboard may be busy, for example under Windows).
    static const std::optional<doc::Json>& internal();
    static void clear();  // tests
};

}  // namespace construe::app
