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
      // Empty/unset is passed through as undefined, not defaulted to 'lux'
      // here: the server does its own auto-detection (workspace's
      // build/lux, then PATH) so installing the extension needs no setup.
      compilerPath: config.get<string>('compilerPath') || undefined,
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
