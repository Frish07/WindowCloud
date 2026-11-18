using System.Buffers;
using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace WindowCloud;

internal sealed partial class Cloud
{
    public async Task<string> CreateFolder(Account account, string parent, string name, CancellationToken cancellation)
    {
        RequireWrite(account);
        ValidateName(name);
        var token = await Access(account.Id, cancellation);
        parent = parent.Length == 0 ? account.Identity.Root : parent;
        if (account.Provider == Provider.Dropbox)
        {
            var path = await DropboxPath(account, parent, name, token, cancellation);
            using var json = await DropboxJson("files/create_folder_v2", token, account.Identity.Namespace,
                new { path, autorename = true }, cancellation);
            return json.RootElement.GetProperty("metadata").Required("id");
        }
        var url = account.Provider == Provider.GoogleDrive ? Google + "files" : GraphParent(parent) + "/children";
        object metadata = account.Provider == Provider.GoogleDrive
            ? new { name, mimeType = "application/vnd.google-apps.folder", parents = new[] { parent } }
            : new Dictionary<string, object> { ["name"] = name, ["folder"] = new { },
                ["@microsoft.graph.conflictBehavior"] = "rename" };
        using var request = JsonRequest(HttpMethod.Post, url, metadata, token);
        using var response = await Send(request, cancellation);
        using var result = await ReadResult(response, cancellation);
        return result.RootElement.Required("id");
    }

    public async Task Upload(Account account, string parent, string path, Action<long, long> progress,
        CancellationToken cancellation)
    {
        RequireWrite(account);
        var name = Path.GetFileName(path);
        ValidateName(name);
        await using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
            65536, FileOptions.Asynchronous | FileOptions.SequentialScan);
        var token = await Access(account.Id, cancellation);
        parent = parent.Length == 0 ? account.Identity.Root : parent;
        switch (account.Provider)
        {
            case Provider.GoogleDrive:
                await GoogleUpload(file, parent, name, account.Id, token, progress, cancellation);
                break;
            case Provider.OneDrive:
                await GraphUpload(file, parent, name, token, progress, cancellation);
                break;
            case Provider.Dropbox:
                var remotePath = await DropboxPath(account, parent, name, token, cancellation);
                await DropboxUpload(file, remotePath, account, progress, cancellation);
                break;
        }
    }

    private async Task GoogleUpload(FileStream file, string parent, string name, Guid account, string token,
        Action<long, long> progress, CancellationToken cancellation)
    {
        using var start = JsonRequest(HttpMethod.Post,
            "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable",
            new { name, parents = new[] { parent } }, token);
        start.Headers.Add("X-Upload-Content-Type", "application/octet-stream");
        start.Headers.Add("X-Upload-Content-Length", file.Length.ToString(System.Globalization.CultureInfo.InvariantCulture));
        using var response = await Send(start, cancellation);
        EnsureSuccess(response);
        var session = SessionUrl(response.Headers.Location?.AbsoluteUri);
        await UploadRanges(file, session, account, google: true, progress, cancellation);
    }

    private async Task GraphUpload(FileStream file, string parent, string name, string token,
        Action<long, long> progress, CancellationToken cancellation)
    {
        // A zero-byte file has no byte ranges to send to an upload session.
        if (file.Length == 0)
        {
            using var empty = new HttpRequestMessage(HttpMethod.Put,
                GraphParent(parent) + ":/" + Esc(name) + ":/content?@microsoft.graph.conflictBehavior=fail");
            empty.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
            empty.Content = new ByteArrayContent([]);
            using var response = await Send(empty, cancellation);
            EnsureSuccess(response);
            progress(0, 0);
            return;
        }
        using var start = JsonRequest(HttpMethod.Post, GraphParent(parent) + ":/" + Esc(name) + ":/createUploadSession",
            new { item = new Dictionary<string, string> { ["@microsoft.graph.conflictBehavior"] = "rename" } }, token);
        using var created = await Send(start, cancellation);
        using var result = await ReadResult(created, cancellation);
        var session = SessionUrl(result.RootElement.Required("uploadUrl"));
        try { await UploadRanges(file, session, null, google: false, progress, cancellation); }
        catch
        {
            using var cleanup = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Delete, session);
                using var response = await Send(request, cleanup.Token);
            }
            catch (Exception error) when (error is HttpRequestException or OperationCanceledException) { }
            throw;
        }
    }

    private async Task UploadRanges(FileStream file, Uri session, Guid? account, bool google,
        Action<long, long> progress, CancellationToken cancellation)
    {
        const int chunkSize = 5 * 1024 * 1024; // Multiple of both 256 KiB and 320 KiB.
        var buffer = ArrayPool<byte>.Shared.Rent(chunkSize);
        try
        {
            long offset = 0;
            do
            {
                cancellation.ThrowIfCancellationRequested();
                var count = (int)Math.Min(chunkSize, file.Length - offset);
                await file.ReadExactlyAsync(buffer.AsMemory(0, count), cancellation);
                using var request = new HttpRequestMessage(HttpMethod.Put, session);
                if (account is { } id)
                    request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", await Access(id, cancellation));
                request.Content = new ByteArrayContent(buffer, 0, count);
                request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
                request.Content.Headers.ContentRange = count == 0 ? new ContentRangeHeaderValue(0)
                    : new ContentRangeHeaderValue(offset, offset + count - 1, file.Length);
                using var response = await Send(request, cancellation);
                offset += count;
                if (google && (int)response.StatusCode == 308)
                {
                    if (offset >= file.Length || !response.Headers.TryGetValues("Range", out var ranges) ||
                        ranges.SingleOrDefault() != $"bytes=0-{offset - 1}")
                        throw new IOException("Google Drive не подтвердил полученный диапазон файла.");
                }
                else if (!google && response.StatusCode == HttpStatusCode.Accepted)
                {
                    if (offset >= file.Length) throw new IOException("OneDrive не завершил загрузку.");
                    using var result = await ReadResult(response, cancellation);
                    var next = result.RootElement.GetProperty("nextExpectedRanges")[0].GetString();
                    if (next != $"{offset}-") throw new IOException("OneDrive запросил другой диапазон файла.");
                }
                else
                {
                    EnsureSuccess(response);
                    if (offset != file.Length) throw new IOException("Облако завершило загрузку раньше времени.");
                }
                progress(offset, file.Length);
            } while (offset < file.Length);
        }
        finally { ArrayPool<byte>.Shared.Return(buffer); }
    }

    private async Task DropboxUpload(FileStream file, string path, Account account,
        Action<long, long> progress, CancellationToken cancellation)
    {
        var commit = new { path, mode = "add", autorename = true, mute = false, strict_conflict = true };
        const int chunkSize = 4 * 1024 * 1024;
        var buffer = ArrayPool<byte>.Shared.Rent(chunkSize);
        try
        {
            long offset = 0;
            string? session = null;
            do
            {
                var count = (int)Math.Min(chunkSize, file.Length - offset);
                await file.ReadExactlyAsync(buffer.AsMemory(0, count), cancellation);
                string route;
                object args;
                if (file.Length <= chunkSize) { route = "files/upload"; args = commit; }
                else if (session is null) { route = "files/upload_session/start"; args = new { close = false }; }
                else
                {
                    var cursor = new { session_id = session, offset };
                    var final = offset + count == file.Length;
                    route = final ? "files/upload_session/finish" : "files/upload_session/append_v2";
                    args = final ? (object)new { cursor, commit } : new { cursor, close = false };
                }
                using var request = new HttpRequestMessage(HttpMethod.Post, DropboxContent + route);
                request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", await Access(account.Id, cancellation));
                request.Headers.Add("Dropbox-API-Arg", JsonSerializer.Serialize(args, AsciiJson));
                SetNamespace(request, account.Identity.Namespace);
                request.Content = new ByteArrayContent(buffer, 0, count);
                request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
                using var response = await Send(request, cancellation);
                EnsureSuccess(response);
                if (route == "files/upload_session/start")
                {
                    using var result = await ReadResult(response, cancellation);
                    session = result.RootElement.Required("session_id");
                }
                offset += count;
                progress(offset, file.Length);
            } while (offset < file.Length);
        }
        finally { ArrayPool<byte>.Shared.Return(buffer); }
    }

    private async Task<string> DropboxPath(Account account, string parent, string name, string token,
        CancellationToken cancellation)
    {
        if (parent.Length == 0) return "/" + name;
        using var json = await DropboxJson("files/get_metadata", token, account.Identity.Namespace,
            new { path = parent }, cancellation);
        return json.RootElement.Required("path_display").TrimEnd('/') + "/" + name;
    }

    private static string GraphParent(string parent) =>
        Graph + (parent == "root" ? "me/drive/root" : "me/drive/items/" + Esc(parent));

    private static HttpRequestMessage JsonRequest(HttpMethod method, string url, object body, string token)
    {
        var request = new HttpRequestMessage(method, url);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        request.Content = new StringContent(JsonSerializer.Serialize(body), Encoding.UTF8, "application/json");
        return request;
    }

    private static Uri SessionUrl(string? value)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || uri.Scheme != "https" || uri.UserInfo.Length != 0)
            throw new IOException("Недопустимый адрес сеанса загрузки.");
        return uri;
    }

    private static async Task<JsonDocument> ReadResult(HttpResponseMessage response, CancellationToken cancellation)
    {
        EnsureSuccess(response);
        return await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(cancellation), cancellationToken: cancellation);
    }

    private static void EnsureSuccess(HttpResponseMessage response)
    {
        if (!response.IsSuccessStatusCode) throw ApiError(response.StatusCode);
    }

    internal static void RequireWrite(Account account)
    {
        if (!account.CanWrite) throw new IOException("Повторно подключите аккаунт, чтобы разрешить загрузку файлов.");
    }

    private static void ValidateName(string name)
    {
        if (string.IsNullOrWhiteSpace(name) || name is "." or ".." || name.IndexOfAny(['/', '\\', '\0']) >= 0)
            throw new IOException("Недопустимое имя файла или папки.");
    }
}
