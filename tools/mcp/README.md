# Orchestral DAW MCP adapter

Exposes the app's control API (see ../../API.md) to AI agents as MCP tools.

One Python file, no dependencies (Python 3.10+). On startup it asks the running app
for its command surface (`describe`) and generates **one tool per command**
(`daw_transport_play`, `daw_clip_addNotes`, ...), so new commands appear here
automatically. A raw `daw_command` passthrough is always available, including when
the app isn't running yet.

## Recommended: let the app run it

Enable **Settings > Agents (MCP)** in the app. The server then starts and stops with
the app (HTTP on port 53218). Register once:

```
claude mcp add --transport http orchestral-daw http://127.0.0.1:53218/mcp
```

## Alternative: client-spawned (stdio)

```
claude mcp add orchestral-daw -- python C:\Users\kajai\Tie\loitsut\orchestral-daw\tools\mcp\orchestral_daw_mcp.py
```

## Claude Desktop (claude_desktop_config.json)

```json
{
  "mcpServers": {
    "orchestral-daw": {
      "command": "python",
      "args": ["C:\\Users\\kajai\\Tie\\loitsut\\orchestral-daw\\tools\\mcp\\orchestral_daw_mcp.py"]
    }
  }
}
```

## Notes

- The app must be running for the full tool catalogue; the adapter refreshes it
  automatically (15 s cache), so starting the app later just works.
- Musical time is in ticks: 960000 per quarter note (tools that take ticks say so).
- Slow commands (`instrument.add`, `project.load`) get long timeouts - sample
  libraries take time to stream in.
- `DAW_API_PORT` overrides the default port 53217.
