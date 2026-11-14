ipc: An elevated service on Windows now works with unelevated apps. It duplicates all handles itself, and it admits its own user's apps on the pipe without IPC_RELAXED_CONNECTION_SECURITY.
