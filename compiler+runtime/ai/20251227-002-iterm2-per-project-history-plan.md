# iTerm2 Per-Project History & Crash-Safe Configuration Plan

## Problem Statement

We want:
1. **Per-project/directory history** - Each project should have its own command history
2. **Crash-safe persistence** - History survives crashes, force quits, and power failures
3. **Immediate saving** - Commands saved as they're entered, not on shell exit

## Research Summary

### Current iTerm2 Native Capabilities

iTerm2 with [Shell Integration](https://iterm2.com/documentation-shell-integration.html) provides:
- Command history stored per `username+hostname` (not per directory)
- Directory history with "frecency" sorting
- Settings > General > "Save copy/paste and command history to disk" (up to 200 commands)

[Session Restoration](https://iterm2.com/documentation-restoration.html) allows:
- Jobs run in long-lived servers that survive iTerm2 crashes
- Scrollback history preserved via OS window restoration
- **Caveat**: Cmd-Q terminates jobs and won't restore them

**Limitation**: iTerm2 doesn't natively support per-directory history - it's always per username+hostname.

---

## Solutions Analysis

### Option 1: Zsh Native Configuration (Crash-Safe Only)

**What it solves**: Immediate history saving, crash safety
**What it doesn't solve**: Per-project history

```zsh
# ~/.zshrc - Crash-safe history configuration

# History file location
export HISTFILE="$HOME/.zsh_history"

# Size settings
export HISTSIZE=100000      # Commands loaded into memory
export SAVEHIST=100000      # Commands saved to file

# Critical options for crash safety
setopt INC_APPEND_HISTORY   # Write immediately, not on shell exit
setopt EXTENDED_HISTORY     # Save timestamp and duration
setopt HIST_IGNORE_DUPS     # No consecutive duplicates
setopt HIST_IGNORE_SPACE    # Commands starting with space not saved
setopt HIST_VERIFY          # Show command before executing from history

# Note: SHARE_HISTORY implies INC_APPEND_HISTORY but also reads from other sessions
# Choose one:
# setopt SHARE_HISTORY      # Share history across all terminal sessions (may be noisy)
```

**Pros**:
- Zero additional dependencies
- Native zsh functionality
- Fast and reliable

**Cons**:
- Single global history file
- No per-project separation

---

### Option 2: oh-my-zsh per-directory-history Plugin

**What it solves**: Per-directory history + global history toggle
**Source**: [ohmyzsh/per-directory-history](https://github.com/ohmyzsh/ohmyzsh/blob/master/plugins/per-directory-history/per-directory-history.zsh)

```zsh
# If using oh-my-zsh, add to plugins:
plugins=(... per-directory-history)

# Or source directly:
source /path/to/per-directory-history.zsh

# Configuration (in ~/.zshrc, BEFORE sourcing):
export HISTORY_BASE="$HOME/.directory_history"      # Where histories are stored
export HISTORY_START_WITH_GLOBAL=false              # Start with dir history
export PER_DIRECTORY_HISTORY_TOGGLE='^G'            # Ctrl+G to toggle
export PER_DIRECTORY_HISTORY_PRINT_MODE_CHANGE=true # Show mode changes
```

**How it works**:
- Creates `~/.directory_history/` folder
- Each directory gets its own history file (path-encoded)
- Commands always saved to BOTH local and global history
- Ctrl+G toggles between local/global view

**Pros**:
- Works with existing oh-my-zsh setup
- Simple to understand
- Can always fall back to global history

**Cons**:
- History per EXACT directory (not per project root)
- Could end up with many small history files

---

### Option 3: ivan-cukic/per-project-history Plugin (RECOMMENDED for projects)

**What it solves**: Per-PROJECT history (detects project roots)
**Source**: [ivan-cukic/per-project-history](https://github.com/ivan-cukic/per-project-history)

```zsh
# Installation with zinit:
zinit light ivan-cukic/per-project-history

# Or manual source:
source /path/to/per-project-history.zsh

# Configuration (BEFORE sourcing):
declare -a PER_PROJECT_HISTORY_TAGS
PER_PROJECT_HISTORY_TAGS=(.git .hg .jj .envrc .per_project_history)
declare -r PER_PROJECT_HISTORY_TAGS
```

**How it works**:
- Searches for "tag files" (.git, .envrc, etc.) in current and parent directories
- Uses the closest parent with a tag as the "project root"
- All subdirectories of a project share ONE history
- Ctrl+G toggles between project/global history

**Default tag files**: `.git .hg .jj .stack-work .cabal .cargo .envrc .per_project_history`

**Example**:
- `/Users/pfeodrippe/dev/jank/` has `.git`
- Commands in `/Users/pfeodrippe/dev/jank/compiler+runtime/src/` use the same history as `/Users/pfeodrippe/dev/jank/`

**Pros**:
- Project-aware (not just directory-aware)
- Works naturally with git repos
- Can add `.per_project_history` to any folder to mark it as project root

**Cons**:
- Requires additional plugin installation
- Slightly more complex than per-directory

---

### Option 4: Atuin (Modern Full Replacement)

**What it solves**: Everything - SQLite database, per-directory context, sync, crash-safe
**Source**: [Atuin](https://atuin.sh/) | [GitHub](https://github.com/atuinsh/atuin)

```zsh
# Installation
curl --proto '=https' --tlsv1.2 -LsSf https://setup.atuin.sh | sh

# Or via Homebrew
brew install atuin

# Add to ~/.zshrc
eval "$(atuin init zsh)"

# Configuration (~/.config/atuin/config.toml)
[sync]
sync_frequency = "10m"   # Sync every 10 minutes (default: 1h)

[filter]
# Filter modes: global, host, session, directory, workspace
filter_mode = "directory"  # Show only commands from current directory by default
```

**Key Features**:
- **SQLite database** with rich metadata (exit code, duration, cwd, hostname)
- **Filter modes**: global, host, session, directory, workspace
- **End-to-end encrypted sync** across machines
- **Self-hostable** or use their cloud service
- **Dotfiles sync** (aliases, env vars)
- **Works with**: zsh, bash, fish, nushell, xonsh

**Atuin Filter Modes**:
| Mode | What it shows |
|------|---------------|
| `global` | All history everywhere |
| `host` | Commands from current machine |
| `session` | Commands from current terminal session |
| `directory` | Commands run in current directory |
| `workspace` | Commands from git repository root |

**Pros**:
- Most powerful and feature-rich option
- Excellent search UI (Ctrl+R)
- Cross-machine sync with encryption
- Context-aware (directory, exit code, duration)
- Active development (desktop app coming)

**Cons**:
- Replaces shell history completely
- Learning curve for advanced features
- Requires Rust binary

---

### Option 5: McFly (Neural Network Suggestions)

**What it solves**: Intelligent history search with directory context
**Source**: [McFly](https://github.com/cantino/mcfly)

```zsh
# Installation
brew install mcfly

# Add to ~/.zshrc
eval "$(mcfly init zsh)"

# Configuration via environment variables
export MCFLY_RESULTS=50              # Number of results to show
export MCFLY_FUZZY=2                 # Fuzzy search threshold
export MCFLY_INTERFACE_VIEW=TOP      # Results at top of screen
export MCFLY_DISABLE_MENU=FALSE      # Show menu interface
```

**How it works**:
- Small neural network ranks command suggestions
- Considers: current directory, previous commands, execution time, exit status
- SQLite database in `~/Library/Application Support/McFly` (macOS)
- Maintains normal ~/.zsh_history as backup

**Pros**:
- Intelligent ranking based on context
- Directory-aware suggestions
- Learns from your behavior
- Keeps normal history file as fallback

**Cons**:
- No sync between machines
- Neural network is a black box
- Less explicit per-project separation

---

### Option 6: hiSHtory (E2E Encrypted with Context)

**What it solves**: Context-aware history with sync
**Source**: [hiSHtory](https://github.com/ddworken/hishtory)

```zsh
# Installation
curl https://hishtory.dev/install.py | python3 -

# Configuration
hishtory config-set filter-duplicate-commands true
```

**Features**:
- SQLite database with context (directory, exit status, duration, hostname)
- End-to-end encrypted sync
- Self-hostable backend
- AI shell assistance (query ChatGPT with `?` prefix)
- Query directly in SQLite: `sqlite3 ~/.hishtory/.hishtory.db`

**Pros**:
- Context-aware like Atuin
- E2E encrypted sync
- Can query raw SQLite
- AI integration

**Cons**:
- Less mature than Atuin
- Smaller community

---

## Recommendations

### For Your Use Case (jank development)

**Recommended Stack**:

```
┌─────────────────────────────────────────────────────┐
│  Layer 1: Crash-Safe Base (Zsh native)              │
│  - INC_APPEND_HISTORY for immediate saves           │
│  - EXTENDED_HISTORY for timestamps                  │
├─────────────────────────────────────────────────────┤
│  Layer 2: Per-Project History                       │
│  - ivan-cukic/per-project-history plugin            │
│  - Uses .git as project marker                      │
│  - Ctrl+G to toggle global/project                  │
├─────────────────────────────────────────────────────┤
│  Layer 3 (Optional): Enhanced Search               │
│  - Atuin OR McFly for better search UI              │
│  - Atuin if you want cross-machine sync             │
└─────────────────────────────────────────────────────┘
```

### Minimal Setup (Low Overhead)

Add to `~/.zshrc`:

```zsh
# Crash-safe history
export HISTFILE="$HOME/.zsh_history"
export HISTSIZE=100000
export SAVEHIST=100000
setopt INC_APPEND_HISTORY
setopt EXTENDED_HISTORY
setopt HIST_IGNORE_DUPS

# Per-project history (install ivan-cukic/per-project-history first)
# git clone https://github.com/ivan-cukic/per-project-history ~/.zsh/per-project-history
source ~/.zsh/per-project-history/per-project-history.zsh
```

### Maximum Power Setup (Atuin)

```bash
# Install Atuin
brew install atuin

# Add to ~/.zshrc
eval "$(atuin init zsh)"
```

Create `~/.config/atuin/config.toml`:

```toml
[settings]
# Show commands from current directory first
filter_mode = "directory"

# Fallback to global when no directory matches
filter_mode_shell_up_key_binding = "directory"

# Sync every 10 minutes
sync_frequency = "10m"

[sync]
# Use their cloud or self-host
# records = true  # Enable sync
```

---

## iTerm2 Specific Settings

Regardless of which shell solution you choose, enable these iTerm2 settings:

1. **Preferences > General > Closing**:
   - [ ] "Confirm closing multiple sessions" - your choice

2. **Preferences > General > Services**:
   - [x] "Save copy/paste history and command history to disk"

3. **System Settings > Desktop & Dock**:
   - [ ] "Close windows when quitting an application" - **UNCHECK THIS**
   - This enables session restoration after crashes

4. **Preferences > General > Startup**:
   - Set to "Use System Window Restoration Setting"

5. **Install Shell Integration**:
   ```bash
   curl -L https://iterm2.com/shell_integration/install_shell_integration.sh | bash
   ```

---

## Decision Matrix

| Feature | Zsh Native | per-directory | per-project | Atuin | McFly | hiSHtory |
|---------|-----------|---------------|-------------|-------|-------|----------|
| Crash-safe | Yes* | Yes* | Yes* | Yes | Yes | Yes |
| Per-directory | No | Yes | No | Yes | Context | Yes |
| Per-project | No | No | Yes | Workspace | Context | No |
| Cross-machine sync | No | No | No | Yes | No | Yes |
| Rich metadata | No | No | No | Yes | Yes | Yes |
| Search UI | Basic | Basic | Basic | Excellent | Good | Good |
| Dependencies | None | Plugin | Plugin | Rust bin | Rust bin | Go bin |
| Learning curve | None | Low | Low | Medium | Low | Medium |

*With `INC_APPEND_HISTORY` or `SHARE_HISTORY` enabled

---

## Implementation Steps

### Quick Start (5 minutes)

1. Add crash-safe settings to `~/.zshrc`
2. Install per-project-history plugin
3. Restart terminal

### Full Setup (15 minutes)

1. Configure iTerm2 settings (session restoration)
2. Install shell integration
3. Configure zsh crash-safe options
4. Install Atuin or per-project-history
5. Test by running commands, force-killing terminal, and verifying history

---

## Sources

- [iTerm2 Shell Integration](https://iterm2.com/documentation-shell-integration.html)
- [iTerm2 Session Restoration](https://iterm2.com/documentation-restoration.html)
- [Feature Request: Per-tab history](https://gitlab.com/gnachman/iterm2/-/issues/7472)
- [jim-hester/per-directory-history](https://github.com/jimhester/per-directory-history)
- [ivan-cukic/per-project-history](https://github.com/ivan-cukic/per-project-history)
- [oh-my-zsh per-directory-history plugin](https://github.com/ohmyzsh/ohmyzsh/blob/master/plugins/per-directory-history/per-directory-history.zsh)
- [Atuin - Magical Shell History](https://atuin.sh/)
- [McFly - Neural Network Shell History](https://github.com/cantino/mcfly)
- [hiSHtory - Synced, Queryable History](https://github.com/ddworken/hishtory)
- [Zsh History Options Reference](https://zsh.sourceforge.io/Doc/Release/Options.html)
- [Mastering Zsh History Config](https://github.com/rothgar/mastering-zsh/blob/master/docs/config/history.md)
