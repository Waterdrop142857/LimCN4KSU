#include "shared/status.h"

namespace bybus::shared {
namespace {

struct Entry {
    Code code;
    const char *text;
    const char *message;
};

constexpr Entry kEntries[] = {
    {Code::Ok, "ok", ""},
    {Code::Usage, "usage", "命令用法无效"},
    {Code::UnknownCommand, "unknown_command", "不支持的命令"},
    {Code::InvalidArgument, "invalid_argument", "参数无效"},
    {Code::GameMissing, "game_missing", "未找到官方游戏安装"},
    {Code::TextMissing, "text_missing", "未找到游戏原文文件"},
    {Code::GameMissingText, "game_missing_text", "游戏原文尚未下载"},
    {Code::ArchiveInvalid, "archive_invalid", "汉化包无效"},
    {Code::ArchiveLimit, "archive_limit", "汉化包超出大小限制"},
    {Code::ArchivePathUnsafe, "archive_path_unsafe", "汉化包包含不安全路径"},
    {Code::ArchiveDuplicate, "archive_duplicate", "汉化包包含重复文本文件"},
    {Code::SourceMissing, "source_missing", "汉化源不存在"},
    {Code::SourceChanged, "source_changed", "汉化源已变化，请重新操作"},
    {Code::FontInvalid, "font_invalid", "字体校验失败"},
    {Code::SnapshotInvalid, "snapshot_invalid", "快照校验失败"},
    {Code::SnapshotStale, "snapshot_stale", "快照与当前游戏原文不匹配"},
    {Code::SnapshotBusy, "snapshot_busy", "已有构建任务在进行"},
    {Code::UnsupportedRuntime, "unsupported_runtime", "当前游戏版本不受支持"},
    {Code::ZygiskMissing, "zygisk_missing", "缺少可用的 Zygisk 实现"},
    {Code::NetworkUnavailable, "network_unavailable", "下载通道不可用"},
    {Code::NetworkRejected, "network_rejected", "下载被拒绝"},
    {Code::JobMissing, "job_missing", "任务不存在"},
    {Code::JobCancelled, "job_cancelled", "任务已取消"},
    {Code::IoError, "io_error", "读写失败"},
    {Code::InternalError, "internal_error", "内部错误"},
};

} // namespace

const char *code_text(Code code) {
    for (const Entry &entry : kEntries) {
        if (entry.code == code) return entry.text;
    }
    return "internal_error";
}

const char *code_message(Code code) {
    for (const Entry &entry : kEntries) {
        if (entry.code == code) return entry.message;
    }
    return "内部错误";
}

bool code_from_text(const std::string &text, Code *out) {
    if (!out) return false;
    for (const Entry &entry : kEntries) {
        if (text == entry.text) {
            *out = entry.code;
            return true;
        }
    }
    return false;
}

} // namespace bybus::shared
