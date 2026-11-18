# WindowCloud

[Русский](README.ru.md)

WindowCloud shows your connected Google Drive, OneDrive, and Dropbox accounts under **This PC** in Windows File Explorer. The app runs in the system tray. Each account appears as a separate virtual folder while the app is running and the provider is reachable.

## Getting started

1. Download `WindowCloud-win-x64-self-contained.zip` from the [latest release](https://github.com/Frish07/WindowCloud/releases/latest) and extract it to a folder. The ZIP includes .NET Desktop Runtime 10 and runs on Windows x64.
2. Launch `WindowCloud.exe`, click the WindowCloud icon in the system tray, and select a provider.
3. The first time you connect a provider in the public build, enter the OAuth client details described below. Sign in through your browser and grant read and write access to your files. Repeat the sign-in process to connect another account. Reconnect accounts added in version 1.1 to grant upload permissions.
4. Open **This PC**. Double-click a file inside an account folder to open its page in your browser. To download files or entire folders, select them, press **Ctrl+C**, open a local folder, and press **Ctrl+V**.
5. To upload local files or folders, copy them and paste into a cloud folder. You can also drag them into an open cloud folder. WindowCloud shows upload progress and a Cancel button. Wait for completion before exiting the app.

WindowCloud fetches the file list when you open a folder and streams file contents to File Explorer when you copy them. It does not cache cloud files or directory listings on disk. Account settings and tokens are encrypted with Windows DPAPI and stored in `%LOCALAPPDATA%\WindowCloud\accounts.dat`.

## OAuth setup

The public ZIP requires you to register an OAuth client for each provider you want to connect.

| Provider | Setup |
| --- | --- |
| Google Drive | In [Google Cloud](https://console.cloud.google.com/), enable the Google Drive API, configure the consent screen, and create a **Desktop app** OAuth client. Enter the desktop client's **Client ID** and **Client secret**. Add your account as a test user if the app is in Testing. Google classifies `drive` as a restricted scope, and refresh tokens issued in Testing typically expire after seven days. [Google documentation](https://developers.google.com/workspace/drive/api/guides/api-specific-auth) |
| OneDrive | In [Microsoft Entra](https://entra.microsoft.com/), register an app that supports personal and work or school accounts. Add the **Mobile and desktop applications** platform, set the redirect URI to `http://localhost/callback`, and add the delegated Microsoft Graph **Files.ReadWrite** permission. Enter the **Application (client) ID**. [Microsoft documentation](https://learn.microsoft.com/en-us/entra/identity-platform/reply-url) |
| Dropbox | In the [Dropbox App Console](https://www.dropbox.com/developers/apps), create a **Scoped access** app with **Full Dropbox** access. Add the redirect URI `http://127.0.0.1:49273/callback` and enable `account_info.read`, `files.metadata.read`, `files.content.read`, and `files.content.write`. Enter the **App key**. To connect another account, switch accounts on the Dropbox website before authorizing if needed. [Dropbox documentation](https://developers.dropbox.com/oauth-guide) |

If you own the registered clients, pass `-OAuthClients <path-to-json>` to the build script to bundle their configuration. WindowCloud also reads `oauth-clients.json` from the same folder as the executable. Connecting a provider listed in the file takes you directly to browser sign-in. Use public desktop OAuth clients whose configuration can be distributed with the app. OneDrive and Dropbox require only `clientId`.

Maintainers can set the repository Actions variable `WINDOWCLOUD_OAUTH_CLIENTS` to the JSON below. The workflow includes that configuration in the release ZIP. Public client registrations for WindowCloud are still pending, so the current public build asks for client details on its first connection.

```json
{
  "GoogleDrive": { "clientId": "...", "desktopSecret": "..." },
  "OneDrive": { "clientId": "..." },
  "Dropbox": { "clientId": "..." }
}
```

## File support

- Copy entire cloud folders to the PC, including nested files and empty folders. The File Explorer transfer format limits relative paths to 259 characters. An item that cannot be downloaded stops copy preparation with an error.
- Upload files and folders from the PC while preserving their hierarchy. Google Drive creates new objects with the supplied names. OneDrive and Dropbox rename duplicates. A zero-byte OneDrive upload with an existing name reports a conflict. Existing objects are preserved.
- Cancellation or failure stops the upload. Completed files and created folders remain in the cloud, and their counts appear in the transfer window. Pasting again creates another copy.
- Symbolic links and reparse points stop the upload with a message.
- Google Docs, Sheets, and Slides export as DOCX, XLSX, and PPTX respectively. Google Drawings export as PDF. [Google Drive API export limits](https://developers.google.com/workspace/drive/api/guides/manage-downloads)
- You may need to select the same account in your browser as in WindowCloud. Preview support depends on the provider and file format.

## Building from source

On Windows x64, install the .NET 10 SDK and Zig 0.15.2, then run `./build.ps1 -SelfContained`. The script writes a self-contained ZIP to `artifacts`. The Windows workflow builds and extracts the ZIP, verifies that the File Explorer DLL loads, and checks that the background app starts with the bundled .NET runtime. [GitHub Actions workflow](https://github.com/Frish07/WindowCloud/actions/workflows/windows.yml)

The background app, user interface, and cloud clients are written in C#. The native C++ module implements the File Explorer interfaces and forwards requests to the app through a named pipe scoped to the current user.
