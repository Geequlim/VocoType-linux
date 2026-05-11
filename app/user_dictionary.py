"""User dictionary support for ASR text normalization."""

from __future__ import annotations

from dataclasses import dataclass
import logging
import os
from pathlib import Path
import re
from typing import Any

import yaml


logger = logging.getLogger(__name__)

USER_DICTIONARY_ENV = "VOCOTYPE_USER_DICTIONARY"
USER_DICTIONARY_FILENAME = "user-dictionary.yaml"
Span = tuple[int, int]
_MAX_NESTED_ALIAS_EXPANSIONS = 256


@dataclass(frozen=True)
class UserDictionaryResult:
    text: str
    protected_spans: tuple[Span, ...] = ()


@dataclass(frozen=True)
class UserDictionary:
    replacements: tuple[tuple[str, str, str], ...] = ()
    protected_phrases: tuple[str, ...] = ()

    def apply(self, text: str) -> UserDictionaryResult:
        if not text:
            return UserDictionaryResult("")

        rewritten, replacement_spans = self._apply_replacements(text)
        phrase_spans = _find_phrase_spans(rewritten, self.protected_phrases)
        return UserDictionaryResult(
            rewritten,
            _merge_spans((*replacement_spans, *phrase_spans)),
        )

    def _apply_replacements(self, text: str) -> tuple[str, tuple[Span, ...]]:
        if not self.replacements:
            return text, ()

        chunks: list[str] = []
        spans: list[Span] = []
        source_index = 0
        output_index = 0

        while source_index < len(text):
            matched_alias = ""
            matched_term = ""
            for alias, alias_folded, term in self.replacements:
                if _slice_casefold_equals(text, source_index, alias, alias_folded):
                    matched_alias = alias
                    matched_term = term
                    break

            if matched_alias:
                chunks.append(matched_term)
                span = (output_index, output_index + len(matched_term))
                if span[0] < span[1]:
                    spans.append(span)
                source_index += len(matched_alias)
                output_index += len(matched_term)
                continue

            char = text[source_index]
            chunks.append(char)
            source_index += 1
            output_index += len(char)

        return "".join(chunks), tuple(spans)


@dataclass
class _DictionaryCache:
    path: Path | None = None
    signature: tuple[int, int] | None = None
    dictionary: UserDictionary = UserDictionary()


_CACHE = _DictionaryCache()


def get_user_dictionary_path() -> Path:
    override = os.environ.get(USER_DICTIONARY_ENV)
    if override:
        return Path(override).expanduser()

    config_home = os.environ.get("XDG_CONFIG_HOME")
    base_dir = Path(config_home).expanduser() if config_home else Path.home() / ".config"
    return base_dir / "vocotype" / USER_DICTIONARY_FILENAME


def apply_user_dictionary(text: str) -> UserDictionaryResult:
    return load_user_dictionary().apply(text)


def load_user_dictionary() -> UserDictionary:
    path = get_user_dictionary_path()
    signature = _file_signature(path)
    previous_path = _CACHE.path
    previous_dictionary = _CACHE.dictionary

    if _CACHE.path == path and _CACHE.signature == signature:
        return _CACHE.dictionary

    if signature is None:
        _CACHE.path = path
        _CACHE.signature = None
        _CACHE.dictionary = UserDictionary()
        return _CACHE.dictionary

    try:
        dictionary = _read_user_dictionary(path)
    except Exception as exc:  # noqa: BLE001 - bad user config must not stop input.
        logger.warning("读取用户词典失败: %s: %s", path, exc)
        _CACHE.path = path
        _CACHE.signature = signature
        if previous_path == path:
            _CACHE.dictionary = previous_dictionary
        else:
            _CACHE.dictionary = UserDictionary()
        return _CACHE.dictionary

    _CACHE.path = path
    _CACHE.signature = signature
    _CACHE.dictionary = dictionary
    return dictionary


def _read_user_dictionary(path: Path) -> UserDictionary:
    with path.open("r", encoding="utf-8") as f:
        raw = yaml.load(f, Loader=yaml.BaseLoader)
    return _compile_user_dictionary(raw)


def _compile_user_dictionary(raw: Any) -> UserDictionary:
    if raw is None:
        return UserDictionary()
    if not isinstance(raw, dict):
        raise ValueError("用户词典顶层必须是映射")

    replace_config = raw.get("replace", {})
    protect_config = raw.get("protect", [])

    if replace_config is None:
        replace_config = {}
    if protect_config is None:
        protect_config = []
    if not isinstance(replace_config, dict):
        raise ValueError("replace 必须是映射")
    if not isinstance(protect_config, list):
        raise ValueError("protect 必须是字符串数组")

    replacements = _compile_replacements(replace_config)
    protected_phrases = _compile_protected_phrases(replace_config, protect_config)
    return UserDictionary(replacements=replacements, protected_phrases=protected_phrases)


def _compile_replacements(config: dict[Any, Any]) -> tuple[tuple[str, str, str], ...]:
    replacements: dict[str, tuple[str, str]] = {}
    entries: list[tuple[str, tuple[str, ...]]] = []

    for raw_term, raw_aliases in config.items():
        term = _normalize_phrase(raw_term, "replace 的标准词")
        aliases = _normalize_aliases(raw_aliases, term)
        entries.append((term, aliases))

    term_aliases = {
        term: aliases
        for term, aliases in entries
        if aliases
    }
    nested_terms = tuple(
        sorted(
            (
                (term, term.casefold(), aliases)
                for term, aliases in term_aliases.items()
            ),
            key=lambda item: (-len(item[0]), item[0]),
        )
    )

    for term, aliases in entries:
        for alias in aliases:
            for expanded_alias in _expand_nested_aliases(alias, term, nested_terms):
                if expanded_alias == term:
                    continue
                alias_key = expanded_alias.casefold()
                if alias_key in replacements and replacements[alias_key][1] != term:
                    logger.warning(
                        "用户词典别名冲突，保留首次映射: alias=%s, kept=%s, ignored=%s",
                        expanded_alias,
                        replacements[alias_key][1],
                        term,
                    )
                    continue
                replacements[alias_key] = (expanded_alias, term)

    return tuple(
        sorted(
            (
                (alias, alias_key, term)
                for alias_key, (alias, term) in replacements.items()
            ),
            key=lambda item: (-len(item[0]), item[0]),
        )
    )


def _expand_nested_aliases(
    alias: str,
    current_term: str,
    nested_terms: tuple[tuple[str, str, tuple[str, ...]], ...],
) -> tuple[str, ...]:
    current_term_key = current_term.casefold()
    expanded: list[str] = []
    seen: set[str] = set()
    truncated = False

    def add(candidate: str) -> None:
        nonlocal truncated
        if candidate in seen:
            return
        if len(expanded) >= _MAX_NESTED_ALIAS_EXPANSIONS:
            truncated = True
            return
        seen.add(candidate)
        expanded.append(candidate)

    def walk(index: int, prefix: str) -> None:
        if len(expanded) >= _MAX_NESTED_ALIAS_EXPANSIONS:
            add(prefix + alias[index:])
            return
        if index >= len(alias):
            add(prefix)
            return

        walk(index + 1, prefix + alias[index])

        for term, term_folded, aliases in nested_terms:
            if term_folded == current_term_key:
                continue
            if _slice_casefold_equals(alias, index, term, term_folded):
                for nested_alias in aliases:
                    walk(index + len(term), prefix + nested_alias)

    walk(0, "")

    if truncated:
        logger.warning(
            "用户词典嵌套别名过多，已截断: term=%s, alias=%s, limit=%s",
            current_term,
            alias,
            _MAX_NESTED_ALIAS_EXPANSIONS,
        )
    return tuple(expanded)


def _compile_protected_phrases(
    replace_config: dict[Any, Any],
    protect_config: list[Any],
) -> tuple[str, ...]:
    phrases: set[str] = set()

    for raw_term in replace_config:
        phrases.add(_normalize_phrase(raw_term, "replace 的标准词"))
    for raw_phrase in protect_config:
        phrases.add(_normalize_phrase(raw_phrase, "protect 词条"))

    return tuple(sorted(phrases, key=lambda phrase: (-len(phrase), phrase)))


def _normalize_aliases(raw_aliases: Any, term: str) -> tuple[str, ...]:
    if isinstance(raw_aliases, str):
        aliases = [raw_aliases]
    elif isinstance(raw_aliases, list):
        aliases = raw_aliases
    else:
        raise ValueError(f"{term} 的别名必须是字符串或字符串数组")

    return tuple(_normalize_phrase(alias, f"{term} 的别名") for alias in aliases)


def _normalize_phrase(raw_phrase: Any, label: str) -> str:
    if not isinstance(raw_phrase, str):
        raise ValueError(f"{label} 必须是字符串")
    phrase = raw_phrase.strip()
    if not phrase:
        raise ValueError(f"{label} 不能为空")
    return phrase


def _find_phrase_spans(text: str, phrases: tuple[str, ...]) -> tuple[Span, ...]:
    spans: list[Span] = []
    for phrase in phrases:
        for match in re.finditer(re.escape(phrase), text, flags=re.IGNORECASE):
            spans.append(match.span())
    return tuple(spans)


def _slice_casefold_equals(text: str, start: int, phrase: str, phrase_folded: str) -> bool:
    end = start + len(phrase)
    if end > len(text):
        return False
    return text[start:end].casefold() == phrase_folded


def _merge_spans(spans: tuple[Span, ...]) -> tuple[Span, ...]:
    normalized = sorted((start, end) for start, end in spans if start < end)
    if not normalized:
        return ()

    merged: list[Span] = []
    current_start, current_end = normalized[0]
    for start, end in normalized[1:]:
        if start <= current_end:
            current_end = max(current_end, end)
            continue
        merged.append((current_start, current_end))
        current_start, current_end = start, end
    merged.append((current_start, current_end))
    return tuple(merged)


def _file_signature(path: Path) -> tuple[int, int] | None:
    try:
        stat = path.stat()
    except FileNotFoundError:
        return None
    return stat.st_mtime_ns, stat.st_size


def _reset_user_dictionary_cache() -> None:
    _CACHE.path = None
    _CACHE.signature = None
    _CACHE.dictionary = UserDictionary()
