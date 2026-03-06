import * as path from "path";
import * as fs from "fs";
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

/**
 * Find the clat-lsp binary. Checks:
 *  1. The lattice.lsp.path setting
 *  2. PATH lookup
 *  3. Common install locations
 */
function findServerBinary(): string | undefined {
  const config = vscode.workspace.getConfiguration("lattice.lsp");
  const configured = config.get<string>("path", "").trim();
  if (configured && fs.existsSync(configured)) {
    return configured;
  }

  // Check PATH by trying common names
  const names = process.platform === "win32"
    ? ["clat-lsp.exe", "clat-lsp"]
    : ["clat-lsp"];

  for (const name of names) {
    const envPath = process.env.PATH || "";
    const dirs = envPath.split(path.delimiter);
    for (const dir of dirs) {
      const full = path.join(dir, name);
      if (fs.existsSync(full)) {
        return full;
      }
    }
  }

  // Check common install locations
  const home = process.env.HOME || process.env.USERPROFILE || "";
  const candidates = [
    path.join(home, ".local", "bin", "clat-lsp"),
    path.join(home, "bin", "clat-lsp"),
    "/usr/local/bin/clat-lsp",
    "/usr/bin/clat-lsp",
  ];
  if (process.platform === "win32") {
    candidates.push(path.join(home, "AppData", "Local", "lattice", "clat-lsp.exe"));
  }
  if (process.platform === "darwin") {
    candidates.push("/opt/homebrew/bin/clat-lsp");
  }

  for (const c of candidates) {
    if (fs.existsSync(c)) {
      return c;
    }
  }

  return undefined;
}

async function startClient(context: vscode.ExtensionContext): Promise<void> {
  const config = vscode.workspace.getConfiguration("lattice.lsp");
  if (!config.get<boolean>("enabled", true)) {
    return;
  }

  const serverPath = findServerBinary();
  if (!serverPath) {
    const choice = await vscode.window.showWarningMessage(
      "Lattice language server (clat-lsp) not found. Build it with `make lsp` in the Lattice source directory, or set `lattice.lsp.path` in settings.",
      "Open Settings"
    );
    if (choice === "Open Settings") {
      vscode.commands.executeCommand("workbench.action.openSettings", "lattice.lsp.path");
    }
    return;
  }

  const serverOptions: ServerOptions = {
    command: serverPath,
    args: [],
  };

  const traceLevel = config.get<string>("trace", "off");

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "lattice" }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.lat"),
    },
    outputChannelName: "Lattice Language Server",
    traceOutputChannel:
      traceLevel !== "off"
        ? vscode.window.createOutputChannel("Lattice LSP Trace")
        : undefined,
  };

  client = new LanguageClient(
    "lattice-lsp",
    "Lattice Language Server",
    serverOptions,
    clientOptions
  );

  await client.start();
}

async function stopClient(): Promise<void> {
  if (client) {
    await client.stop();
    client = undefined;
  }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  // Register restart command
  context.subscriptions.push(
    vscode.commands.registerCommand("lattice.restartServer", async () => {
      await stopClient();
      await startClient(context);
      vscode.window.showInformationMessage("Lattice language server restarted.");
    })
  );

  // React to config changes
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration(async (e) => {
      if (e.affectsConfiguration("lattice.lsp")) {
        await stopClient();
        await startClient(context);
      }
    })
  );

  await startClient(context);
}

export async function deactivate(): Promise<void> {
  await stopClient();
}
