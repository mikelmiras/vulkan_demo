# vkworld Lua debugger for VS Code

Debug a game's Lua scripts (`scripts/*.lua`) while it runs: breakpoints (conditions, hit counts, logpoints),
stepping, the stack, locals and upvalues, tables to expand, watches and hovers, setting variables, data breakpoints
on a table's fields, and coroutines as threads.

1. Install: copy this folder into your VS Code extensions folder (`~/.vscode/extensions/vkworld-lua`, or
   `%USERPROFILE%\.vscode\extensions\vkworld-lua` on Windows) and restart VS Code. (No dependencies to install.)
2. Start the game with the debugger listening: the editor's **Game > Lua debugger**, or `vkgame --lua-debug`
   (port 4711; `--lua-debug 5000` for another).
3. Open the project folder in VS Code and add to `.vscode/launch.json`:

   ```json
   { "type": "vkworld-lua", "request": "attach", "name": "Attach to vkworld", "host": "127.0.0.1", "port": 4711 }
   ```

4. Run *Attach to vkworld*, set breakpoints in your scripts. When the game reaches one it stops (its window stops
   drawing) until you continue.
