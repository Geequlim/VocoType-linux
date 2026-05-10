# VoCoType Linux FAQ

本文档只覆盖当前仍在维护的 **Fcitx 5** 版本。

## 1. 支持哪些环境

- Linux
- Fcitx 5
- Python 3.11 / 3.12

Python 3.13 目前不在支持范围内，主要受 `onnxruntime` 生态限制。

## 2. 安装时提示缺少编译依赖

先补齐 Fcitx 5 addon 所需系统包。

Debian / Ubuntu:

```bash
sudo apt install cmake pkg-config libfcitx5-dev nlohmann-json3-dev
```

Fedora:

```bash
sudo dnf install cmake pkgconfig fcitx5-devel json-devel
```

Arch:

```bash
sudo pacman -S cmake pkgconfig fcitx5 nlohmann-json
```

## 3. Python 版本不兼容

安装脚本要求 Python 3.11-3.12。

检查版本：

```bash
python3 --version
```

如果系统没有合适版本，优先使用 `uv`：

```bash
pip install uv
```

然后重新运行安装脚本即可。

## 4. backend 无法启动

先看 systemd 状态：

```bash
systemctl --user status vocotype-fcitx5-backend.service
journalctl --user -u vocotype-fcitx5-backend.service -f
```

也可以前台直接启动：

```bash
~/.local/bin/vocotype-fcitx5-backend --debug
```

## 5. Fcitx 5 里找不到 VoCoType

先确认 addon 文件已经安装：

```bash
ls ~/.local/lib/fcitx5/vocotype.so
ls ~/.local/share/fcitx5/addon/vocotype.conf
ls ~/.local/share/fcitx5/inputmethod/vocotype.conf
```

然后重启 Fcitx 5：

```bash
fcitx5 -r
```

最后打开配置工具手动添加：

```bash
fcitx5-configtool
```

## 6. F9 没反应

依次检查：

1. backend 服务是否在运行
2. 麦克风是否配置完成
3. 当前输入法是否已经切换到 `VoCoType`

检查 backend 进程：

```bash
pgrep -fa fcitx5_server.py
```

重新配置音频：

```bash
~/.local/share/vocotype-fcitx5/.venv/bin/python \
  ~/.local/share/vocotype-fcitx5/scripts/setup-audio.py
```

## 7. Rime 拼音不可用

确认 Fcitx 5 的 Rime 目录存在：

```bash
ls ~/.local/share/fcitx5/rime/
```

如果目录不存在，先安装并使用一次 `fcitx5-rime`，让它完成 Rime 部署后再重新运行安装脚本。

如果 `pyrime` 缺失：

```bash
~/.local/share/vocotype-fcitx5/.venv/bin/pip install pyrime
```

## 8. 如何看日志

默认 backend 日志输出到 stderr / systemd journal。

如果需要文件日志，在 `~/.config/vocotype/fcitx5-backend.json` 里添加：

```json
{
  "logging": {
    "file": true,
    "dir": "logs",
    "level": "INFO"
  }
}
```

启用后日志目录默认是：

```text
~/.local/share/vocotype-fcitx5/logs/
```

辅助脚本：

```bash
./scripts/analyze-rime-logs.sh
```

## 9. 如何验证 IPC 正常

```bash
echo '{"type":"ping"}' | nc -N -U /tmp/vocotype-fcitx5.sock
```

返回 `{"pong": true}` 说明 backend socket 正常。

## 10. 如何卸载

```bash
systemctl --user disable --now vocotype-fcitx5-backend.service
rm -rf ~/.local/share/vocotype-fcitx5
rm -f ~/.local/lib/fcitx5/vocotype.so
rm -f ~/.local/share/fcitx5/addon/vocotype.conf
rm -f ~/.local/share/fcitx5/inputmethod/vocotype.conf
rm -f ~/.local/bin/vocotype-fcitx5-backend
rm -f ~/.local/bin/vocotype-fcitx5-recorder
rm -f ~/.config/systemd/user/vocotype-fcitx5-backend.service
fcitx5 -r
```
