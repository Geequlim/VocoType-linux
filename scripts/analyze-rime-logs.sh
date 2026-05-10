#!/usr/bin/env bash
set -euo pipefail

DEFAULT_LOG_DIR="$HOME/.local/share/vocotype-fcitx5/logs"

resolve_log_file() {
    if [[ -n "${VOCOTYPE_LOG_FILE:-}" && -f "${VOCOTYPE_LOG_FILE}" ]]; then
        printf '%s\n' "${VOCOTYPE_LOG_FILE}"
        return 0
    fi

    if [[ ! -d "${DEFAULT_LOG_DIR}" ]]; then
        return 1
    fi

    local latest
    latest=$(ls -1t "${DEFAULT_LOG_DIR}"/log_*.log 2>/dev/null | head -n 1 || true)
    [[ -n "${latest}" ]] || return 1
    printf '%s\n' "${latest}"
}

LOG_FILE="$(resolve_log_file || true)"

if [[ -z "${LOG_FILE}" ]]; then
    echo "未找到日志文件。"
    echo "请先在 ~/.config/vocotype/fcitx5-backend.json 中启用文件日志，再重新运行。"
    exit 1
fi

show_menu() {
    echo
    echo "=== VoCoType Fcitx5 日志分析 ==="
    echo "日志文件: ${LOG_FILE}"
    echo
    echo "1) 查看错误日志"
    echo "2) 查看 Rime 相关日志"
    echo "3) 查看 socket / IPC 日志"
    echo "4) 查看 ASR / SLM 日志"
    echo "5) 实时监控"
    echo "6) 查看完整日志"
    echo "0) 退出"
    echo
}

view_errors() {
    rg -i "error|failed|warning|traceback|exception" "${LOG_FILE}" | less
}

view_rime() {
    rg -i "rime|schema|preedit|candidate|commit" "${LOG_FILE}" | less
}

view_ipc() {
    rg -i "socket|ipc|ping|request|connect|accept" "${LOG_FILE}" | less
}

view_asr_slm() {
    rg -i "funasr|asr|transcribe|slm|polish|warmup" "${LOG_FILE}" | less
}

monitor_live() {
    tail -f "${LOG_FILE}"
}

view_full() {
    less "${LOG_FILE}"
}

while true; do
    show_menu
    read -r -p "选择选项 [0-6]: " choice
    case "${choice}" in
        1) view_errors ;;
        2) view_rime ;;
        3) view_ipc ;;
        4) view_asr_slm ;;
        5) monitor_live ;;
        6) view_full ;;
        0) exit 0 ;;
        *) echo "无效选项" ;;
    esac
done
