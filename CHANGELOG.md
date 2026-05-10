# Changelog

All notable changes to this fork are documented in this file.

## [Unreleased]

### Changed

- 项目口径调整为独立维护的 Fcitx 5 版本
- 根文档、FAQ、Rime 指南和 Fcitx 5 安装文档已改为 fcitx5-only
- `pyproject.toml` 与 `requirements.txt` 已移除 IBus 相关元数据

### Removed

- 删除 `ibus/` 运行时代码
- 删除 IBus 安装、卸载、调试和测试脚本
- 删除 IBus 组件模板与安装验证测试
