from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import types


MODULE_PATH = Path(__file__).resolve().parents[1] / "app" / "slm_polisher.py"
SPEC = importlib.util.spec_from_file_location("vocotype_slm_polisher", MODULE_PATH)
assert SPEC and SPEC.loader
slm_polisher = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = slm_polisher
SPEC.loader.exec_module(slm_polisher)
SLMPolisher = slm_polisher.SLMPolisher


def test_disabled_stream_returns_original():
    polisher = SLMPolisher({"enabled": False})

    events = list(polisher.stream_polish("测试文本", long_mode=True))
    out, metrics = polisher.polish("测试文本", long_mode=True)

    assert events == [{"kind": "final", "text": "测试文本", "reason": "disabled"}]
    assert out == "测试文本"
    assert metrics.used is False
    assert metrics.reason == "disabled"


def test_not_long_mode_stream_returns_original():
    polisher = SLMPolisher({"enabled": True})

    events = list(polisher.stream_polish("测试文本", long_mode=False))
    out, metrics = polisher.polish("测试文本", long_mode=False)

    assert events == [{"kind": "final", "text": "测试文本", "reason": "not_long_mode"}]
    assert out == "测试文本"
    assert metrics.used is False
    assert metrics.reason == "not_long_mode"


def test_too_short_stream_returns_original():
    polisher = SLMPolisher({"enabled": True, "min_chars": 20})

    events = list(polisher.stream_polish("太短", long_mode=True))
    out, metrics = polisher.polish("太短", long_mode=True)

    assert events == [{"kind": "final", "text": "太短", "reason": "too_short"}]
    assert out == "太短"
    assert metrics.used is False
    assert metrics.reason == "too_short"


def test_min_chars_override_controls_polish_threshold():
    polisher = SLMPolisher({"enabled": True, "min_chars": 20})

    assert polisher.should_polish("八个字以上文本", long_mode=True) is False
    assert (
        polisher.should_polish(
            "八个字以上文本",
            long_mode=True,
            min_chars=4,
        )
        is True
    )
    assert list(
        polisher.stream_polish(
            "短文本",
            long_mode=True,
            min_chars=10,
        )
    ) == [{"kind": "final", "text": "短文本", "reason": "too_short"}]


def test_stream_polish_litellm_chunks(monkeypatch):
    polisher = SLMPolisher(
        {
            "enabled": True,
            "min_chars": 1,
            "endpoint": "http://test.local/v1/chat/completions",
            "model": "Qwen/Qwen3.5-0.8B",
        }
    )

    def _fake_stream(text, **_kwargs):
        assert text == "原始文本"
        return [
            {"choices": [{"delta": {"content": "润色后"}}]},
            {"choices": [{"delta": {"content": "的文本。"}}]},
        ]

    monkeypatch.setattr(polisher, "_create_litellm_stream", _fake_stream)
    events = list(polisher.stream_polish("原始文本", long_mode=True))
    out, metrics = polisher.polish("原始文本", long_mode=True)

    assert events[0]["kind"] == "status"
    assert events[1]["kind"] == "delta"
    assert events[1]["preview"] == "润色后"
    assert events[2]["preview"] == "润色后的文本。"
    assert events[3]["kind"] == "final"
    assert events[3]["text"] == "润色后的文本。"
    assert out == "润色后的文本。"
    assert metrics.used is True
    assert metrics.applied is True
    assert metrics.reason == "ok"


def test_stream_polish_keeps_alive_on_empty_chunks(monkeypatch):
    polisher = SLMPolisher({"enabled": True, "min_chars": 1})

    def _fake_stream(_text, **_kwargs):
        return [
            {"choices": [{"delta": {"role": "assistant"}}]},
            {"choices": [{"delta": {"content": "润色结果"}}]},
        ]

    monkeypatch.setattr(polisher, "_create_litellm_stream", _fake_stream)
    events = list(polisher.stream_polish("原始文本", long_mode=True))

    assert events[1]["kind"] == "heartbeat"
    assert events[2]["kind"] == "delta"
    assert events[-1]["text"] == "润色结果"


def test_stream_error_when_litellm_fails(monkeypatch):
    polisher = SLMPolisher({"enabled": True, "min_chars": 1})

    def _fail(_text, **_kwargs):
        raise RuntimeError("network failed")

    monkeypatch.setattr(polisher, "_create_litellm_stream", _fail)
    events = list(polisher.stream_polish("原始文本", long_mode=True))
    out, metrics = polisher.polish("原始文本", long_mode=True)

    assert events[-1]["kind"] == "error"
    assert events[-1]["reason"] == "request_error"
    assert "network failed" in events[-1]["message"]
    assert out == "原始文本"
    assert metrics.used is True
    assert metrics.applied is False
    assert metrics.reason == "request_error"


def test_litellm_api_base_from_chat_completions_endpoint():
    polisher = SLMPolisher(
        {
            "enabled": True,
            "endpoint": "http://127.0.0.1:18080/v1/chat/completions",
        }
    )

    assert polisher._remote_api_base() == "http://127.0.0.1:18080/v1"
    assert polisher.litellm_model == "openai/Qwen/Qwen3.5-0.8B"


def test_litellm_model_defaults_to_configured_model():
    polisher = SLMPolisher(
        {
            "enabled": True,
            "model": "deepseek/deepseek-v4-flash",
        }
    )

    assert polisher.litellm_model == "openai/deepseek/deepseek-v4-flash"


def test_stale_qwen_litellm_model_does_not_override_configured_model():
    polisher = SLMPolisher(
        {
            "enabled": True,
            "model": "deepseek/deepseek-v4-flash",
            "litellm_model": "openai/Qwen/Qwen3.5-0.8B",
        }
    )

    assert polisher.litellm_model == "openai/deepseek/deepseek-v4-flash"


def test_openrouter_enable_thinking_false_maps_to_reasoning_extra_body():
    polisher = SLMPolisher(
        {
            "enabled": True,
            "endpoint": "https://openrouter.ai/api/v1/chat/completions",
            "model": "deepseek/deepseek-v4-flash",
            "enable_thinking": False,
        }
    )

    assert polisher._request_extra_body() == {
        "reasoning": {"effort": "none", "exclude": True},
        "include_reasoning": False,
    }


def test_openrouter_adds_default_extra_headers():
    polisher = SLMPolisher(
        {
            "enabled": True,
            "endpoint": "https://openrouter.ai/api/v1/chat/completions",
        }
    )

    assert polisher._request_extra_headers() == {
        "HTTP-Referer": "https://github.com/geequlim/VocoType-linux",
        "X-Title": "VoCoType",
    }


def test_extra_headers_override_openrouter_defaults():
    polisher = SLMPolisher(
        {
            "enabled": True,
            "endpoint": "https://openrouter.ai/api/v1/chat/completions",
            "extra_headers": {
                "HTTP-Referer": "https://example.test/app",
                "X-Title": "Custom App",
            },
        }
    )

    assert polisher._request_extra_headers() == {
        "HTTP-Referer": "https://example.test/app",
        "X-Title": "Custom App",
    }


def test_enable_thinking_request_override_takes_precedence():
    polisher = SLMPolisher(
        {
            "enabled": True,
            "endpoint": "https://openrouter.ai/api/v1/chat/completions",
            "model": "deepseek/deepseek-v4-flash",
            "enable_thinking": False,
        }
    )

    assert polisher._request_extra_body(enable_thinking=True) == {
        "reasoning": {"enabled": True},
    }


def test_create_litellm_stream_does_not_send_max_tokens(monkeypatch):
    captured = {}
    fake_litellm = types.ModuleType("litellm")

    def _completion(**kwargs):
        captured.update(kwargs)
        return []

    fake_litellm.completion = _completion
    monkeypatch.setitem(sys.modules, "litellm", fake_litellm)

    polisher = SLMPolisher(
        {
            "enabled": True,
            "endpoint": "https://openrouter.ai/api/v1/chat/completions",
            "model": "deepseek/deepseek-v4-flash",
            "max_tokens": 512,
        }
    )

    assert list(polisher._create_litellm_stream("原始文本")) == []
    assert "max_tokens" not in captured
    assert captured["extra_headers"] == {
        "HTTP-Referer": "https://github.com/geequlim/VocoType-linux",
        "X-Title": "VoCoType",
    }


def test_extract_stream_delta_accepts_message_content():
    assert (
        SLMPolisher._extract_stream_delta(
            {"choices": [{"message": {"content": "最终内容"}}]}
        )
        == "最终内容"
    )


def test_normalize_remote_endpoint():
    assert (
        SLMPolisher._normalize_remote_endpoint("http://8.153.102.23:13001/")
        == "http://8.153.102.23:13001/v1/chat/completions"
    )
    assert (
        SLMPolisher._normalize_remote_endpoint("http://8.153.102.23:13001/v1")
        == "http://8.153.102.23:13001/v1/chat/completions"
    )
    assert (
        SLMPolisher._normalize_remote_endpoint(
            "http://8.153.102.23:13001/v1/chat/completions"
        )
        == "http://8.153.102.23:13001/v1/chat/completions"
    )


def test_is_failure_reason():
    assert SLMPolisher.is_failure_reason("ok") is False
    assert SLMPolisher.is_failure_reason("too_short") is False
    assert SLMPolisher.is_failure_reason("request_error") is True
    assert SLMPolisher.is_failure_reason("idle_timeout") is True


def test_format_failure_message():
    assert SLMPolisher.format_failure_message("timeout") == "SLM 调用失败：请求超时"
    assert (
        SLMPolisher.format_failure_message("idle_timeout")
        == "SLM 调用失败：长时间未收到模型输出"
    )
    assert (
        SLMPolisher.format_failure_message("litellm_not_installed")
        == "SLM 调用失败：缺少 litellm 依赖"
    )


def test_strip_thinking_tag_block():
    text = "<think>分析过程</think>\n我今天去那家公司面试了，感觉还可以。"
    assert SLMPolisher._strip_thinking_content(text) == "我今天去那家公司面试了，感觉还可以。"


def test_strip_thinking_process_with_final_marker():
    text = (
        "Thinking Process:\n"
        "1. Fix punctuation.\n"
        "2. Keep meaning.\n\n"
        "Final Answer: 我今天去那家公司面试了，感觉还可以。"
    )
    assert SLMPolisher._strip_thinking_content(text) == "我今天去那家公司面试了，感觉还可以。"


def test_strip_thinking_only_returns_empty():
    text = "Thinking Process: The user asks for post-processing and punctuation fixes."
    assert SLMPolisher._strip_thinking_content(text) == ""
