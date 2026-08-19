# 单相 STATCOM 4号工程三仓 GitHub 同步与目录重构交底书

日期：2026-08-19  
适用基线：DSP / CPLD / 上位机协议 `0x0105`  
执行对象：接手进行 Git 整理、提交和推送的 AI  

## 1. 任务边界

本交底书只规定后续 GitHub 整理与发布工作。本次编写交底书期间没有执行下列操作：

- 没有复制、移动或删除任何工程目录；
- 没有修改 DSP、CPLD 或上位机源码；
- 没有执行 `git init`、`git add`、`git commit`、`git tag` 或 `git push`；
- 没有创建或修改 GitHub 仓库、分支、Release；
- 没有解除 DSP 或 CPLD 的功率输出硬封锁。

接手 AI 必须先完成只读盘点，把“工程源目录”“本地 Git 仓库根目录”“远端 GitHub 地址”分开确认，再向用户报告计划。任何提交、推送、建分支、打标签或发布 Release 均需用户明确同意。

## 2. 三端兼容基线与不可突破的安全边界

三端必须作为同一套 `0x0105` 兼容基线记录：

| 分类 | 当前工程 | 关键状态 |
| --- | --- | --- |
| 4号 DSP 工程 | `F:\DSP\SST_PRJ\4_STATCOM_CURRENT_LOOP` | 20 kHz 原始码快速保护；PLL 4 kHz；锁相已能稳定锁定；功率输出封锁 |
| CPLD 工程 | `F:\Quartus_project\SST_prj\CPLD_EPM1270_STATCOM_MODBUS` | Modbus、Vdc 软件保护、本地温度保护；四路桥臂为 0，`pwm_hold_o=1` |
| 上位机工程 | `F:\DSP\SST_PRJ\4_STATCOM_CURRENT_LOOP\docs\statcom_host` | 与 `0x0105` 原始码协议配套；源码当前嵌在 DSP 工程文档目录内 |

强制安全要求：

1. `STATCOM_POWER_OUTPUT_ENABLED=0` 必须保持不变。
2. CPLD 四路桥臂输出必须保持常量 0，`pwm_hold_o` 必须保持 1。
3. START/STOP 只用于通信、状态机和联锁验收，不得借发布整理解除 PWM 封锁。
4. 不得把协议 `0x0105` 自动等同于 Git 语义化版本号。Git 标签名由用户另行确认。
5. 三仓的 README/兼容清单都必须明确写出：当前版本不能产生功率 PWM。

## 3. 分类一：4号 DSP 工程

### 3.1 已确认位置与 Git 状态

- 工程源目录：`F:\DSP\SST_PRJ\4_STATCOM_CURRENT_LOOP`
- 本地 Git 仓库根目录：`F:\DSP\SST_PRJ`
- 已确认远端：`git@github.com:rongsen123/DSP_PRJ.git`
- 当前分支：`feature/2-adc-sci-gpio-plecs`
- 4号工程当前在仓库状态中表现为未跟踪目录：`?? 4_STATCOM_CURRENT_LOOP/`
- 同一仓库中还存在 3号工程等其他既有未提交改动；这些改动不属于本次发布范围。

这是本次工作最重要的 Git 风险：严禁执行 `git add .` 或 `git add -A`。只能显式暂存确认过的 4号工程路径，以及经用户同意更新的仓库根目录索引文件。

### 3.2 DSP 仓库目标结构

保持现有“一个 CCS 工程一个文件夹”的结构，并更新根目录总工程列表：

```text
DSP_PRJ/
  README.md                       # 总工程列表，新增4号工程
  .gitignore
  1_SCI_GPIO/
  1_SCIB_GPIO/
  2_ADC_SCI_GPIO/
  3_STATCOM_PI_PR/
  4_STATCOM_CURRENT_LOOP/
    README.md
    CHANGELOG.md                  # 建议新增，记录4号工程版本演进
    VERSION                       # 版本内容须先由用户确认
    .ccsproject
    .cproject
    .project
    .settings/
    app/
    cmd/
    headers/
    include/
    lib/
    source/
    targetConfigs/
    docs/
```

仓库根 `README.md` 当前已有 1、2、3号工程和 PLECS 条目，执行者应增加 4号工程条目，并检查是否需要补充现存的 `1_SCIB_GPIO` 条目。不要顺手修改其他工程源码。

### 3.3 DSP 应纳入与排除内容

应纳入：C/C++ 源码、头文件、链接命令文件、必要库文件、CCS 工程配置、目标配置、寄存器表、README、交底与测试说明。

应排除：

```text
Debug/
Release/
*.out
*.hex
*.map
*.lst
*.obj
*.o
*.d
*.d_raw
.metadata/
.jxbrowser.userdata/
.tempFiles/
dvt/
RemoteSystemsTempFiles/
docs/statcom_host/build/
docs/statcom_host/dist/
**/__pycache__/
*.pyc
```

DSP 根 `.gitignore` 已覆盖主要 CCS 生成物；仍应使用 `git status --ignored` 复核。上位机源码将独立进入新仓库，但在用户确认独立仓库发布成功前，不要擅自从 DSP 工程中删除原副本。

### 3.4 DSP 发布记录必须包含

- 目标器件：TMS320F28062；CCS 12.3.0；C2000 编译器 22.6.0.LTS。
- ADC 硬件 20 kHz；电流/电网保护为整数原始码快速路径；SOGI-PLL 为固定 5:1 抽取后的 4 kHz。
- 协议版本 `0x0105`。
- 关键原始码寄存器：`0005` 电网正半波，`0006` Iac，`0007` Iac 零点，`0008/0009` CPLD Vdc 原始/平均值，`000A` 温度脉冲计数。
- 阈值寄存器：`1000` Vdc 原始码，`1001` 电网原始码，`1002` 电流偏差原始码。
- STOP 状态清故障会重新武装“曾锁定后失锁”监测；START 仍要求 PLL 有效且锁定。
- 用户已反馈保护项目完成试验、PLL 已可锁定；最近观测 `adc_overflow_count=0`、`adc_queue_overflow_count=0`，处理计数和 PLL 计数正常累加。
- 当前构建仍会出现 `IQmath.lib` 与 `rts2800_fpu32_fast_supplement_coff.lib` 未解析警告。构建虽成功，但不得声称快速数学库已经链接生效。
- 功率输出仍硬封锁，电流环、交流环和实际 PWM 尚未开放。

### 3.5 DSP 安全执行步骤

1. 关闭或停止正在写工程状态的 CCS 会话。
2. 在 `F:\DSP\SST_PRJ` 只读检查 `git status --short`、当前分支、上游分支和 `origin`。
3. 向用户列出本次拟提交的精确路径；明确排除 3号工程和其他既有脏改动。
4. 更新根 `README.md` 的 4号工程索引，并按用户确认的版本方案新增 4号工程 `CHANGELOG.md`/`VERSION`。
5. 编译 4号工程，记录工具版本、结果及现存警告。
6. 只显式暂存，例如 `git add -- 4_STATCOM_CURRENT_LOOP <用户确认的根目录索引文件>`；不得全量暂存。
7. 检查 `git diff --cached --name-status` 和 `git diff --cached`，确认没有 `Debug/`、`.out` 或其他工程改动。
8. 把拟提交文件清单、提交说明、分支、远端再次展示给用户；获准后才提交和推送。

建议提交说明仅供用户选择：`feat(ccs): 收录4号STATCOM原始码保护与稳定锁相基线`。

## 4. 分类二：CPLD 工程

### 4.1 已确认位置与当前阻塞项

- 当前配套工程源目录：`F:\Quartus_project\SST_prj\CPLD_EPM1270_STATCOM_MODBUS`
- `F:\Quartus_project\SST_prj` 当前不是 Git 仓库。
- `F:\Quartus_project\SST_prj\CPLD_1270_prj` 当前也未被 Git 识别为仓库，且其中混有构建目录、临时工作区和旧上位机内容，不能直接整体初始化后上传。
- 历史交底只写有“GitHub CPLD_PRJ，功能提交 `7701840`”，没有留下可由本机 Git 验证的准确远端 URL。

**阻塞项：执行任何 CPLD Git 写操作前，必须由用户确认 CPLD 仓库的准确 SSH 地址。不得把可能的 `git@github.com:rongsen123/CPLD_PRJ.git` 当成已确认事实。**

### 4.2 CPLD 仓库目标结构

建议在一个干净、独立的本地克隆目录中整理，且该目录不能嵌在 DSP 仓库中：

```text
CPLD_PRJ/
  README.md                       # 仓库说明
  PROJECT_INDEX.md               # 一个工程一行的总工程列表
  TRACKED_FILE_LIST.txt          # 可选：由 git ls-files 生成的总文件列表
  CHANGELOG.md
  .gitignore
  CPLD_1270_BASE/                # 原有基线工程，如远端已存在则保留原名
  CPLD_EPM1270_STATCOM_MODBUS/
    README.md
    CHANGELOG.md
    VERSION
    top.qpf
    top.qsf
    top.sdc
    rtl/
    tb/
    docs/
```

不要把现有 `F:\Quartus_project\SST_prj\CPLD_1270_prj` 的全部内容直接复制成一个工程。先克隆用户确认的远端，阅读已有结构和历史，再将当前配套工程作为独立文件夹增量加入，保留远端已有工程目录。

`PROJECT_INDEX.md` 至少记录：目录名、目标器件、用途、配套 DSP 工程、协议版本、Quartus 版本、构建状态、最新提交/标签。`TRACKED_FILE_LIST.txt` 如需提交，应在最终暂存内容确认后由 `git ls-files` 生成，作为精确总文件清单。

### 4.3 CPLD 应纳入与排除内容

当前工程建议纳入：

```text
README.md
top.qpf
top.qsf
top.sdc
rtl/*.v
tb/tb_modbus_rtu_slave_fixed8.v
docs/MODBUS_RTU_REGISTER_MAP.md
```

建议排除：

```text
db/
incremental_db/
output_files/
simulation/
work/
*.qws
*.rpt
*.summary
*.smsg
*.pof
*.sof
*.jdi
*.done
*.pin
*.vo
*.sdo
```

如现有远端对某些 Quartus 文件已有不同规则，应先解释差异并让用户确认，不要为了统一而删除历史文件。

### 4.4 CPLD 发布记录必须包含

- 器件 MAX II `EPM1270T144C5`，30 MHz，Quartus 13.1。
- Modbus RTU 115200 bit/s、8N1，从站地址 1，支持 FC03/FC04/FC06。
- Vdc 软件过压为 CPLD 本地连续 3 个 ADC 样本确认；硬件过压链路保持独立。
- 温度阈值固定为 `5000 count/100 ms`，其含义是 100 ms 窗口内的脉冲计数，等效门限频率约 50 kHz；不是“5000 Hz”。方向与最终阈值仍需实物标定，且当前不开放上位机下发。
- 四路桥臂输出为 0，`pwm_hold_o=1`；START/STOP 不产生 PWM。
- 已有一次 Quartus 13.1 全编译记录：0 errors；1044/1270 LE（82%）；setup slack 7.406 ns；hold slack 1.078 ns。发布前仍应从干净源码重新编译确认。

### 4.5 CPLD 安全执行步骤

1. 先向用户索取并确认 CPLD 远端 SSH 地址。
2. 选择一个不在 DSP 仓库内部的全新本地目录，克隆远端；禁止在混杂目录中盲目 `git init`。
3. 检查远端默认分支、已有工程目录、标签、README 和 `.gitignore`。
4. 复制当前 `CPLD_EPM1270_STATCOM_MODBUS` 的必要源码到同名独立子目录，不移动、不删除原工程。
5. 新建/更新根 `PROJECT_INDEX.md`，必要时生成 `TRACKED_FILE_LIST.txt`。
6. 从整理后的源码执行 Quartus 全编译和时序检查，确认输出硬封锁常量未变。
7. 只显式暂存新增工程和索引文件，检查暂存清单与大文件。
8. 向用户展示远端、分支、文件清单、编译结果和提交说明；获准后才提交、打标签或推送。

建议提交说明仅供用户选择：`feat(fpga-quartus): 收录STATCOM Modbus与本地保护工程`。

## 5. 分类三：上位机工程

### 5.1 已确认位置与远端

- 当前源码目录：`F:\DSP\SST_PRJ\4_STATCOM_CURRENT_LOOP\docs\statcom_host`
- 当前源码不是独立仓库，而是位于 DSP 仓库内部。
- 用户指定的新远端：`git@github.com:rongsen123/host_app.git`

不得在上述源码目录内执行 `git init`，也不得在 `F:\DSP\SST_PRJ` 内克隆新仓库，否则会形成嵌套 Git 仓库。应选择 DSP 仓库以外的干净目录克隆 `host_app`，再复制必要源码。

### 5.2 上位机仓库目标结构

```text
host_app/
  README.md
  PROJECT_INDEX.md
  TRACKED_FILE_LIST.txt          # 可选：由 git ls-files 生成
  CHANGELOG.md
  .gitignore
  STATCOM_HOST_V0105/
    README.md
    CHANGELOG.md
    VERSION
    main.py
    requirements.txt
    STATCOM上位机.spec
    communication/
    protocol/
    storage/
    ui/
    tests/
    docs/
```

`STATCOM_HOST_V0105` 是建议的清晰目录名；执行者在复制前应把该命名和版本号展示给用户确认。以后不同设备或不兼容协议的上位机放在新的并列文件夹中，不覆盖旧版本。

根 `PROJECT_INDEX.md` 至少记录：文件夹、适配设备、协议版本、配套 DSP/CPLD 工程、Python/PySide6 要求、构建方式、发布日期、最新提交/标签。每个子工程独立维护 README、CHANGELOG 和 VERSION。

### 5.3 上位机应纳入与排除内容

当前建议纳入：

```text
main.py
requirements.txt
STATCOM上位机.spec
communication/*.py
protocol/*.py
storage/*.py
ui/*.py
tests/test_protocol.py
必要交底文档
```

建议排除：

```text
build/
dist/
data_logs/
__pycache__/
*.pyc
.pytest_cache/
.venv/
venv/
*.log
运行产生的 *.csv
```

`.spec` 文件只有在其路径配置可移植、确实用于重复打包时才纳入；若包含本机绝对路径，应先修正或说明，不能直接提交机器专用路径。

### 5.4 上位机发布记录必须包含

- 配套协议 `0x0105`、DSP 4号工程和 `CPLD_EPM1270_STATCOM_MODBUS`。
- 29 个输入寄存器周期监控和 FC03/FC06 阈值读写。
- `0007` 为交流电流 ADC 零点原始码；显示电流由上位机按标定系数换算，DSP 实时路径不做 V/A 换算。
- `1000`、`1001`、`1002` 下发的是 DSP/CPLD 使用的原始码阈值；界面可以输入工程量，但必须在上位机内部换算为原始码后下发。
- 温度保护为 CPLD 固定 `5000 count/100 ms`，当前不是上位机可写阈值。
- 通信诊断、START/STOP、清故障和复位功能的兼容关系。
- 功率输出硬封锁，界面 START 不等于实际 PWM 使能。

### 5.5 上位机安全执行步骤

1. 在 DSP 仓库外选择全新目录，克隆 `git@github.com:rongsen123/host_app.git`。
2. 只读检查远端是否为空、默认分支、已有目录与历史；不得强推覆盖。
3. 经用户确认目录名后，将源码复制到 `STATCOM_HOST_V0105/`，不删除 DSP 工程内原副本。
4. 新增根 README、PROJECT_INDEX、CHANGELOG 和 `.gitignore`，建立子工程 VERSION/CHANGELOG。
5. 在干净 Python 环境安装 `requirements.txt`，运行协议测试；当前测试入口为 `tests/test_protocol.py`。执行者必须记录实际命令和实际结果，不得沿用旧文档中的“已通过”结论代替本次验证。
6. 检查 PyInstaller 配置；如需要发布 EXE，执行打包和最小启动验证。
7. 只显式暂存源码、测试、文档和索引；检查不得包含 build、dist、日志、CSV、缓存或串口设备信息。
8. 向用户展示远端、分支、目录结构、测试结果、暂存清单和提交说明；获准后才提交和推送。

建议提交说明仅供用户选择：`feat(host): 发布STATCOM V0105监控上位机`。

## 6. 三仓统一版本记录方案

不要擅自选择标签。先让用户确认发布名称，例如“STATCOM 4号联调基线”，再确认三个仓库各自的 Git 标签。协议号 `0x0105` 可作为兼容字段，但不自动决定标签。

建议每个仓库都增加一份兼容清单，例如：

```text
docs/releases/STATCOM_V0105_COMPATIBILITY.md
```

内容必须相互交叉记录：

| 字段 | 内容 |
| --- | --- |
| 发布名称 | 用户确认后填写 |
| 协议 | `0x0105` |
| DSP | 仓库、分支、提交 SHA、工程目录 |
| CPLD | 仓库、分支、提交 SHA、工程目录 |
| 上位机 | 仓库、分支、提交 SHA、工程目录 |
| 工具版本 | CCS/C2000 编译器、Quartus、Python/PySide6/PyInstaller |
| 安全边界 | DSP/CPLD 功率输出硬封锁 |
| 已验证项目 | 编译、协议测试、上板锁相、故障保护 |
| 未完成项目 | 电流环/交流环/PWM 开放、最终标定等 |
| 发布附件 | 文件名、大小、SHA-256 |

完成三个源码提交后，先把三个提交 SHA 回填到兼容清单，再提交兼容清单。若这会形成交叉提交，应采用两阶段记录，或在 GitHub Release 说明中填写最终三个 SHA。

## 7. 发布二进制与当前校验值

生成物不进入源码提交。如用户要求保存可直接下载的固件，使用对应仓库 GitHub Release 附件，并记录源提交、构建工具、时间、文件大小和 SHA-256。

当前可核对的产物如下；发布前应在整理后的提交上重新构建并重新计算校验值，不能默认沿用：

| 分类 | 当前文件 | 大小 | 时间 | SHA-256 |
| --- | --- | ---: | --- | --- |
| DSP | `F:\DSP\SST_PRJ\4_STATCOM_CURRENT_LOOP\Debug\4_STATCOM_CURRENT_LOOP.out` | 298145 bytes | 2026-08-18 18:43:35 | `4D96865685640488C7A9A40EA477429CFC605D18D3644A9B3631801330098DB1` |
| CPLD | `F:\Quartus_project\SST_prj\CPLD_EPM1270_STATCOM_MODBUS\output_files\top.pof` | 27322 bytes | 2026-08-18 17:53:52 | `A07BBC68C0E226ABCE7C91454FF462BF18D5123ACEB472F112CB5ACF59EEA36B` |
| 上位机 | 当前未指定唯一发布 EXE | — | — | 发布打包后计算 |

## 8. 严禁事项

- 禁止 `git add .`、`git add -A`、强制推送、重写远端历史。
- 禁止把现有混杂 CPLD 工作目录整体初始化并上传。
- 禁止在 DSP 仓库内部建立上位机嵌套仓库。
- 禁止删除、移动或覆盖三个现有工程源目录。
- 禁止提交密码、SSH 私钥、串口设备隐私、本机缓存和绝对路径凭据。
- 禁止把编译缓存、Debug/Release、Quartus db/output、Python build/dist 当源码提交。
- 禁止仅凭旧交底书声称本次测试通过；必须记录本次实际命令和结果。
- 禁止把协议号直接当作未经用户确认的 Git 标签。
- 禁止解除任何功率输出封锁。

## 9. 接手 AI 的首次回复格式

接手 AI 在执行写操作前，应按以下三类向用户报告：

### 4号 DSP 工程

- 已确认的工程目录、仓库根、远端、分支；
- 本次精确拟提交路径；
- 如何避开 3号工程和其他既有脏改动；
- 构建与版本记录方案。

### CPLD 工程

- 当前工程源目录；
- 待用户确认的准确远端 SSH 地址；
- 干净克隆目录和“一工程一文件夹”目标结构；
- 根 PROJECT_INDEX/总文件列表方案；
- 编译、时序、硬封锁复核方案。

### 上位机

- 新远端 `git@github.com:rongsen123/host_app.git`；
- DSP 仓库外的干净克隆位置；
- 拟采用的子目录名和版本记录文件；
- 测试、打包、Release 方案；
- 明确不修改上位机业务功能，除非用户另行授权。

用户确认以上计划后，接手 AI 才可分阶段执行。每完成一类，先报告该类的暂存文件清单、验证结果和拟提交信息，再请求下一步授权，不得一次性静默推完三个仓库。

## 10. 完成判据

只有同时满足以下条件，才可向用户报告三仓同步完成：

1. 三个远端地址均经用户确认，且推送目标和分支明确。
2. DSP 仓库仅纳入 4号工程及确认过的索引/版本文件，没有夹带既有脏改动。
3. CPLD 仓库实现一个工程一个文件夹，并有根 PROJECT_INDEX；准确远端不再是未知项。
4. 上位机仓库实现一个上位机一个文件夹，并有 README、PROJECT_INDEX、CHANGELOG、VERSION。
5. 三端兼容清单记录协议 `0x0105` 和三个最终提交 SHA。
6. DSP、CPLD、上位机分别完成实际构建/测试并保存结果。
7. 源码仓库不含生成物；如发布二进制，Release 附件包含 SHA-256 和工具版本。
8. 三端 README 均明确功率输出仍处于硬封锁状态。

