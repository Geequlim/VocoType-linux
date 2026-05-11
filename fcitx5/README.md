# VoCoType Fcitx 5

VoCoType Linux 当前唯一维护的输入法前端。

## 架构

```text
Fcitx 5
  -> C++ addon (fcitx5/addon/)
  -> Unix socket IPC
  -> Python backend (fcitx5/backend/)
  -> app/ 共享语音识别与文本处理能力
```

## 功能

- `F9` 按住说话，松开提交
- 可在输入法配置面板中设置 AI 润色默认开启和触发字数
- `Shift+F9` 临时反向切换本次录音是否 AI 润色
- Rime 拼音输入
- 本地用户词典
- 可选 LiteLLM / OpenAI-compatible 流式 SLM/LLM 润色

## 系统要求

- Fcitx 5
- Python 3.11 / 3.12
- CMake 3.10+
- pkg-config
- C++17 编译器
- `libfcitx5-dev` / `fcitx5-devel`
- `nlohmann-json3-dev` / `json-devel`

可选：

- `pyrime`
- `fcitx5-rime`

## 安装

```bash
git clone https://github.com/LeonardNJU/VocoType-linux.git
cd VocoType-linux
bash fcitx5/scripts/install-fcitx5.sh
systemctl --user enable --now vocotype-fcitx5-backend.service
fcitx5 -r
```

然后执行：

```bash
fcitx5-configtool
```

把 `VoCoType` 加到输入法列表。

## 安装脚本会做什么

1. 检查 Fcitx 5 addon 依赖
2. 编译并安装 `vocotype.so`
3. 安装 Python backend 到 `~/.local/share/vocotype-fcitx5`
4. 配置虚拟环境或复用现有 Python
5. 生成 `systemd --user` 服务
6. 配置音频设备
7. 可选写入 SLM 配置
8. 检测并记录可用 Rime schema

## 快捷键与配置

Fcitx 5 会把 addon 配置写到：

```text
~/.config/fcitx5/inputmethod/vocotype.conf
```

也可以在 Fcitx 配置工具中直接调整常用选项：

![VoCoType Fcitx 配置面板](../screenshots/fcitx5-panel.png)

常用配置项：

- `PTTKey`：默认 `F9`
- `PTTHoldThresholdMs`：按住多久后开始录音
- `PolishByDefault`：普通录音是否默认启用 AI 润色，默认开启
- `PolishMinChars`：ASR 文本达到多少字才调用 AI 润色，默认 `16`
- `PolishTimeoutMs`：AI 润色流式输出空闲超时，默认 `12000`
- `EnableThinking`：是否允许模型 thinking / reasoning 输出，默认关闭
- `LongModeModifier`：AI 润色临时切换修饰键，默认 `Shift`
- `StripTrailingPeriodOnCommit`：提交前去尾部句号

当 `PolishByDefault=true` 时，直接按 `F9` 会在达到 `PolishMinChars` 后尝试润色；
按住 `Shift+F9` 会临时跳过本次润色。当 `PolishByDefault=false` 时行为相反：
直接 `F9` 只识别，`Shift+F9` 临时启用润色。

修改后重启 `fcitx5` 即可生效。

## Backend 配置

配置文件：

```text
~/.config/vocotype/fcitx5-backend.json
```

字符串配置支持环境变量展开，格式可用 `$VAR` 或 `${VAR}`。这对远程 SLM 配置里的 `endpoint`、`model`、`api_key` 很适合。

示例：

```json
{
  "slm": {
    "endpoint": "${VOCOTYPE_SLM_ENDPOINT}",
    "model": "${VOCOTYPE_SLM_MODEL}",
    "api_key": "${VOCOTYPE_SLM_API_KEY}"
  }
}
```

### 启用文件日志

```json
{
  "logging": {
    "file": true,
    "dir": "logs",
    "level": "INFO"
  }
}
```

### 启用 SLM / AI 润色能力

```json
{
  "slm": {
    "enabled": true,
    "endpoint": "http://127.0.0.1:18080/v1/chat/completions",
    "model": "Qwen/Qwen3.5-0.8B",
    "api_key": "${COMMON_LLM_API_KEY}"
  }
}
```

SLM 统一通过 `litellm` 读取 OpenAI-compatible 流式输出。
默认会把 `model` 转成 `openai/<model>` 交给 `litellm`；只有需要显式指定
LiteLLM provider 时，才需要额外写 `litellm_model`。
`max_tokens` 不再默认传给大模型，避免长文本润色被固定输出上限截断。
触发字数、流式空闲超时与 thinking 开关在输入法配置面板中调整。

## Rime

Rime 目录使用 Fcitx 5 的标准位置：

```text
~/.local/share/fcitx5/rime/
```

推荐配合 `fcitx5-rime` 和 `rime-ice` 使用。详细说明见 [RIME_CONFIG_GUIDE.md](../RIME_CONFIG_GUIDE.md)。

## 日志与调试

查看服务日志：

```bash
journalctl --user -u vocotype-fcitx5-backend.service -f
```

前台调试：

```bash
~/.local/bin/vocotype-fcitx5-backend --debug
```

辅助分析脚本：

```bash
./scripts/analyze-rime-logs.sh
```

## 卸载

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

## 相关文档

- [项目主页](../readme.md)
- [FAQ](../docs/FAQ.md)
- [Rime 配置指南](../RIME_CONFIG_GUIDE.md)

## 上游与许可证

当前仓库是基于上游项目继续维护的 Fcitx 5 版本，来源包括：

- [LeonardNJU/VocoType-linux](https://github.com/LeonardNJU/VocoType-linux)
- [233stone/vocotype-cli](https://github.com/233stone/vocotype-cli)

感谢上游作者和贡献者提供前序实现与开源基础。

本仓库继续遵守上游开源协议，许可证见 [LICENSE](../LICENSE)，第三方依赖与模型许可见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
