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

  const config = workspace.getConfiguration('lux');

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'lux' }],
    synchronize: {
      fileEvents: workspace.createFileSystemWatcher('**/*.lux'),
    },
    initializationOptions: {
      compilerPath: config.get<string>('compilerPath') || 'lux',
    },
  };

  client = new LanguageClient(
    'luxLanguageServer',
    'Lux Script Language Server',
    serverOptions,
    clientOptions,
  );

  client.start();
}

export function deactivate(): Thenable<void> | undefined {
  if (!client) return undefined;
  return client.stop();
}
