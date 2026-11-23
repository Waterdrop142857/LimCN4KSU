// service/game_scan.h
// Responsibility: read-only discovery of the official game install and of the
// game's own localisation text. Two hard rules from the contract:
//  - only the fixed package `com.ProjectMoon.LimbusCompany` is ever inspected,
//    and only inside <files>/Localize/<jp|en|kr> after the documented search;
//  - nothing else (accounts, saves, unrelated private directories) is read.
// Install identity is package|path|size|mtime|... hashed with SHA-256
// (task contract §8, §9; 01-implementation-design.md §4.1).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "service/diagnostics.h"
#include "service/paths.h"
#include "shared/status.h"
#include "translation/snapshot_builder.h"

namespace bybus::service {

struct GameInstall {
    bool installed = false;
    std::string package_name = kGamePackage;
    int64_t version_code = 0;
    std::string version_name;
    std::string install_identity;  // 64 lowercase hex when installed
    std::vector<std::string> apk_paths;
};

struct GameTextFile {
    std::string resource;  // Localize/<locale>/<relative path>
    std::string path;
    uint64_t bytes = 0;
};

struct GameTextScan {
    shared::Code code = shared::Code::Ok;  // also reports read/enumeration failures
    std::string root;                      // the Localize directory that was used
    std::vector<std::string> locales;
    std::vector<GameTextFile> files;
    std::vector<translation::GameTable> tables;  // filled only with load_contents
    bool loaded_contents = false;
};

// `pm path` + `dumpsys package` through process_exec. Never fails hard: an
// absent game yields installed=false and Code::GameMissing.
shared::Code probe_install(GameInstall *out, Logger *log);

// Enumerates the localisation JSON files. With load_contents=false only names
// and sizes are collected (used by `status`); with true the file bodies are
// read into `tables` for build_generation().
shared::Code scan_game_texts(const GameInstall &install, bool load_contents, GameTextScan *out);

// Short-lived cache so `status` does not spawn `pm` twice per second.
class GameProbe {
public:
    GameProbe();
    void snapshot(bool force, GameInstall *install, GameTextScan *texts);
    void invalidate();

private:
    std::mutex state_mu_;
    std::mutex probe_mu_;
    GameInstall install_;
    GameTextScan texts_;
    int64_t at_millis_ = 0;
    bool valid_ = false;
};

} // namespace bybus::service
