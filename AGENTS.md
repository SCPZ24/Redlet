当前仓库是我的简历项目。
目标是用C++实现一个小的Redis。

当我要求你帮我设计实现方案，或者要求你设计测试方案时，你需要你告诉我为什么这样设计、这样想，有没有别的方案，如何做出的取舍。

## 代码运行环境

需要编译、运行代码、执行测试或性能实验时，使用当前已启动的 Redlet Linux 实验容器。环境配置位于 `.devcontainer/devcontainer.json` 和 `.devcontainer/Dockerfile`，基于 Ubuntu 24.04，仓库挂载到容器内的 `/workspace`。

- 执行 `docker ps` 查找当前运行的 Redlet 开发容器，可通过 `vsc-redlet-` 镜像前缀识别。若有多个候选，使用 `docker inspect <容器ID或名称>` 确认当前仓库挂载到 `/workspace`。
- 使用 `docker exec -w /workspace <容器ID或名称> bash -lc '<命令>'` 执行编译、运行和测试命令。
- 容器重建后名称和 ID 可能变化，执行前以 `docker ps` 为准。