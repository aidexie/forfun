# Claude Code 最佳实践与工作流优化

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 建立一套系统化的 Claude Code 使用规范，最大化 AI 辅助编程效率，同时保持代码质量和架构控制。

**Architecture:** 分为两部分：(1) 工作流程指南 - 软性规范，指导如何与 Claude Code 协作；(2) 技术实现 - 硬性工具，通过自动化强制执行最佳实践。

**Tech Stack:** Git hooks, PowerShell scripts, Markdown templates

---

## Part 1: 工作流程指南（软性规范）

### 核心原则

#### 原则 1: Context 是稀缺资源

```
Context 质量 × Context 相关性 = Claude 输出质量
```

**实践要点：**
- 每个对话专注单一主题
- 完成一个逻辑单元后 `/clear`
- CLAUDE.md 只保留高频使用的规则，低频内容放 docs/

#### 原则 2: 人类掌控架构，AI 负责实现

```
❌ "让渲染更快"
✅ "在 GBufferPass 中添加 early-z pre-pass，使用 depth-only PSO"
```

**决策边界：**
| 人类决策 | AI 执行 |
|---------|--------|
| 选择算法/数据结构 | 实现算法细节 |
| 定义 API 接口 | 填充函数体 |
| 设计模块边界 | 编写模块内代码 |
| 确定测试策略 | 编写具体测试用例 |

#### 原则 3: 小步快跑，频繁检查点

```
任务粒度标准：
- 可在 5-15 分钟内完成
- 改动不超过 3 个文件
- 有明确的验证方式（编译通过/测试通过/视觉检查）
```

**工作节奏：**
```
[定义任务] → [AI 实现] → [Review diff] → [运行测试] → [Commit] → [下一个任务]
     ↑                                                              |
     └──────────────────────────────────────────────────────────────┘
```

---

### 标准工作流程

#### 流程 A: 新功能开发

```
1. 需求分析（人类主导）
   - 明确功能边界
   - 确定技术方案
   - 定义验收标准

2. 计划制定（协作）
   - 使用 superpowers:brainstorming 讨论方案
   - 使用 superpowers:writing-plans 生成实现计划
   - 人类 review 并调整计划

3. 实现（AI 主导，人类监督）
   - 按计划逐步执行
   - 每个任务后 review diff
   - 发现问题立即修正

4. 验收（人类主导）
   - 运行完整测试套件
   - 视觉/功能验证
   - 代码 review
```

#### 流程 B: Bug 修复

```
1. 复现 Bug（人类）
   - 记录复现步骤
   - 收集错误日志

2. 定位问题（协作）
   - 提供相关 context（日志、堆栈、相关代码）
   - AI 分析可能原因
   - 人类确认根因

3. 修复（AI）
   - 先写失败测试
   - 实现修复
   - 验证测试通过

4. 回归测试（自动化）
   - 运行相关测试套件
```

#### 流程 C: 重构

```
1. 定义重构范围（人类）
   - 明确要改什么，不改什么
   - 定义成功标准（行为不变）

2. 建立安全网（协作）
   - 确保测试覆盖率足够
   - 必要时先补充测试

3. 小步重构（AI，人类频繁 review）
   - 每次只做一种类型的改动
   - 每步都要编译通过、测试通过
   - 频繁 commit

4. 验证（自动化 + 人类）
   - 完整测试套件
   - 性能对比（如适用）
```

---

### 任务描述模板

#### 模板 1: 功能实现

```markdown
## 任务：[功能名称]

### 背景
[为什么需要这个功能，解决什么问题]

### 技术方案
- 算法/数据结构：[具体选择]
- 涉及文件：[列出主要文件]
- 依赖：[前置条件]

### 验收标准
- [ ] 编译通过
- [ ] 测试 TestXXX 通过
- [ ] [其他具体标准]

### 约束
- 不要修改 [XXX] 的接口
- 保持与 [YYY] 的兼容性
```

#### 模板 2: Bug 修复

```markdown
## Bug：[简短描述]

### 复现步骤
1. [步骤1]
2. [步骤2]
3. [观察到的错误行为]

### 期望行为
[正确的行为应该是什么]

### 相关信息
- 日志：[关键日志行]
- 堆栈：[如有]
- 相关代码：[文件:行号]

### 验收标准
- [ ] Bug 不再复现
- [ ] 添加回归测试
- [ ] 现有测试全部通过
```

---

### Review 检查清单

每次 AI 完成任务后，执行以下检查：

```markdown
## Quick Review Checklist

### 1. Diff 检查（必做）
```bash
git diff --stat        # 改动范围是否合理
git diff <file>        # 具体改动是否正确
```

### 2. 编译检查（必做）
```bash
cmake --build build --target forfun
```

### 3. 测试检查（必做）
```bash
./build/Debug/forfun.exe --test TestXXX
# 检查 debug/{TestName}/runtime.log
```

### 4. 架构检查（重要改动时）
- [ ] 是否引入了不必要的复杂度？
- [ ] 是否违反了现有的设计模式？
- [ ] 是否有更简单的实现方式？

### 5. 安全检查（涉及输入处理时）
- [ ] 是否有潜在的注入风险？
- [ ] 边界条件是否处理？
```

---

## Part 2: 技术实现（硬性工具）

### Task 1: 创建任务模板目录

**Files:**
- Create: `docs/templates/task-feature.md`
- Create: `docs/templates/task-bugfix.md`
- Create: `docs/templates/task-refactor.md`

**Step 1: 创建模板目录结构**

```bash
mkdir -p docs/templates
```

**Step 2: 创建功能实现模板**

创建 `docs/templates/task-feature.md`：

```markdown
# 功能：[名称]

## 背景
[问题描述]

## 技术方案
- 算法：
- 文件：
- 依赖：

## 验收标准
- [ ] 编译通过
- [ ] 测试通过
- [ ]

## 约束
-
```

**Step 3: 创建 Bug 修复模板**

创建 `docs/templates/task-bugfix.md`：

```markdown
# Bug：[描述]

## 复现
1.
2.
3.

## 期望行为


## 相关信息
- 日志：
- 代码：

## 验收标准
- [ ] Bug 修复
- [ ] 回归测试
- [ ] 现有测试通过
```

**Step 4: 创建重构模板**

创建 `docs/templates/task-refactor.md`：

```markdown
# 重构：[描述]

## 范围
- 改：
- 不改：

## 成功标准
- 行为不变
- 测试全过

## 步骤
1.
2.
3.
```

**Step 5: Commit**

```bash
git add docs/templates/
git commit -m "docs: add task templates for Claude Code workflow"
```

---

### Task 2: 创建 Review 辅助脚本

**Files:**
- Create: `scripts/review.ps1`

**Step 1: 创建脚本目录**

```bash
mkdir -p scripts
```

**Step 2: 创建 review 脚本**

创建 `scripts/review.ps1`：

```powershell
# Claude Code Review Helper
# Usage: .\scripts\review.ps1 [TestName]

param(
    [string]$TestName = ""
)

Write-Host "=== Claude Code Review Checklist ===" -ForegroundColor Cyan

# 1. Show diff stats
Write-Host "`n[1/4] Diff Statistics:" -ForegroundColor Yellow
git diff --stat

# 2. Build check
Write-Host "`n[2/4] Building..." -ForegroundColor Yellow
cmake --build build --target forfun 2>&1 | Select-Object -Last 5

if ($LASTEXITCODE -eq 0) {
    Write-Host "Build: PASSED" -ForegroundColor Green
} else {
    Write-Host "Build: FAILED" -ForegroundColor Red
    exit 1
}

# 3. Run test if specified
if ($TestName -ne "") {
    Write-Host "`n[3/4] Running Test: $TestName" -ForegroundColor Yellow
    $timeout = 30
    $process = Start-Process -FilePath ".\build\Debug\forfun.exe" -ArgumentList "--test $TestName" -PassThru -NoNewWindow
    $process | Wait-Process -Timeout $timeout -ErrorAction SilentlyContinue

    if ($process.HasExited) {
        $logPath = "E:\forfun\debug\$TestName\runtime.log"
        if (Test-Path $logPath) {
            Write-Host "`nTest Log (last 20 lines):" -ForegroundColor Yellow
            Get-Content $logPath -Tail 20
        }
    } else {
        Write-Host "Test timed out after $timeout seconds" -ForegroundColor Red
        $process | Stop-Process -Force
    }
} else {
    Write-Host "`n[3/4] No test specified, skipping..." -ForegroundColor Gray
}

# 4. Prompt for manual review
Write-Host "`n[4/4] Manual Review Checklist:" -ForegroundColor Yellow
Write-Host "  [ ] Diff changes are correct"
Write-Host "  [ ] No unnecessary complexity"
Write-Host "  [ ] No security issues"
Write-Host "  [ ] Ready to commit?"

Write-Host "`n=== Review Complete ===" -ForegroundColor Cyan
```

**Step 3: Commit**

```bash
git add scripts/review.ps1
git commit -m "scripts: add review helper for Claude Code workflow"
```

---

### Task 3: 更新 CLAUDE.md 添加工作流引用

**Files:**
- Modify: `CLAUDE.md`

**Step 1: 在 CLAUDE.md 末尾添加工作流引用**

在 `## Documentation Index` 部分添加：

```markdown
### Workflow & Templates (docs/)
- `docs/plans/2026-01-29-claude-code-best-practices.md` - Claude Code 使用最佳实践
- `docs/templates/task-feature.md` - 功能开发任务模板
- `docs/templates/task-bugfix.md` - Bug 修复任务模板
- `docs/templates/task-refactor.md` - 重构任务模板

### Scripts (scripts/)
- `scripts/review.ps1` - Review 辅助脚本
```

**Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: add workflow references to CLAUDE.md"
```

---

### Task 4: 创建 Context 清理提醒（可选）

**Files:**
- Modify: `CLAUDE.md`

**Step 1: 在 CLAUDE.md 的 Core Working Principles 部分添加**

```markdown
### **TOP 4: Context Hygiene**

**定期清理 context 以保持 AI 性能：**

- 完成一个独立功能后 → `/clear`
- 对话超过 20 轮后 → 考虑 `/clear`
- 切换到不相关任务时 → `/clear`
- 发现 AI 输出质量下降时 → `/clear`
```

**Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: add context hygiene principle to CLAUDE.md"
```

---

## 执行顺序总结

| Task | 描述 | 依赖 |
|------|------|------|
| 1 | 创建任务模板 | 无 |
| 2 | 创建 Review 脚本 | 无 |
| 3 | 更新 CLAUDE.md 引用 | Task 1, 2 |
| 4 | 添加 Context 清理原则 | 无 |

---

**Plan complete and saved to `docs/plans/2026-01-29-claude-code-best-practices.md`.**

**Two execution options:**

1. **Subagent-Driven (this session)** - I dispatch fresh subagent per task, review between tasks, fast iteration

2. **Parallel Session (separate)** - Open new session with executing-plans, batch execution with checkpoints

**Which approach?**
