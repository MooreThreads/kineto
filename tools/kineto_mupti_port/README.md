# Kineto CUDA/CUPTI → MUSA/MUPTI 整树原地迁移

本工具让 MUSA/MUPTI **直接取代** Kineto 中 CUDA/CUPTI 的生态位。它不会保留 CUDA backend，也不会增加并行的 `HAS_MUPTI` patch 分支。

迁移分两阶段：

1. 对完整 `libkineto` 源码树执行最长 key 优先的机械替换与文件重命名；
2. 应用已经在 MUSA 5.1/MUPTI 5.1 上通过编译和 trace smoke test 的兼容修正。

> **这是原地、破坏性的源码迁移。只应在干净 Git 分支或可恢复副本上执行。**

## 迁移结果

- 公共层原有 `HAS_CUPTI`、CUPTI factory 和 CUDA backend 直接改成 `HAS_MUPTI`、MUPTI factory 和 MUSA backend；不生成 patch。
- `Cupti*`、`cupti_strings*` 原地重命名为对应的 `Mupti*`、`mupti_strings*`；旧文件不保留。
- `DeviceUtil*` 和 `DeviceProperties*` 是中性公共文件名，文件名保持不变；只把文件内部的 CUDA/CUPTI 实现替换为 MUSA/MUPTI。
- CUDA/CUPTI runtime、driver、callback、版本宏、构建项和 API 直接映射到 MUSA/MUPTI。
- `ActivityType` 保留 Kineto 已序列化的 CUDA ABI 槽位和名称：`CUDA_RUNTIME=7`、`CUDA_DRIVER=8`、`CUDA_SYNC=15`、`CUDA_EVENT=16`、`CUDA_PROFILER_RANGE=19`。
- MUSA runtime/driver 使用 PrivateUse1 槽位，并在枚举末尾增加 MUSA 专用 activity：

  ```text
  PRIVATEUSE1_RUNTIME = 24
  PRIVATEUSE1_DRIVER  = 25
  MUSA_PROFILER_RANGE = 29
  MUSA_SYNC           = 30
  MUSA_EVENT          = 31
  ENUM_COUNT          = 32
  ```

- 除 `ActivityType.h` 中为兼容序列化 ABI 而保留的 CUDA activity 外，其他 CUDA 命名直接替换为 MUSA 命名，包括 sync/event/profiler-range；目标树不保留其他 CUDA/CUPTI token。

## 用法

脚本接受 Kineto 仓库根、`libkineto/` 或 `libkineto/src/` 所在项目的 `libkineto` 根。

先预览完整迁移计划，不写盘：

```bash
python3 tools/kineto_mupti_port/port_cupti_to_mupti.py \
  --src /path/to/kineto \
  --dry-run
```

确认源树可恢复后执行原地迁移：

```bash
python3 tools/kineto_mupti_port/port_cupti_to_mupti.py \
  --src /path/to/kineto
```

脚本先计算完整内容与重命名计划并检查目标冲突，确认可执行后才写入。若对应的 `Mupti*`/`Musa*` 目标文件已存在且不是本次迁移输入，脚本拒绝覆盖。

## MUPTI 5.1 二次修复

暴力机械替换后，脚本继续处理目标 SDK 的确定差异：

- callback ID 的 API 前缀改为 `musa*` / `mu*`；
- activity record 使用目标 SDK 实际类型：`Kernel6`、`Memcpy4`、`MemcpyPtoP3`、`Memset3`、`MusaEvent`；
- 删除 MUPTI 5.1 不支持的 selective runtime/driver、event timestamp、per-thread buffer 和新版 overhead API/enum；
- `muptiActivitySetAttribute` 在目标头文件中有声明但动态库不导出，因此 device buffer size/pool setter 保持 no-op；
- 不使用不存在的 timestamp callback；activity timestamp 通过实时钟与单调钟偏移转换到 Unix epoch，duration 使用 `end - start`；
- occupancy 使用 MUSA 类型、函数和 `MUSA_OCC_SUCCESS`；
- 构建必须使用 `MusaUtil.cpp`/`MusaDeviceProperties.cpp`，防止 CUDA device utility 使 GPU availability 检测静默失败。

## 迁移后检查

脚本将未替换的 CUDA/CUPTI token 视为失败，成功执行后还应检查：

```bash
git -C /path/to/kineto diff --check
git -C /path/to/kineto status --short
```

确认：

1. 不存在 `Cupti*`、`cupti_strings*` 旧路径；`DeviceUtil*`、`DeviceProperties*` 保持中性文件名；
2. Controller 原 CUPTI factory 已直接变成 MUPTI factory，没有并行 backend patch；
3. CMake/Bazel 只引用 MUSA/MUPTI device sources 和 libraries；
4. 实际构建日志显示 MUPTI 初始化成功，而不是 CPU-only fallback；
5. smoke trace 包含 PrivateUse1 runtime、kernel、HtoD/DtoH memcpy，并且 runtime 与设备 activity correlation ID 一致。

脚本会在 `libkineto` 根写入 `MUSA_MUPTI_PORT_REPORT.md`，记录迁移文件数、重命名数和 MUPTI 5.1 compatibility pass。
