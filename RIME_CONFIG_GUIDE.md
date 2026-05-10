# VoCoType Rime 配置指南

当前仓库只支持 **Fcitx 5**，因此 Rime 配置也以 `fcitx5-rime` 生态为准。

## 配置目录

VoCoType 使用 Fcitx 5 的标准 Rime 用户目录：

```text
~/.local/share/fcitx5/rime/
```

如果你已经在用 `fcitx5-rime`，VoCoType 会直接复用这套配置和词库。

## 安装脚本记录的方案选择

安装脚本会把当前选中的 schema 记录到：

```text
~/.config/vocotype/rime/user.yaml
```

这个文件只记录 VoCoType 安装阶段选中的方案，真正的 Rime 数据和部署结果仍在 `~/.local/share/fcitx5/rime/`。

## 推荐方案

推荐使用 [rime-ice](https://github.com/iDvel/rime-ice)。

优点：

- 词库更新积极
- 全拼 / 双拼体验更完整
- 配置资料多

## 安装 rime-ice

先备份现有配置：

```bash
cp -r ~/.local/share/fcitx5/rime ~/.local/share/fcitx5/rime.backup
```

再安装：

```bash
git clone https://github.com/iDvel/rime-ice.git /tmp/rime-ice
cp -r /tmp/rime-ice/* ~/.local/share/fcitx5/rime/
```

安装后重启 Fcitx 5，或在 Rime 菜单里重新部署。

## 常用自定义

### 修改候选词数量

`default.custom.yaml`:

```yaml
patch:
  "menu/page_size": 9
```

### 指定输入方案

例如在 `default.custom.yaml` 中调整 schema 列表，或直接在 Fcitx 5 的 Rime 菜单里切换。

### 添加自定义词库

在具体 schema 的 `.custom.yaml` 中追加 `translator/dictionary` 配置即可。

## 配置不生效怎么办

先清理 build 目录：

```bash
rm -rf ~/.local/share/fcitx5/rime/build/
```

然后重启：

```bash
fcitx5 -r
```

## 常见问题

### VoCoType 会覆盖我原来的 fcitx5-rime 配置吗

不会主动覆盖。安装脚本只会读取可用 schema，并把选择结果写入 `~/.config/vocotype/rime/user.yaml`。

### 没有拼音候选，只有语音输入

通常是下面几种情况：

1. `fcitx5-rime` 还没部署
2. `pyrime` 没装好
3. `~/.local/share/fcitx5/rime/` 缺少基础数据

### 重新部署 Rime 的推荐方式

- 先重启 `fcitx5`
- 或使用 Rime 菜单中的重新部署

## 参考资料

- https://rime.im/docs/
- https://github.com/rime/home/wiki/UserGuide
- https://github.com/iDvel/rime-ice
