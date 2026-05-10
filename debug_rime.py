#!/usr/bin/env python3
"""调试 VoCoType 的 Fcitx 5 Rime 集成。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))


def test_rime():
    print("=== VoCoType Fcitx5 Rime 调试测试 ===\n")

    print("[1] 检查 pyrime...")
    try:
        import pyrime

        print(f"    ✓ pyrime 版本: {pyrime.__version__}")
    except ImportError as e:
        print(f"    ✗ pyrime 不可用: {e}")
        return False

    print("\n[2] 检查目录...")
    fcitx_rime_dir = Path.home() / ".local" / "share" / "fcitx5" / "rime"
    user_data_dir = fcitx_rime_dir
    print(f"    Fcitx5 Rime 目录: {user_data_dir}")
    print(f"    存在: {user_data_dir.exists()}")

    shared_dirs = [
        Path("/usr/share/rime-data"),
        Path("/usr/local/share/rime-data"),
    ]
    shared_data_dir = next((d for d in shared_dirs if d.exists()), None)
    print(f"    共享数据目录: {shared_data_dir}")

    log_dir = Path.home() / ".local" / "share" / "vocotype-fcitx5" / "rime"
    print(f"    日志目录: {log_dir}")
    log_dir.mkdir(parents=True, exist_ok=True)
    user_data_dir.mkdir(parents=True, exist_ok=True)

    if shared_data_dir is None:
        print("    ✗ 找不到共享数据目录")
        return False

    print("\n[3] 初始化 Rime Session...")
    try:
        from pyrime.api import API, Traits
        from pyrime.session import Session

        traits = Traits(
            shared_data_dir=str(shared_data_dir),
            user_data_dir=str(user_data_dir),
            log_dir=str(log_dir),
            distribution_name="VoCoType",
            distribution_code_name="vocotype-fcitx5",
            distribution_version="1.0",
            app_name="rime.vocotype.fcitx5",
        )

        api = API()
        session = Session(traits=traits, api=api)
        schema = session.get_current_schema()
        print("    ✓ Session 创建成功")
        print(f"    当前方案: {schema}")
    except Exception as e:
        print(f"    ✗ 初始化失败: {e}")
        import traceback

        traceback.print_exc()
        return False

    print("\n[4] 测试按键处理...")
    test_keys = [
        ("n", ord("n"), "输入 'n'"),
        ("i", ord("i"), "输入 'i'"),
        ("h", ord("h"), "输入 'h'"),
        ("a", ord("a"), "输入 'a'"),
        ("o", ord("o"), "输入 'o'"),
    ]

    for _, keyval, desc in test_keys:
        try:
            handled = session.process_key(keyval, 0)
            commit = session.get_commit()
            context = session.get_context()

            preedit = ""
            if context and context.composition:
                preedit = context.composition.preedit or ""

            commit_text = ""
            if commit and commit.text:
                commit_text = commit.text

            print(f"    {desc}: handled={handled}, preedit='{preedit}', commit='{commit_text}'")
        except Exception as e:
            print(f"    {desc}: 失败 - {e}")

    print("\n[5] 测试空格选词...")
    try:
        handled = session.process_key(0x20, 0)
        commit = session.get_commit()
        commit_text = commit.text if commit else ""
        print(f"    空格: handled={handled}, commit='{commit_text}'")
    except Exception as e:
        print(f"    空格: 失败 - {e}")

    print("\n=== 测试完成 ===")
    return True


if __name__ == "__main__":
    test_rime()
