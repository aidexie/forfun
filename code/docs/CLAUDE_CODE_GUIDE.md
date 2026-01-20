# Claude Code Guide

Comprehensive guide to Claude Code features, extensibility, and how agents work.

---

## Table of Contents

1. [Feature Overview](#feature-overview)
2. [Plugins](#plugins)
3. [Agents](#agents)
4. [Hooks](#hooks)
5. [MCP Servers (Model Context Protocol)](#mcp-servers)
6. [Skills (Slash Commands)](#skills-slash-commands)
7. [Memory (CLAUDE.md)](#memory-claudemd)
8. [Settings](#settings)
9. [How Agents Work](#how-agents-work)
10. [Why Agents Are Effective](#why-agents-are-effective)
11. [Local Configuration (This Project)](#local-configuration-this-project)
12. [Quick Reference](#quick-reference)
13. [Plugin Features](#plugin-features)

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

MCP (Model Context Protocol) is a **standardized protocol** for connecting AI models to external tools and data sources. Think of it as a **USB for AI** - a universal way to plug in capabilities.

### What is MCP?

```
┌─────────────┐         MCP Protocol         ┌─────────────┐
│   Claude    │ ◄──────────────────────────► │  MCP Server │
│   (Client)  │      JSON-RPC over stdio     │  (Tools)    │
└─────────────┘                              └─────────────┘
```

**Core Principle - Separation of Concerns**:
- **AI Model**: Reasoning, decision-making
- **MCP Server**: Tool execution, data access

The AI doesn't need to know HOW tools work - just WHAT they do.

### Architecture

```
┌────────────────────────────────────────────────────────┐
│                    Claude Code (Host)                   │
├────────────────────────────────────────────────────────┤
│                    MCP Client                          │
│  ┌──────────────────────────────────────────────────┐  │
│  │  Discovers servers → Calls tools → Gets results  │  │
│  └──────────────────────────────────────────────────┘  │
└────────────────────┬───────────────────────────────────┘
                     │ JSON-RPC (stdio/HTTP)
     ┌───────────────┼───────────────┬───────────────┐
     ▼               ▼               ▼               ▼
┌─────────┐   ┌─────────┐     ┌─────────┐     ┌─────────┐
│ GitHub  │   │Postgres │     │  Slack  │     │  File   │
│ Server  │   │ Server  │     │ Server  │     │ System  │
└─────────┘   └─────────┘     └─────────┘     └─────────┘
```

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

### Communication Flow

```
Step 1: Discovery
─────────────────
Client: "What tools do you have?"
Server: [
  { name: "query", description: "Run SQL query" },
  { name: "insert", description: "Insert data" }
]

Step 2: Tool Call
─────────────────
Client: { method: "query", params: { sql: "SELECT * FROM users" } }
Server: { result: [{ id: 1, name: "John" }, ...] }

Step 3: Result
─────────────────
Claude receives data and uses it in response
```

### Protocol Details

MCP uses **JSON-RPC 2.0** over stdio:

```json
// Request (Client → Server)
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "github_create_issue",
    "arguments": {
      "repo": "owner/repo",
      "title": "Bug report",
      "body": "Description..."
    }
  }
}

// Response (Server → Client)
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "issue_url": "https://github.com/owner/repo/issues/123"
  }
}
```

### What a Server Provides

| Capability | Description |
|------------|-------------|
| **Tools** | Actions the AI can take |
| **Resources** | Data the AI can read |
| **Prompts** | Pre-built prompt templates |

### Example MCP Server (TypeScript)

```typescript
import { Server } from "@modelcontextprotocol/sdk/server";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio";

const server = new Server({
  name: "my-server",
  version: "1.0.0"
}, {
  capabilities: { tools: {} }
});

// Define a tool
server.setRequestHandler("tools/list", async () => ({
  tools: [{
    name: "get_weather",
    description: "Get weather for a city",
    inputSchema: {
      type: "object",
      properties: {
        city: { type: "string" }
      }
    }
  }]
}));

// Handle tool calls
server.setRequestHandler("tools/call", async (request) => {
  if (request.params.name === "get_weather") {
    const city = request.params.arguments.city;
    const weather = await fetchWeather(city);
    return { content: [{ type: "text", text: weather }] };
  }
});

// Start server
const transport = new StdioServerTransport();
server.connect(transport);
```

### Example MCP Server (Python)

```python
from mcp import Server

server = Server("postgres-server")

@server.tool("query")
def query(sql: str) -> list:
    """Execute a SQL query"""
    return database.execute(sql)

@server.tool("insert")
def insert(table: str, data: dict) -> bool:
    """Insert a row into a table"""
    return database.insert(table, data)

@server.resource("schema")
def get_schema() -> str:
    """Get database schema"""
    return database.get_schema()

server.run()
```

### How MCP Works in Practice

```
User: "Query the database for all users created this week"
→ Claude uses postgres MCP server
→ Executes: SELECT * FROM users WHERE created_at > NOW() - INTERVAL '7 days'
→ Returns results

User: "Create a GitHub issue for this bug"
→ Claude uses github MCP server
→ Creates issue via GitHub API
```

### What Happens on Startup

```
1. Claude Code starts
2. Spawns each MCP server as subprocess
3. Connects via stdio
4. Asks each server: "What tools do you have?"
5. Tools become available to Claude
```

### Core Principles

#### 1. Standardization

One protocol for all tools:

```
Before MCP:
├── Claude has custom GitHub integration
├── Claude has custom Slack integration
├── Claude has custom Database integration
└── Each is different, hard to maintain

After MCP:
├── GitHub MCP Server (standard protocol)
├── Slack MCP Server (standard protocol)
├── Database MCP Server (standard protocol)
└── All use the same interface
```

#### 2. Decoupling

AI model is separate from tool implementation:

```
┌─────────────────┐         ┌─────────────────┐
│     Claude      │         │   MCP Server    │
├─────────────────┤         ├─────────────────┤
│ Knows: "I can   │   →→→   │ Knows: How to   │
│ call 'query'"   │         │ execute SQL     │
│                 │   ←←←   │                 │
│ Gets: Results   │         │ Returns: Data   │
└─────────────────┘         └─────────────────┘
```

#### 3. Security Boundary

Server controls what's exposed:

```
┌─────────────────────────────────────────┐
│              MCP Server                  │
├─────────────────────────────────────────┤
│  Exposes:          │  Hidden:           │
│  - query()         │  - Connection str  │
│  - insert()        │  - Credentials     │
│                    │  - Internal logic  │
└─────────────────────────────────────────┘
```

#### 4. Composability

Multiple servers work together:

```
Claude Code
├── GitHub Server    → Create issues, PRs
├── Postgres Server  → Query database
├── Slack Server     → Send messages
└── File Server      → Read/write files

All available simultaneously!
```

### MCP vs Direct API Calls

| Aspect | Direct API | MCP |
|--------|------------|-----|
| **Integration** | Custom per-service | Standardized |
| **Security** | API keys in prompt | Server manages auth |
| **Discovery** | Hardcoded | Dynamic tool listing |
| **Maintenance** | Update Claude | Update server only |
| **Extensibility** | Requires model changes | Add new servers |

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

## Local Configuration (This Project)

### Notification Hooks

This project uses hooks to notify when Claude needs attention.

**Location**: `.claude/settings.local.json`

```json
{
  "hooks": {
    "Stop": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "powershell -ExecutionPolicy Bypass -File \"E:/forfun/source/code/.claude/notify.ps1\""
          }
        ]
      }
    ],
    "PreToolUse": [
      {
        "matcher": "AskUserQuestion",
        "hooks": [
          {
            "type": "command",
            "command": "powershell -ExecutionPolicy Bypass -File \"E:/forfun/source/code/.claude/notify.ps1\" -Message \"Claude has a question\""
          }
        ]
      }
    ]
  }
}
```

| Hook | Matcher | Trigger | Message |
|------|---------|---------|---------|
| `Stop` | (any) | Claude stops and waits for input | "Needs input" |
| `PreToolUse` | `AskUserQuestion` | Claude presents choices/questions | "Claude has a question" |

**Notification Script**: `.claude/notify.ps1`
- Uses BurntToast module if installed
- Falls back to Windows Toast via .NET
- Final fallback: console beep + colored output
- Reads `CLAUDE_TAB` environment variable to identify which terminal

### VS Code Shortcuts for Claude 1-5

Keyboard shortcuts to launch Claude Code with different tab identifiers.

**Terminal Profiles** (in VS Code `settings.json`):

```json
{
  "terminal.integrated.profiles.windows": {
    "Claude 1": {
      "path": "powershell.exe",
      "args": ["-NoExit", "-Command", "$Host.UI.RawUI.WindowTitle='CC 1'; $env:CLAUDE_TAB='1'; claude"],
      "icon": "robot",
      "color": "terminal.ansiBlue",
      "overrideName": true
    },
    "Claude 2": {
      "path": "powershell.exe",
      "args": ["-NoExit", "-Command", "$Host.UI.RawUI.WindowTitle='CC 2'; $env:CLAUDE_TAB='2'; claude"],
      "icon": "robot",
      "color": "terminal.ansiGreen",
      "overrideName": true
    },
    "Claude 3": {
      "path": "powershell.exe",
      "args": ["-NoExit", "-Command", "$Host.UI.RawUI.WindowTitle='CC 3'; $env:CLAUDE_TAB='3'; claude"],
      "icon": "robot",
      "color": "terminal.ansiYellow",
      "overrideName": true
    },
    "Claude 4": {
      "path": "powershell.exe",
      "args": ["-NoExit", "-Command", "$Host.UI.RawUI.WindowTitle='CC 4'; $env:CLAUDE_TAB='4'; claude"],
      "icon": "robot",
      "color": "terminal.ansiMagenta",
      "overrideName": true
    },
    "Claude 5": {
      "path": "powershell.exe",
      "args": ["-NoExit", "-Command", "$Host.UI.RawUI.WindowTitle='CC 5'; $env:CLAUDE_TAB='5'; claude"],
      "icon": "robot",
      "color": "terminal.ansiCyan",
      "overrideName": true
    }
  }
}
```

**Keybindings** (in VS Code `keybindings.json`):

```json
[
  { "key": "ctrl+alt+1", "command": "workbench.action.terminal.newWithProfile", "args": { "profileName": "Claude 1" } },
  { "key": "ctrl+alt+2", "command": "workbench.action.terminal.newWithProfile", "args": { "profileName": "Claude 2" } },
  { "key": "ctrl+alt+3", "command": "workbench.action.terminal.newWithProfile", "args": { "profileName": "Claude 3" } },
  { "key": "ctrl+alt+4", "command": "workbench.action.terminal.newWithProfile", "args": { "profileName": "Claude 4" } },
  { "key": "ctrl+alt+5", "command": "workbench.action.terminal.newWithProfile", "args": { "profileName": "Claude 5" } }
]
```

| Shortcut | Terminal | Color |
|----------|----------|-------|
| `Ctrl+Alt+1` | Claude 1 | Blue |
| `Ctrl+Alt+2` | Claude 2 | Green |
| `Ctrl+Alt+3` | Claude 3 | Yellow |
| `Ctrl+Alt+4` | Claude 4 | Magenta |
| `Ctrl+Alt+5` | Claude 5 | Cyan |

**How It Works**:
1. Shortcut opens new terminal with profile
2. Profile sets `CLAUDE_TAB` environment variable
3. Profile runs `claude` command automatically
4. When Claude needs input, notification shows "Claude: 1 - Needs input" (or appropriate tab number)

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

## Plugin Features

Plugins can include multiple types of features:

### 1. Skills (Slash Commands)

User-invocable commands:

```bash
/commit           # from commit-commands plugin
/code-review      # from code-review plugin
/code-simplifier  # from code-simplifier plugin
```

### 2. Agents

Autonomous AI entities that can be spawned:

```
code-simplifier plugin includes:
└── agents/code-simplifier.md  # Agent definition

The agent:
- Uses Opus model
- Simplifies recently modified code
- Follows project standards
```

### 3. LSP Servers (Language Server Protocol)

Code intelligence backends:

```json
// clangd-lsp plugin
{
  "lspServers": {
    "clangd": {
      "command": "clangd",
      "args": ["--background-index"],
      "extensionToLanguage": {
        ".cpp": "cpp",
        ".h": "c"
      }
    }
  }
}
```

### 4. MCP Servers (Model Context Protocol)

External tool providers:

```json
// github plugin
{
  "mcpServers": {
    "github": {
      "command": "npx",
      "args": ["@modelcontextprotocol/server-github"]
    }
  }
}
```

### 5. Hooks

Event-triggered commands:

```json
// security-guidance plugin
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Write",
        "hooks": [{
          "type": "command",
          "command": "security-check.sh"
        }]
      }
    ]
  }
}
```

### Plugin Structure

A typical plugin folder:

```
plugin-name/
├── .claude-plugin/
│   └── plugin.json       # Plugin metadata
├── agents/
│   └── agent-name.md     # Agent definitions
├── skills/
│   └── skill-name.md     # Skill definitions
└── hooks/
    └── hook-config.json  # Hook configurations
```

### plugin.json Example

```json
{
  "name": "code-simplifier",
  "version": "1.0.0",
  "description": "Simplifies code for clarity",
  "author": {
    "name": "Anthropic",
    "email": "support@anthropic.com"
  }
}
```

### Feature Matrix by Plugin

| Plugin | Skills | Agents | LSP | MCP | Hooks |
|--------|--------|--------|-----|-----|-------|
| `commit-commands` | ✅ `/commit`, `/commit-push-pr` | | | | |
| `code-review` | ✅ `/code-review` | ✅ | | | |
| `code-simplifier` | ✅ `/code-simplifier` | ✅ | | | |
| `clangd-lsp` | | | ✅ | | |
| `typescript-lsp` | | | ✅ | | |
| `github` | | | | ✅ | |
| `playwright` | | | | ✅ | |
| `security-guidance` | | | | | ✅ |
| `hookify` | ✅ | | | | ✅ |
| `pr-review-toolkit` | | ✅ Multiple | | | |
| `feature-dev` | ✅ | ✅ Multiple | | | |

### Summary

| Feature Type | Purpose | Example |
|--------------|---------|---------|
| **Skills** | User-invoked commands | `/commit` |
| **Agents** | Autonomous task execution | code-simplifier agent |
| **LSP** | Code intelligence | clangd, typescript-lsp |
| **MCP** | External tool integration | GitHub, Postgres |
| **Hooks** | Event-triggered actions | Security warnings |

A single plugin can include **any combination** of these features.

---

**Last Updated**: 2026-01-20
