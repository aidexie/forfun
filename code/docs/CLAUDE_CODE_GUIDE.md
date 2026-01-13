# Claude Code Guide

Comprehensive guide to Claude Code features, extensibility, and how agents work.

---

## Table of Contents

1. [Feature Overview](#feature-overview)
2. [Plugins](#plugins)
3. [Agents](#agents)
4. [Hooks](#hooks)
5. [MCP Servers](#mcp-servers)
6. [Skills (Slash Commands)](#skills-slash-commands)
7. [Memory (CLAUDE.md)](#memory-claudemd)
8. [Settings](#settings)
9. [How Agents Work](#how-agents-work)
10. [Why Agents Are Effective](#why-agents-are-effective)

---

## Feature Overview

Claude Code provides multiple extensibility mechanisms at the same level:

| Feature | Purpose | Trigger |
|---------|---------|---------|
| **Plugins** | Add pre-built commands | Manual (`/cmd`) |
| **Agents** | Autonomous multi-step tasks | Automatic/Task tool |
| **Hooks** | React to events | Automatic on events |
| **MCP Servers** | Add custom tools | Available to Claude |
| **Skills** | Invoke workflows | Manual (`/cmd`) |
| **Memory** | Persistent instructions | Always active |
| **Settings** | Configure behavior | Always active |

### Architecture

```
┌─────────────────────────────────────────┐
│            Claude Code CLI              │
├─────────────────────────────────────────┤
│  Settings & Memory (CLAUDE.md)          │
├──────────┬──────────┬──────────┬────────┤
│ Plugins  │  Agents  │  Hooks   │  MCP   │
│ (skills) │ (Task)   │ (events) │(tools) │
└──────────┴──────────┴──────────┴────────┘
```

---

## Plugins

Plugins are **pre-built extensions** that add specific functionalities to Claude Code.

### Configuration

Plugins are configured in `~/.claude/settings.json`:

```json
{
  "enabledPlugins": {
    "code-review@claude-plugins-official": true,
    "commit-commands@claude-plugins-official": true,
    "code-simplifier@claude-plugins-official": true
  }
}
```

### Plugin Management Commands

```bash
# Add a marketplace
/plugin marketplace add https://github.com/anthropics/claude-plugins-official

# Update marketplace
/plugin marketplace update claude-plugins-official

# List available plugins
/plugin list

# Install a plugin
/plugin install code-simplifier@claude-plugins-official

# Uninstall a plugin
/plugin uninstall code-simplifier@claude-plugins-official
```

### Available Official Plugins

#### LSP (Language Servers)

| Plugin | Description |
|--------|-------------|
| `typescript-lsp` | TypeScript/JavaScript |
| `pyright-lsp` | Python |
| `gopls-lsp` | Go |
| `rust-analyzer-lsp` | Rust |
| `clangd-lsp` | C/C++ |
| `php-lsp` | PHP |
| `swift-lsp` | Swift |
| `kotlin-lsp` | Kotlin |
| `csharp-lsp` | C# |
| `jdtls-lsp` | Java |
| `lua-lsp` | Lua |

#### Productivity

| Plugin | Description |
|--------|-------------|
| `code-simplifier` | Simplifies code for clarity |
| `code-review` | Automated PR review |
| `commit-commands` | Git commit workflows |
| `pr-review-toolkit` | PR review agents |
| `hookify` | Create custom hooks |

#### Development

| Plugin | Description |
|--------|-------------|
| `feature-dev` | Feature development workflow |
| `frontend-design` | Frontend design generation |
| `plugin-dev` | Plugin development toolkit |
| `ralph-loop` | Iterative development loops |
| `agent-sdk-dev` | Agent SDK tools |

#### External Integrations

| Plugin | Description |
|--------|-------------|
| `github` | GitHub integration |
| `gitlab` | GitLab integration |
| `playwright` | Browser automation |
| `supabase` | Supabase database |
| `firebase` | Firebase |
| `figma` | Figma design |
| `slack` | Slack messaging |
| `linear` | Linear issue tracking |
| `sentry` | Error monitoring |
| `vercel` | Vercel deployment |
| `stripe` | Stripe payments |

### Using Plugins

After installation and restart:

```bash
/commit              # Create a git commit
/code-review 123     # Review PR #123
/code-simplifier     # Simplify your code
```

---

## Agents

Agents are **autonomous AI entities** capable of complex task execution.

### Characteristics

- **AI-Driven Decision Making**: Use AI models to understand tasks, break them down, and decide actions
- **Multi-step Execution**: Handle complex workflows across multiple operations
- **Tool Access**: Can use various tools (Read, Write, Bash, etc.)
- **Autonomous**: Make decisions about next steps without explicit instructions

### Built-in Agents

| Agent | Purpose |
|-------|---------|
| `Bash` | Command execution (git, npm, docker) |
| `Explore` | Fast codebase exploration |
| `Plan` | Architecture and implementation planning |
| `general-purpose` | Multi-step research tasks |

### How Agents Are Used

```
User: "Find all files that handle authentication"

Claude Code spawns an Explore agent that:
1. Searches for "auth" patterns
2. Reads relevant files
3. Follows import chains
4. Returns a comprehensive answer
```

### Agent SDK Example (TypeScript)

```typescript
import Anthropic from "@anthropic-ai/sdk";

const client = new Anthropic();

const response = await client.messages.create({
  model: "claude-sonnet-4-20250514",
  max_tokens: 4096,
  system: "You are a security audit agent. Analyze code for vulnerabilities.",
  tools: [
    {
      name: "read_file",
      description: "Read a file",
      input_schema: {
        type: "object",
        properties: { path: { type: "string" } }
      }
    }
  ],
  messages: [{ role: "user", content: "Audit src/ for SQL injection" }]
});
```

### Plugin vs Agent

| Aspect | Plugins | Agents |
|--------|---------|--------|
| **Nature** | Pre-defined commands/scripts | Autonomous AI entities |
| **Intelligence** | Executes pre-programmed logic | Uses AI to reason and adapt |
| **Complexity** | Single-purpose functions | Multi-step, complex workflows |
| **Autonomy** | Requires direct invocation | Operates autonomously |

---

## Hooks

Hooks are shell commands that execute in response to Claude Code events.

### Configuration

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Write",
        "hooks": [
          {
            "type": "command",
            "command": "echo 'About to write: $CLAUDE_FILE_PATH'"
          }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "Write",
        "hooks": [
          {
            "type": "command",
            "command": "npx prettier --write $CLAUDE_FILE_PATH"
          }
        ]
      }
    ],
    "Notification": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "notify-send 'Claude' '$CLAUDE_NOTIFICATION'"
          }
        ]
      }
    ]
  }
}
```

### Hook Events

| Event | Trigger |
|-------|---------|
| `PreToolUse` | Before a tool is executed |
| `PostToolUse` | After a tool completes |
| `Notification` | When Claude sends a notification |

### Example Use Case

```
User: "Create a new React component"
→ Claude writes file
→ PostToolUse hook triggers
→ Prettier auto-formats the file
```

---

## MCP Servers

MCP (Model Context Protocol) servers are external processes that provide additional tools to Claude.

### Configuration

```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/home/user/projects"]
    },
    "github": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": {
        "GITHUB_TOKEN": "ghp_xxxxxxxxxxxx"
      }
    },
    "postgres": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-postgres"],
      "env": {
        "DATABASE_URL": "postgresql://user:pass@localhost/db"
      }
    },
    "brave-search": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-brave-search"],
      "env": {
        "BRAVE_API_KEY": "your-api-key"
      }
    }
  }
}
```

### How MCP Works

```
User: "Query the database for all users created this week"
→ Claude uses postgres MCP server
→ Executes: SELECT * FROM users WHERE created_at > NOW() - INTERVAL '7 days'
→ Returns results

User: "Create a GitHub issue for this bug"
→ Claude uses github MCP server
→ Creates issue via GitHub API
```

---

## Skills (Slash Commands)

Skills are commands invoked with `/command-name`.

### Built-in Skills

```bash
/help                 # Show help
/clear                # Clear conversation
/compact              # Summarize conversation to save context
/config               # Open settings
/cost                 # Show token usage
/doctor               # Diagnose issues
/init                 # Initialize CLAUDE.md
/review               # Review code changes
/vim                  # Toggle vim mode
```

### Plugin-Provided Skills

```bash
/commit               # AI-generated commit message
/commit-push-pr       # Full PR workflow
/code-review 456      # Review PR #456
```

### Example Workflow

```
> /commit
Claude: Analyzing staged changes...

Commit message:
"feat(auth): add JWT refresh token rotation

- Implement automatic token refresh before expiry
- Add refresh token blacklist for security
- Update auth middleware to handle rotation"

Proceed? [Y/n]
```

---

## Memory (CLAUDE.md)

Project-specific instructions that Claude follows automatically.

### Location

Place `CLAUDE.md` in your project root.

### Example

```markdown
# Project Instructions

## Build Commands
- Build: `npm run build`
- Test: `npm test`
- Lint: `npm run lint`

## Code Style
- Use TypeScript strict mode
- Prefer functional components with hooks
- Use Tailwind CSS for styling

## Architecture
- `/src/components` - React components
- `/src/hooks` - Custom hooks
- `/src/api` - API client functions

## Rules
- Always add tests for new features
- Run lint before committing
- Use conventional commit messages
```

### How It Works

```
User: "Add a new button component"
→ Claude reads CLAUDE.md automatically
→ Creates TypeScript component
→ Uses functional style with hooks
→ Adds Tailwind classes
→ Creates test file
```

---

## Settings

### Global Settings (`~/.claude/settings.json`)

```json
{
  "model": "claude-sonnet-4-20250514",
  "customInstructions": "Be concise. Prefer simple solutions.",
  "permissions": {
    "allow": [
      "Bash(npm run *)",
      "Bash(git *)",
      "Read",
      "Write(src/**)"
    ],
    "deny": [
      "Bash(rm -rf *)",
      "Write(.env*)"
    ]
  },
  "apiKeyHelper": "op read op://vault/anthropic/key",
  "theme": "dark"
}
```

### Project Settings (`.claude/settings.json`)

```json
{
  "customInstructions": "This is a Python project. Use pytest for testing.",
  "permissions": {
    "allow": [
      "Bash(pytest *)",
      "Bash(python *)"
    ]
  }
}
```

---

## How Agents Work

### The Basic Loop

```
┌─────────────────────────────────────────────────┐
│                   USER PROMPT                    │
└─────────────────────┬───────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────┐
│                   LLM THINKS                     │
│  "What do I need to do? What tool should I use?" │
└─────────────────────┬───────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────┐
│               TOOL CALL (Action)                 │
│        Read("file.cpp") / Bash("npm test")       │
└─────────────────────┬───────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────┐
│              TOOL RESULT (Observation)           │
│            File contents / Command output        │
└─────────────────────┬───────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────┐
│            LLM THINKS AGAIN                      │
│   "Based on this result, what's next?"          │
└─────────────────────┬───────────────────────────┘
                      ▼
              ┌───────┴───────┐
              │  Goal done?   │
              └───────┬───────┘
                 NO ↙     ↘ YES
                  ▼         ▼
            (loop back)   RESPOND
```

### Implementation (Pseudocode)

```python
def agent_loop(user_message, tools):
    messages = [{"role": "user", "content": user_message}]

    while True:
        # 1. Send to LLM
        response = llm.call(
            messages=messages,
            tools=tools  # Available tools
        )

        # 2. Check if LLM wants to use a tool
        if response.has_tool_call:
            tool_name = response.tool_call.name
            tool_args = response.tool_call.arguments

            # 3. Execute the tool
            result = execute_tool(tool_name, tool_args)

            # 4. Add result to conversation
            messages.append({"role": "assistant", "content": response})
            messages.append({"role": "tool", "content": result})

            # 5. Loop continues...
        else:
            # LLM is done, return final response
            return response.content
```

### Real Example: "Fix the bug in auth.js"

```
Turn 1:
├── User: "Fix the bug in auth.js"
├── LLM thinks: "I need to read auth.js first"
├── Tool call: Read("auth.js")
└── Result: [file contents with bug]

Turn 2:
├── LLM thinks: "I see the bug - missing null check on line 42"
├── Tool call: Edit("auth.js", old="user.name", new="user?.name")
└── Result: "File edited successfully"

Turn 3:
├── LLM thinks: "Should verify the fix works"
├── Tool call: Bash("npm test")
└── Result: "All tests passed"

Turn 4:
├── LLM thinks: "Bug fixed and verified"
└── Response: "Fixed the null reference bug on line 42..."
```

### Key Components

#### 1. Tool Definition

```json
{
  "name": "Read",
  "description": "Read a file from disk",
  "parameters": {
    "file_path": {
      "type": "string",
      "description": "Path to the file"
    }
  }
}
```

#### 2. LLM Tool Calling

The LLM outputs structured JSON:

```json
{
  "tool": "Read",
  "arguments": {
    "file_path": "E:/forfun/source/code/main.cpp"
  }
}
```

#### 3. Tool Executor

```python
def execute_tool(name, args):
    if name == "Read":
        return open(args["file_path"]).read()
    elif name == "Bash":
        return subprocess.run(args["command"])
    elif name == "Edit":
        # ... edit file
```

#### 4. Context Window

All previous messages stay in context:

```
[User message]
[Assistant tool call]
[Tool result]
[Assistant tool call]
[Tool result]
[Assistant tool call]
[Tool result]
[Final response]
```

### Sub-Agents (Parallelism)

Claude Code spawns **sub-agents** for complex tasks:

```
Main Agent
├── spawns → Explore Agent (search codebase)
├── spawns → Bash Agent (run commands)
└── spawns → Plan Agent (design architecture)

Each sub-agent:
- Has its own context
- Runs its own loop
- Returns result to main agent
```

### Memory Management

```
┌────────────────────────────────────┐
│         Context Window             │
├────────────────────────────────────┤
│ System prompt                      │
│ CLAUDE.md instructions             │
│ User message 1                     │
│ Tool call + result                 │
│ Tool call + result                 │
│ ... (summarized if too long) ...   │
│ Recent tool call + result          │
│ Current thinking                   │
└────────────────────────────────────┘
```

---

## Why Agents Are Effective

### Core Principle: Tool Use + Reasoning Loop

```
while (goal not achieved):
    1. Observe current state
    2. Reason about what to do next
    3. Choose and execute a tool/action
    4. Observe result
    5. Update understanding
```

### Key Principles

#### 1. Decomposition

Complex tasks → smaller, manageable steps

```
"Build a feature" becomes:
├── Search for relevant files
├── Read and understand code
├── Plan implementation
├── Write code
├── Test
└── Fix issues
```

#### 2. Iterative Refinement

Agents can observe results and correct mistakes:

```
Agent: writes code
→ runs tests
→ sees failure
→ reads error
→ fixes code
→ runs tests again
→ success
```

#### 3. Tool Augmentation

LLMs alone can only generate text. Tools give them **actions**:

| Tool | Capability Added |
|------|------------------|
| Read | See file contents |
| Write | Modify files |
| Bash | Execute commands |
| Search | Find information |
| Web | Access external data |

#### 4. Context Management

Agents maintain state across multiple steps:
- Remember what they've done
- Track what's left to do
- Build understanding over time

### Why This Works Better Than Single-Shot

| Single Prompt | Agent |
|---------------|-------|
| Must guess everything upfront | Can explore and learn |
| No error correction | Can retry and fix |
| Limited context | Gathers context as needed |
| One attempt | Multiple iterations |

### The "ReAct" Pattern

Most agents use **Reasoning + Acting**:

```
Thought: I need to find where authentication is handled
Action: Grep("auth")
Observation: Found 5 files...

Thought: auth.ts looks most relevant
Action: Read("auth.ts")
Observation: [file contents]

Thought: Now I understand the auth flow...
Action: Edit(...)
```

### Mathematical View

```
Agent = f(state, goal) → action

where:
- state = (conversation history, tool results, current files)
- goal = user's objective
- action = next tool call or response
- f = LLM reasoning
```

### Why Effective for Coding?

1. **Exploration**: Can search unfamiliar codebases
2. **Verification**: Can run tests to validate changes
3. **Iteration**: Can fix errors until code works
4. **Context Building**: Reads only what's needed, when needed

---

## Quick Reference

### Install a Plugin

```bash
/plugin marketplace add https://github.com/anthropics/claude-plugins-official
/plugin install <plugin-name>@claude-plugins-official
# Restart Claude Code
```

### Enable/Disable Plugin

Edit `~/.claude/settings.json`:

```json
{
  "enabledPlugins": {
    "plugin-name@marketplace": true
  }
}
```

### Check Plugin Status

```bash
/plugin list
/plugin
```

### File Locations

| File | Purpose |
|------|---------|
| `~/.claude/settings.json` | Global settings |
| `~/.claude/plugins/installed_plugins.json` | Installed plugins |
| `~/.claude/plugins/marketplaces/` | Downloaded marketplaces |
| `~/.claude/plugins/cache/` | Plugin cache |
| `./CLAUDE.md` | Project instructions |
| `./.claude/settings.json` | Project settings |

---

**Last Updated**: 2026-01-13
