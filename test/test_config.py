from __future__ import annotations

import importlib.util
import json
from pathlib import Path


def _load_config_module():
    module_path = Path(__file__).resolve().parents[1] / "app" / "config.py"
    spec = importlib.util.spec_from_file_location("vocotype_test_config", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_load_config_expands_env_vars_in_slm_fields(tmp_path: Path, monkeypatch) -> None:
    config_module = _load_config_module()
    monkeypatch.setenv("VOCOTYPE_TEST_ENDPOINT", "https://example.com/v1/chat/completions")
    monkeypatch.setenv("VOCOTYPE_TEST_MODEL", "test/model")
    monkeypatch.setenv("VOCOTYPE_TEST_API_KEY", "secret-token")

    config_path = tmp_path / "fcitx5-backend.json"
    config_path.write_text(
        json.dumps(
            {
                "slm": {
                    "endpoint": "${VOCOTYPE_TEST_ENDPOINT}",
                    "model": "$VOCOTYPE_TEST_MODEL",
                    "api_key": "${VOCOTYPE_TEST_API_KEY}",
                }
            }
        ),
        encoding="utf-8",
    )

    config = config_module.load_config(str(config_path))

    assert config["slm"]["endpoint"] == "https://example.com/v1/chat/completions"
    assert config["slm"]["model"] == "test/model"
    assert config["slm"]["api_key"] == "secret-token"


def test_load_config_keeps_missing_env_var_placeholder(tmp_path: Path) -> None:
    config_module = _load_config_module()
    config_path = tmp_path / "fcitx5-backend.json"
    config_path.write_text(
        json.dumps(
            {
                "slm": {
                    "endpoint": "${VOCOTYPE_MISSING_ENDPOINT}",
                }
            }
        ),
        encoding="utf-8",
    )

    config = config_module.load_config(str(config_path))

    assert config["slm"]["endpoint"] == "${VOCOTYPE_MISSING_ENDPOINT}"
