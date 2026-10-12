// The vkworld Lua debugger speaks the Debug Adapter Protocol itself (script/lua_debugger.h in the engine): all this
// extension does is tell VS Code where to find it.
const vscode = require('vscode');

function activate(context) {
    context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('vkworld-lua', {
        createDebugAdapterDescriptor(session) {
            const c = session.configuration;
            return new vscode.DebugAdapterServer(c.port || 4711, c.host || '127.0.0.1');
        }
    }));
}

function deactivate() {}

module.exports = { activate, deactivate };
