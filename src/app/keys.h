// Hotkeys by physical key (spec/app-ux.md §1 P10, §7.3): the Windows virtual key code of
// US-layout letters/digits matches Qt::Key_A…Z / Key_0…9; if it is 0 (synthetic events,
// Qt Test) — by key(). macOS (S8-PKG-003, app-ux §7.3 «macOS»): the native code is the
// kVK_ANSI_* key code of the US layout, used only for letters of a non-Latin layout; the
// key labelled «delete» (Qt::Key_Backspace) deletes like Delete.
#pragma once

#include <QKeyEvent>

namespace construe::app {

enum class KeyPlatform { Windows, Mac, Other };

#ifdef Q_OS_MACOS
inline constexpr KeyPlatform kKeyPlatform = KeyPlatform::Mac;
#elif defined(Q_OS_WIN)
inline constexpr KeyPlatform kKeyPlatform = KeyPlatform::Windows;
#else
inline constexpr KeyPlatform kKeyPlatform = KeyPlatform::Other;
#endif

// Qt key of a macOS kVK_ANSI_* key code (Carbon Events.h: kVK_ANSI_A = 0x00 … kVK_ANSI_Period = 0x2F); 0 — not a
// letter or digit.
inline int macAnsiKey(quint32 vk) {
    static constexpr char kAnsi[] = "ASDFHGZXCV\0BQWERYT123465\0" "97\0" "80\0OU\0IP\0LJ\0K\0\0\0\0NM";
    if (vk >= sizeof(kAnsi) - 1) return 0;
    const char c = kAnsi[vk];
    return c ? int(c) : 0;  // '0'…'9' and 'A'…'Z' equal Qt::Key_0…9 and Qt::Key_A…Z
}

// The physical key of an event with native code vk and Qt key on the given system (tests pass the system).
inline int physicalKeyFor(quint32 vk, int key, KeyPlatform platform) {
    if (platform == KeyPlatform::Mac) {
        if (key == Qt::Key_Backspace) return Qt::Key_Delete;
        // ASCII and named keys (Esc, Tab, arrows…) as reported; kVK codes 0x30… are Tab, Space, Esc — not digits.
        // Synthetic events carry vk 0 (= kVK_ANSI_A) — their key is ASCII, so the table is not consulted.
        if (key < 0x80 || key >= Qt::Key_Escape) return key;
        const int k = macAnsiKey(vk);
        return k ? k : key;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return int(vk);
    return key;
}

inline int physicalKey(const QKeyEvent* e) { return physicalKeyFor(e->nativeVirtualKey(), e->key(), kKeyPlatform); }

// The delete key of the system (lists, trees, tables outside text fields): Delete; on macOS also «delete» (Backspace).
inline bool isDeleteKey(const QKeyEvent* e) { return physicalKey(e) == Qt::Key_Delete; }

}  // namespace construe::app
