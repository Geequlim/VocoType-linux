from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


user_dictionary = load_module("_vocotype_user_dictionary", ROOT / "app" / "user_dictionary.py")
text_normalizer = load_module("vocotype_text_normalizer_with_dictionary", ROOT / "app" / "text_normalizer.py")


@pytest.fixture
def dictionary_path(monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    path = tmp_path / "user-dictionary.yaml"
    monkeypatch.setenv(user_dictionary.USER_DICTIONARY_ENV, str(path))
    user_dictionary._reset_user_dictionary_cache()
    yield path
    user_dictionary._reset_user_dictionary_cache()


def write_dictionary(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")


def test_missing_user_dictionary_is_noop(dictionary_path: Path):
    assert not dictionary_path.exists()
    assert text_normalizer.normalize_text("鬼斯提 版本是一点二") == "鬼斯提 版本是1.2"


def test_user_dictionary_replaces_list_and_string_aliases(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  Ghostty:
    - 鬼斯提
    - 格斯提
  NodeJS: node js
""",
    )

    assert text_normalizer.normalize_text("鬼斯提 和 node js") == "Ghostty 和 NodeJS"


def test_user_dictionary_replacements_ignore_case(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  Ghostty: ghostty
  NodeJS: node js
""",
    )

    assert text_normalizer.normalize_text("GHOSTTY 和 Node Js") == "Ghostty 和 NodeJS"


def test_user_dictionary_treats_yaml_scalars_as_strings(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  NoSQL: no
""",
    )

    assert text_normalizer.normalize_text("no 数据库") == "NoSQL 数据库"


def test_user_dictionary_applies_even_when_number_conversion_is_disabled(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  Ghostty: 鬼斯提
""",
    )

    assert (
        text_normalizer.normalize_text("鬼斯提 版本是一点二", convert_chinese_numbers=False)
        == "Ghostty 版本是一点二"
    )


def test_user_dictionary_protects_configured_phrases(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
protect:
  - 一百米计划
""",
    )

    assert text_normalizer.normalize_text("一百米计划启动，一百米") == "一百米计划启动，100米"


def test_user_dictionary_protects_configured_phrases_ignore_case(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
protect:
  - API一百二十
""",
    )

    assert text_normalizer.normalize_text("api一百二十启动，设置成一百二十") == "api一百二十启动，设置成120"


def test_chinese_number_normalizer_respects_protected_spans(dictionary_path: Path):
    text = "一百米计划启动，一百米"

    assert (
        text_normalizer.normalize_chinese_numbers(text, protected_spans=((0, 5),))
        == "一百米计划启动，100米"
    )


def test_user_dictionary_protects_replacement_terms(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  一百米计划: hundred meter plan
""",
    )

    assert (
        text_normalizer.normalize_text("hundred meter plan启动，一百米")
        == "一百米计划启动，100米"
    )


def test_user_dictionary_uses_longest_alias_first(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  短词: alpha
  长词: alpha beta
""",
    )

    assert text_normalizer.normalize_text("alpha beta") == "长词"


def test_user_dictionary_expands_nested_terms_in_aliases(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  README:
    - read me
    - readme
  README.md:
    - README点MD
    - README文件
""",
    )

    assert text_normalizer.normalize_text("read me点md") == "README.md"
    assert text_normalizer.normalize_text("readme文件") == "README.md"
    assert text_normalizer.normalize_text("read me") == "README"


def test_user_dictionary_reloads_when_file_changes(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  NodeJS: node js
""",
    )
    assert text_normalizer.normalize_text("node js") == "NodeJS"

    write_dictionary(
        dictionary_path,
        """
replace:
  Ghostty: 鬼斯提
""",
    )
    assert text_normalizer.normalize_text("鬼斯提") == "Ghostty"
    assert text_normalizer.normalize_text("node js") == "node js"


def test_user_dictionary_keeps_previous_dictionary_when_reload_fails(dictionary_path: Path):
    write_dictionary(
        dictionary_path,
        """
replace:
  Ghostty: 鬼斯提
""",
    )
    assert text_normalizer.normalize_text("鬼斯提") == "Ghostty"

    write_dictionary(dictionary_path, "replace: [\n")
    assert text_normalizer.normalize_text("鬼斯提") == "Ghostty"


def test_user_dictionary_uses_xdg_config_home(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    monkeypatch.delenv(user_dictionary.USER_DICTIONARY_ENV, raising=False)
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "xdg-config"))
    user_dictionary._reset_user_dictionary_cache()

    assert (
        user_dictionary.get_user_dictionary_path()
        == tmp_path / "xdg-config" / "vocotype" / "user-dictionary.yaml"
    )
