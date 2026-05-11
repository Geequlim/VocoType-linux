"""Streaming LLM text polisher used by long-form voice mode."""

from __future__ import annotations

import logging
import re
import time
import urllib.parse
from dataclasses import dataclass
from typing import Any, Dict, Iterator, Tuple


logger = logging.getLogger(__name__)


DEFAULT_SYSTEM_PROMPT = """你是中文语音转写文本的后处理器。

目标：在不改变原意、不新增事实的前提下，做最小必要修正，让文本通顺、自然、易读。

仅允许：
1. 补充/修改/删除标点
2. 调整断句与分句
3. 删除明显口头禅、重复词、无意义语气词
4. 修正明显同音/近音错词、漏字、多字
5. 原句明显不通顺时，做最小限度顺句

核心约束：
- 最小编辑：能不改就不改，能少改就少改
- 含义守恒：不新增事实、细节、观点、结论；不扩写、不解释、不总结
- 技术字符串保真：英文、缩写、模型名、版本号、路径、命令、参数、代码片段按原样优先保留
- 形式保真：技术标识中的大小写、数字、连字符(-)、斜杠(/)、下划线(_)、小数点(.)尽量不改写
- 技术词纠偏：若技术词存在明显转写偏差（同音/近形/单字符误差）且上下文可确定，可做最小字符级修正
- 混排保真：字母数字混合标识保持字母/数字角色，不把字母读音替换成数字或汉字
- 术语优先：若有多个近似写法，优先更常见的技术术语拼写
- 数字规范：默认保留阿拉伯数字，非固定汉语表达不要改成汉字
- 不确定时保留原样，避免误改

输出要求：只输出最终文本，不要任何说明。"""


@dataclass
class PolisherMetrics:
    """Compatibility metrics for callers that still need a final string."""

    used: bool
    applied: bool
    latency_ms: float
    reason: str

    def to_log_dict(self) -> Dict[str, Any]:
        return {
            "used": self.used,
            "applied": self.applied,
            "latency_ms": round(self.latency_ms, 2),
            "reason": self.reason,
        }


class SLMPolisher:
    """Single streaming LLM polisher implementation based on LiteLLM."""

    _NON_FAILURE_REASONS = {
        "ok",
        "disabled",
        "not_long_mode",
        "too_short",
    }
    _THINKING_PREFIX_RE = re.compile(
        r"^\s*(?:thinking\s*process|thought\s*process|reasoning|analysis|chain\s*of\s*thought|思考过程|推理过程|分析过程)\s*[:：]",
        flags=re.IGNORECASE,
    )
    _FINAL_ANSWER_MARKER_RE = re.compile(
        r"(?:(?:^|\n)\s*)(?:final\s*answer|final\s*response|answer|最终答案|最终输出|润色结果|输出结果|输出)\s*[:：]",
        flags=re.IGNORECASE,
    )
    _REASONING_LINE_RE = re.compile(
        r"^\s*(?:"
        r"(?:thinking\s*process|thought\s*process|reasoning|analysis|chain\s*of\s*thought|let'?s\s+think|step\s*\d*)"
        r"|(?:思考过程|推理过程|分析过程|推理|分析|思路)"
        r"|(?:\d+[\.\)]\s+)"
        r"|(?:[-*]\s+)"
        r")",
        flags=re.IGNORECASE,
    )

    def __init__(self, config: Dict[str, Any] | None = None):
        cfg = dict(config or {})
        self.enabled = bool(cfg.get("enabled", False))
        self.endpoint = self._normalize_remote_endpoint(
            str(cfg.get("endpoint", "http://127.0.0.1:18080/v1/chat/completions"))
        )
        self.model = str(cfg.get("model", "Qwen/Qwen3.5-0.8B")).strip()
        self.litellm_model = self._resolve_litellm_model(
            self.model,
            cfg.get("litellm_model"),
        )
        self.timeout_ms = int(cfg.get("timeout_ms", 12000))
        self.transport_timeout_ms = max(0, int(cfg.get("transport_timeout_ms", 0)))
        self.min_chars = max(1, int(cfg.get("min_chars", 16)))
        self.temperature = float(cfg.get("temperature", 0.0))
        self.top_p = float(cfg.get("top_p", 0.9))
        self.api_key = str(cfg.get("api_key", "")).strip()
        self.system_prompt = str(cfg.get("system_prompt", DEFAULT_SYSTEM_PROMPT))
        self.enable_thinking = self._optional_bool(cfg.get("enable_thinking"))
        extra_body = cfg.get("extra_body", {})
        self.extra_body = dict(extra_body) if isinstance(extra_body, dict) else {}

    def should_polish(
        self,
        text: str,
        *,
        long_mode: bool,
        min_chars: int | None = None,
    ) -> bool:
        threshold = self._effective_min_chars(min_chars)
        return self.enabled and long_mode and len((text or "").strip()) >= threshold

    def stream_polish(
        self,
        text: str,
        *,
        long_mode: bool,
        min_chars: int | None = None,
        enable_thinking: bool | None = None,
    ) -> Iterator[Dict[str, Any]]:
        """Yield normalized streaming events for the input panel."""

        original = text or ""
        if not self.enabled:
            yield {"kind": "final", "text": original, "reason": "disabled"}
            return

        if not long_mode:
            yield {"kind": "final", "text": original, "reason": "not_long_mode"}
            return

        stripped = original.strip()
        if len(stripped) < self._effective_min_chars(min_chars):
            yield {"kind": "final", "text": original, "reason": "too_short"}
            return

        start = time.perf_counter()
        full = ""
        try:
            yield {"kind": "status", "text": "正在调用大模型..."}
            chunk_count = 0
            content_chunk_count = 0
            for chunk in self._iter_litellm_chunks(
                self._create_litellm_stream(
                    stripped,
                    enable_thinking=enable_thinking,
                )
            ):
                chunk_count += 1
                delta = self._extract_stream_delta(chunk)
                if not delta:
                    yield {"kind": "heartbeat"}
                    continue
                content_chunk_count += 1
                full += delta
                yield {"kind": "delta", "text": delta, "preview": full}

            polished = self._strip_thinking_content(full).strip()
            if not polished:
                reason = "blank_content" if full else "empty_content"
                logger.warning(
                    "SLM stream returned no usable content: reason=%s chunks=%s content_chunks=%s",
                    reason,
                    chunk_count,
                    content_chunk_count,
                )
                yield {
                    "kind": "error",
                    "reason": reason,
                    "message": self.format_failure_message(reason),
                    "latency_ms": (time.perf_counter() - start) * 1000.0,
                }
                return

            yield {
                "kind": "final",
                "text": polished,
                "reason": "ok",
                "latency_ms": (time.perf_counter() - start) * 1000.0,
            }
        except ImportError:
            yield {
                "kind": "error",
                "reason": "litellm_not_installed",
                "message": self.format_failure_message("litellm_not_installed"),
            }
        except Exception as exc:  # noqa: BLE001
            logger.warning("SLM LiteLLM stream failed: %s", exc)
            yield {
                "kind": "error",
                "reason": "request_error",
                "message": self._format_exception_message(exc),
            }

    def polish(self, text: str, *, long_mode: bool) -> Tuple[str, PolisherMetrics]:
        """Compatibility wrapper; the only implementation still uses streaming."""

        start = time.perf_counter()
        original = text or ""
        final_text = original
        reason = "empty_content"
        used = self.enabled and long_mode

        for event in self.stream_polish(original, long_mode=long_mode):
            kind = str(event.get("kind", ""))
            if kind == "final":
                final_text = str(event.get("text", original))
                reason = str(event.get("reason", "ok"))
                used = reason not in {"disabled", "not_long_mode", "too_short"}
                break
            if kind == "error":
                reason = str(event.get("reason", "request_error"))
                final_text = original
                used = True
                break

        return final_text, PolisherMetrics(
            used=used,
            applied=final_text != original,
            latency_ms=(time.perf_counter() - start) * 1000.0,
            reason=reason,
        )

    def _create_litellm_stream(
        self,
        stripped: str,
        *,
        enable_thinking: bool | None = None,
    ) -> Any:
        from litellm import completion

        kwargs: Dict[str, Any] = {
            "model": self.litellm_model,
            "api_base": self._remote_api_base(),
            "api_key": self.api_key or "EMPTY",
            "messages": [
                {"role": "system", "content": self.system_prompt},
                {"role": "user", "content": f"原文：{stripped}\n输出："},
            ],
            "temperature": self.temperature,
            "top_p": self.top_p,
            "stream": True,
        }
        if self.transport_timeout_ms > 0:
            kwargs["timeout"] = max(0.05, self.transport_timeout_ms / 1000.0)
        extra_body = self._request_extra_body(enable_thinking=enable_thinking)
        if extra_body:
            kwargs["extra_body"] = extra_body
        return completion(**kwargs)

    def _request_extra_body(
        self,
        *,
        enable_thinking: bool | None = None,
    ) -> Dict[str, Any]:
        extra_body = dict(self.extra_body)
        thinking_enabled = self.enable_thinking if enable_thinking is None else enable_thinking
        if thinking_enabled is None:
            return extra_body

        api_base = self._remote_api_base().lower()
        if "openrouter.ai" in api_base:
            if thinking_enabled:
                extra_body.setdefault("reasoning", {"enabled": True})
            else:
                extra_body.setdefault(
                    "reasoning",
                    {"effort": "none", "exclude": True},
                )
                extra_body.setdefault("include_reasoning", False)
            return extra_body

        extra_body.setdefault("enable_thinking", thinking_enabled)
        return extra_body

    def _effective_min_chars(self, override: int | None) -> int:
        if override is None:
            return self.min_chars
        return max(1, int(override))

    def _remote_api_base(self) -> str:
        parsed = urllib.parse.urlparse(self.endpoint)
        if not parsed.scheme or not parsed.netloc:
            return self.endpoint

        path = parsed.path.rstrip("/")
        if path.endswith("/chat/completions"):
            path = path[: -len("/chat/completions")]
        if not path:
            path = "/v1"
        return urllib.parse.urlunparse(
            parsed._replace(path=path, params="", query="", fragment="")
        )

    @classmethod
    def is_failure_reason(cls, reason: str) -> bool:
        return str(reason or "").strip() not in cls._NON_FAILURE_REASONS

    @staticmethod
    def format_failure_message(reason: str) -> str:
        normalized = str(reason or "").strip()
        if not normalized:
            return "SLM 调用失败"
        if normalized == "timeout":
            return "SLM 调用失败：请求超时"
        if normalized == "idle_timeout":
            return "SLM 调用失败：长时间未收到模型输出"
        if normalized == "request_error":
            return "SLM 调用失败：请求错误"
        if normalized == "litellm_not_installed":
            return "SLM 调用失败：缺少 litellm 依赖"
        if normalized == "empty_content":
            return "SLM 调用失败：返回内容为空"
        if normalized == "blank_content":
            return "SLM 调用失败：润色结果为空"
        if normalized == "thinking_only":
            return "SLM 调用失败：仅返回思考内容"
        if normalized == "exception":
            return "SLM 调用失败：运行异常"
        return f"SLM 调用失败：{normalized}"

    @staticmethod
    def _format_exception_message(exc: BaseException) -> str:
        detail = " ".join(str(exc).strip().split())
        if not detail:
            return SLMPolisher.format_failure_message("request_error")
        if len(detail) > 240:
            detail = f"{detail[:237]}..."
        return f"SLM 调用失败：{detail}"

    @staticmethod
    def _normalize_remote_endpoint(endpoint: str) -> str:
        text = str(endpoint or "").strip()
        if not text:
            return "http://127.0.0.1:18080/v1/chat/completions"

        parsed = urllib.parse.urlparse(text)
        if not parsed.scheme or not parsed.netloc:
            return text

        path = parsed.path or ""
        stripped_path = path.rstrip("/")
        if stripped_path in {"", "/"}:
            path = "/v1/chat/completions"
        elif stripped_path == "/v1":
            path = "/v1/chat/completions"

        return urllib.parse.urlunparse(parsed._replace(path=path))

    @staticmethod
    def _default_litellm_model(model: str) -> str:
        normalized = str(model or "").strip()
        if not normalized:
            return "openai/Qwen/Qwen3.5-0.8B"
        provider_prefixes = (
            "openai/",
            "azure/",
            "anthropic/",
            "ollama/",
            "openrouter/",
            "hosted_vllm/",
            "text-completion-openai/",
        )
        if normalized.startswith(provider_prefixes):
            return normalized
        return f"openai/{normalized}"

    @classmethod
    def _resolve_litellm_model(cls, model: str, configured: Any) -> str:
        normalized_model = str(model or "").strip()
        normalized_configured = str(configured or "").strip()
        legacy_default = "openai/Qwen/Qwen3.5-0.8B"

        if (
            normalized_configured
            and normalized_configured != legacy_default
            and normalized_configured != "Qwen/Qwen3.5-0.8B"
        ):
            return normalized_configured

        if normalized_configured in {legacy_default, "Qwen/Qwen3.5-0.8B"} and (
            not normalized_model or normalized_model == "Qwen/Qwen3.5-0.8B"
        ):
            return legacy_default

        return cls._default_litellm_model(normalized_model)

    @staticmethod
    def _optional_bool(value: Any) -> bool | None:
        if value is None:
            return None
        if isinstance(value, bool):
            return value
        if isinstance(value, str):
            normalized = value.strip().lower()
            if normalized in {"1", "true", "yes", "on"}:
                return True
            if normalized in {"0", "false", "no", "off"}:
                return False
        return bool(value)

    @classmethod
    def _iter_litellm_chunks(cls, response: Any) -> Iterator[Any]:
        if cls._field(response, "choices") is not None:
            yield response
            return
        yield from response

    @staticmethod
    def _field(value: Any, key: str) -> Any:
        if value is None:
            return None
        if isinstance(value, dict):
            return value.get(key)
        if hasattr(value, "model_dump"):
            try:
                data = value.model_dump()
                if isinstance(data, dict) and key in data:
                    return data.get(key)
            except Exception:  # noqa: BLE001
                pass
        if hasattr(value, "dict"):
            try:
                data = value.dict()
                if isinstance(data, dict) and key in data:
                    return data.get(key)
            except Exception:  # noqa: BLE001
                pass
        return getattr(value, key, None)

    @classmethod
    def _coerce_text(cls, value: Any) -> str:
        if isinstance(value, str):
            return value
        if isinstance(value, list):
            parts = []
            for item in value:
                if isinstance(item, str):
                    parts.append(item)
                    continue
                text = cls._field(item, "text")
                if isinstance(text, str):
                    parts.append(text)
            return "".join(parts)
        return ""

    @classmethod
    def _extract_stream_delta(cls, chunk: Any) -> str:
        choices = cls._field(chunk, "choices")

        if not choices:
            return cls._coerce_text(cls._field(chunk, "content")) or cls._coerce_text(
                cls._field(chunk, "text")
            )

        first = choices[0]
        for container in (
            cls._field(first, "delta"),
            cls._field(first, "message"),
            first,
        ):
            content = cls._coerce_text(cls._field(container, "content"))
            if content:
                return content
            text = cls._coerce_text(cls._field(container, "text"))
            if text:
                return text

        return ""

    @staticmethod
    def _strip_thinking_content(content: str) -> str:
        text = str(content or "")
        if not text:
            return ""

        text = re.sub(r"<think>.*?</think>", "", text, flags=re.DOTALL)
        if "<think>" in text:
            text = text.split("<think>", 1)[0]
        text = text.strip()
        if not text:
            return ""

        marker_matches = list(SLMPolisher._FINAL_ANSWER_MARKER_RE.finditer(text))
        if marker_matches:
            candidate = text[marker_matches[-1].end() :].strip()
            if candidate:
                text = candidate
            else:
                return ""

        if not SLMPolisher._THINKING_PREFIX_RE.match(text):
            return text

        paragraphs = [p.strip() for p in re.split(r"\n\s*\n+", text) if p.strip()]
        if len(paragraphs) >= 2:
            last_para = paragraphs[-1]
            if not SLMPolisher._is_reasoning_line(last_para):
                return last_para

        lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
        for line in reversed(lines):
            if SLMPolisher._is_reasoning_line(line):
                continue
            return line
        return ""

    @classmethod
    def _is_reasoning_line(cls, text: str) -> bool:
        return bool(cls._REASONING_LINE_RE.match(str(text or "").strip()))
