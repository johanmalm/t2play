t2play

Bringing the tint2 spirit to the modern desktop

## Configuration

t2play reads `$XDG_CONFIG_HOME/t2play/config.yaml` (falling back to
`~/.config/t2play/config.yaml`) on startup. Example:

```yaml
panel_items: TC
```

### panel_items

A string of single-character codes that controls which items appear on the
panel and in what order:

| Code | Item    | Position   |
|------|---------|------------|
| T    | Taskbar | left       |
| C    | Clock   | right      |

Default: `TC`
