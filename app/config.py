"""Configuration helpers for the speak-keyboard runtime."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from typing import Any, Dict, Optional


DEFAULT_CONFIG: Dict[str, Any] = {
    "hotkeys": {
        "toggle": "f2",
        "ptt_key": "F9",
        "ptt_fallback_keycode": 67,
        "ptt_hold_threshold_ms": 0,
        "long_mode_modifier": "Shift",
    },
    "audio": {
        "sample_rate": 16000,
        "block_ms": 20,
        "device": None,
        # 单次录音的最大大小（字节），默认20MB
        # 达到此限制后将自动停止录音并开始转录
        "max_session_bytes": 20 * 1024 * 1024,
    },
    "vad": {
        "start_threshold": 0.02,
        "stop_threshold": 0.01,
        "min_speech_ms": 300,
        "min_silence_ms": 200,
        "pad_ms": 200,
    },
    "asr": {
        "use_vad": False,
        "use_punc": True,
        "normalize_chinese_numbers": True,
        "language": "zh",
        "hotword": "",
        "batch_size_s": 60.0,
    },
    "slm": {
        "enabled": False,
        "endpoint": "http://127.0.0.1:18080/v1/chat/completions",
        "model": "Qwen/Qwen3.5-0.8B",
        "timeout_ms": 12000,
        # 流式润色按最近一次模型输出重新计时，而不是按请求开始计时
        "stream_idle_timeout_ms": 12000,
        # 0 表示不额外设置 SDK 请求总超时，避免覆盖 idle timeout 语义
        "transport_timeout_ms": 0,
        "min_chars": 8,
        "max_tokens": 96,
        "temperature": 0.0,
        "top_p": 0.9,
        "api_key": "",
    },
    "output": {
        "dedupe": True,
        "max_history": 5,
        "min_chars": 1,
        "method": "auto",
        "append_newline": False,
    },
    "logging": {"dir": "logs", "level": "INFO"},
}


def _merge_dict(base: Dict[str, Any], overrides: Dict[str, Any]) -> Dict[str, Any]:
    result = dict(base)
    for key, value in overrides.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = _merge_dict(result[key], value)
        else:
            result[key] = value
    return result


def _expand_env_vars(value: Any) -> Any:
    """Recursively expand environment variables in string config values."""
    if isinstance(value, dict):
        return {key: _expand_env_vars(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_expand_env_vars(item) for item in value]
    if isinstance(value, str):
        return os.path.expandvars(value)
    return value


def load_config(path: Optional[str] = None) -> Dict[str, Any]:
    """Load configuration from JSON file if provided, otherwise defaults."""

    config = dict(DEFAULT_CONFIG)
    if not path:
        return config

    expanded_path = os.path.expanduser(path)
    if not os.path.exists(expanded_path):
        raise FileNotFoundError(f"Config file not found: {expanded_path}")

    with open(expanded_path, "r", encoding="utf-8") as f:
        overrides = json.load(f)

    return _merge_dict(config, _expand_env_vars(overrides))


def ensure_logging_dir(config: Dict[str, Any]) -> str:
    """Ensure the logging directory exists and return its absolute path.
    
    日志目录相对于项目根目录（main.py 所在目录），而不是当前工作目录。
    这样即使从其他目录运行脚本，日志也能正确保存到项目目录下。
    """
    log_dir = config["logging"].get("dir", "logs")
    
    # 如果已经是绝对路径，直接使用
    if os.path.isabs(log_dir):
        pass
    else:
        # 相对路径：基于项目根目录（向上两级到达项目根目录）
        # app/config.py -> app/ -> 项目根目录
        project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        log_dir = os.path.join(project_root, log_dir)
    
    os.makedirs(log_dir, exist_ok=True)
    return log_dir
