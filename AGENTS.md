# QuickPhrase 开发约定

修改前阅读 `docs/DEVELOPMENT.md`；当前重构与验证记录见 `docs/3.5.0-release.md`。

- C11 + 原生 Win32，保留旧 JSON 和规则优先级；核心算法与 UI 分离。
- 已有工作区修改和本地资料属于用户，不回退、覆盖或删除无关内容。
- 修复先复现并增加回归；检查内存所有权、通知重入、状态回滚与保存失败。
- 使用 `make test`、`make sanitize` 和 `make setup` 验证。共享固定临时夹具的测试套件不要同时运行。
- 布局使用配对的私有队列接口；预览显示与复制须映射同一份原文。
- 不把模拟 API 检查称为真实 Windows 验收。发布报告必须说明跳过项和平台限制。
- 版本源头是 `src/platform/version.h`，同步 manifest。图标母图已入库，普通构建不重新生成图像。
