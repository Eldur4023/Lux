import * as path from 'path';
import { workspace, ExtensionContext } from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient;

export function activate(context: ExtensionContext) {
  const serverModule = context.asAbsolutePath(path.join('server', 'out', 'server.js'));

  const serverOptions: ServerOptions = {
    run: { module: serverModule, transport: TransportKind.stdio },
    debug: { module: serverModule, transport: TransportKind.stdio },
  };

  const config = workspace.getConfiguration('lumen');

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'lumen' }],
    synchronize: {
      fileEvents: workspace.createFileSystemWatcher('**/*.lum'),
    },
    initializationOptions: {
      compilerPath: config.get<string>('compilerPath') || 'lumen',
    },
  };

  client = new LanguageClient(
    'lumenLanguageServer',
    'Lumen Script Language Server',
    serverOptions,
    clientOptions,
  );

  client.start();
}

export function deactivate(): Thenable<void> | undefined {
  if (!client) return undefined;
  return client.stop();
}
