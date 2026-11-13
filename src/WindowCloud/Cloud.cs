using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace WindowCloud;

internal sealed record Item(string Id, string Name, bool Folder, bool Copyable, long? Size,
    DateTimeOffset? Modified, string? CopyName = null);

internal sealed class Cloud(Accounts accounts)
{
    private const string Google = "https://www.googleapis.com/drive/v3/";
    private const string Graph = "https://graph.microsoft.com/v1.0/";
    private const string Dropbox = "https://api.dropboxapi.com/2/";
    private const string DropboxContent = "https://content.dropboxapi.com/2/";
    private static readonly JsonSerializerOptions AsciiJson = new();
    private readonly SemaphoreSlim refreshGate = new(1, 1);

    public async Task<Identity> Identify(Provider provider, Token token, CancellationToken cancellation)
    {
        using var json = provider switch
        {
            Provider.GoogleDrive => await GetJson(Google + "about?fields=user(permissionId,emailAddress,displayName)", token.Access, cancellation),
            Provider.OneDrive => await GetJson(Graph + "me/drive?$select=id,owner", token.Access, cancellation),
            _ => await DropboxJson("users/get_current_account", token.Access, null, null, cancellation)
        };
        var root = json.RootElement;
        switch (provider)
        {
            case Provider.GoogleDrive:
                var google = root.GetProperty("user");
                return new Identity(google.Required("permissionId"), google.Text("emailAddress") ?? google.Required("displayName"), "root");
            case Provider.OneDrive:
                var drive = root.Required("id");
                var owner = root.TryGetProperty("owner", out var ownerValue) && ownerValue.TryGetProperty("user", out var user)
                    ? user.Text("email") ?? user.Text("displayName") : null;
                return new Identity(drive, owner ?? drive, "root");
            default:
                var ns = root.GetProperty("root_info").Required("root_namespace_id");
                return new Identity(root.Required("account_id"), root.Required("email"), "", ns);
        }
    }

    public async Task<List<Item>> List(Account account, string parent, CancellationToken cancellation)
    {
        var items = new List<Item>();
        var token = await Access(account.Id, cancellation);
        parent = parent.Length == 0 ? account.Identity.Root : parent;
        switch (account.Provider)
        {
            case Provider.GoogleDrive:
                string? page = null;
                do
                {
                    var query = Esc($"'{parent.Replace("'", "\\'")}' in parents and trashed = false");
                    using var json = await GetJson(Google + "files?pageSize=1000&q=" + query +
                        "&fields=nextPageToken,files(id,name,mimeType,size,modifiedTime,capabilities(canDownload),shortcutDetails(targetId))" +
                        (page is null ? "" : "&pageToken=" + Esc(page)), token, cancellation);
                    foreach (var value in json.RootElement.GetProperty("files").EnumerateArray()) items.Add(GoogleItem(value));
                    page = json.RootElement.Text("nextPageToken");
                } while (!string.IsNullOrEmpty(page));
                break;
            case Provider.OneDrive:
                string? next = Graph + (parent == "root" ? "me/drive/root" : "me/drive/items/" + Esc(parent)) +
                    "/children?$select=id,name,folder,file,size,lastModifiedDateTime,remoteItem,package&$top=200";
                while (next is not null)
                {
                    if (!Uri.TryCreate(next, UriKind.Absolute, out var uri) || uri.Scheme != "https" ||
                        uri.Host != "graph.microsoft.com" || !uri.IsDefaultPort || uri.UserInfo.Length != 0)
                        throw new InvalidDataException("Недопустимая ссылка на страницу Microsoft Graph.");
                    using var json = await GetJson(next, token, cancellation);
                    foreach (var value in json.RootElement.GetProperty("value").EnumerateArray()) items.Add(GraphItem(value));
                    next = json.RootElement.Text("@odata.nextLink");
                }
                break;
            case Provider.Dropbox:
                string? cursor = null;
                do
                {
                    var route = cursor is null ? "files/list_folder" : "files/list_folder/continue";
                    object args = cursor is null ? new { path = parent, limit = 500 } : new { cursor };
                    using var json = await DropboxJson(route, token, account.Identity.Namespace, args, cancellation);
                    foreach (var value in json.RootElement.GetProperty("entries").EnumerateArray())
                    {
                        var folder = value.Required(".tag") == "folder";
                        var copy = !folder && (!value.TryGetProperty("is_downloadable", out var downloadable) || downloadable.GetBoolean());
                        items.Add(new Item(value.Required("id"), value.Required("name"), folder, copy, value.Size(),
                            Date(value, "server_modified")));
                    }
                    cursor = json.RootElement.GetProperty("has_more").GetBoolean() ? json.RootElement.Required("cursor") : null;
                } while (cursor is not null);
                break;
        }
        return items;
    }

    public async Task<string> BrowserLink(Account account, string id, CancellationToken cancellation)
    {
        var token = await Access(account.Id, cancellation);
        switch (account.Provider)
        {
            case Provider.GoogleDrive:
                using (var json = await GetJson(Google + "files/" + Esc(id) + "?fields=webViewLink,shortcutDetails(targetId)", token, cancellation))
                {
                    var file = json.RootElement;
                    if (file.TryGetProperty("shortcutDetails", out var shortcut)) id = shortcut.Required("targetId");
                    return file.Text("webViewLink") ?? $"https://drive.google.com/open?id={Esc(id)}";
                }
            case Provider.OneDrive:
                using (var json = await GetJson(Graph + "me/drive/items/" + Esc(id) + "?$select=webUrl,remoteItem", token, cancellation))
                {
                    var file = json.RootElement;
                    return file.Text("webUrl") ?? file.GetProperty("remoteItem").Required("webUrl");
                }
            default:
                using (var json = await DropboxJson("files/get_metadata", token, account.Identity.Namespace, new { path = id }, cancellation))
                {
                    var path = json.RootElement.Required("path_display");
                    var slash = path.LastIndexOf('/');
                    var directory = string.Join("/", path[..slash].Split('/').Select(Esc));
                    return $"https://www.dropbox.com/home{directory}?preview={Esc(path[(slash + 1)..])}";
                }
        }
    }

    public async Task<HttpResponseMessage> Download(Account account, string id, long offset, CancellationToken cancellation)
    {
        var token = await Access(account.Id, cancellation);
        using var request = account.Provider switch
        {
            Provider.GoogleDrive => await GoogleDownload(id, token, cancellation),
            Provider.OneDrive => new HttpRequestMessage(HttpMethod.Get, Graph + "me/drive/items/" + Esc(id) + "/content"),
            _ => new HttpRequestMessage(HttpMethod.Post, DropboxContent + "files/download")
        };
        if (account.Provider == Provider.Dropbox)
        {
            request.Headers.Add("Dropbox-API-Arg", JsonSerializer.Serialize(new { path = id }, AsciiJson));
            SetNamespace(request, account.Identity.Namespace);
        }
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        if (offset > 0) request.Headers.Range = new RangeHeaderValue(offset, null);
        var response = await Send(request, cancellation);
        if (account.Provider == Provider.OneDrive && (int)response.StatusCode is >= 300 and < 400)
        {
            var location = response.Headers.Location;
            response.Dispose();
            if (location is null || !location.IsAbsoluteUri || location.Scheme != "https" || location.UserInfo.Length != 0)
                throw new IOException("OneDrive вернул недопустимую ссылку для скачивания.");
            using var redirect = new HttpRequestMessage(HttpMethod.Get, location);
            redirect.Headers.Range = request.Headers.Range;
            response = await Send(redirect, cancellation);
        }
        if (!response.IsSuccessStatusCode)
        {
            var code = response.StatusCode;
            response.Dispose();
            throw ApiError(code);
        }
        return response;
    }

    private static async Task<HttpRequestMessage> GoogleDownload(string id, string token, CancellationToken cancellation)
    {
        var url = Google + "files/" + Esc(id);
        using var json = await GetJson(url + "?fields=mimeType", token, cancellation);
        var export = GoogleExport(json.RootElement.Required("mimeType"));
        return new HttpRequestMessage(HttpMethod.Get, export is null ? url + "?alt=media" :
            url + "/export?mimeType=" + Esc(export.Value.Mime));
    }

    private async Task<string> Access(Guid id, CancellationToken cancellation)
    {
        var account = accounts.Find(id) ?? throw new IOException("Аккаунт отключён.");
        if (account.Token.Expires > DateTimeOffset.UtcNow.AddMinutes(2)) return account.Token.Access;
        await refreshGate.WaitAsync(cancellation);
        try
        {
            account = accounts.Find(id) ?? throw new IOException("Аккаунт отключён.");
            if (account.Token.Expires > DateTimeOffset.UtcNow.AddMinutes(2)) return account.Token.Access;
            var token = await OAuth.Refresh(account, cancellation);
            return accounts.UpdateToken(account, token).Token.Access;
        }
        finally { refreshGate.Release(); }
    }

    private static Item GoogleItem(JsonElement value)
    {
        var name = value.Required("name");
        var mime = value.Required("mimeType");
        var folder = mime == "application/vnd.google-apps.folder";
        var export = GoogleExport(mime);
        var native = mime.StartsWith("application/vnd.google-apps.", StringComparison.Ordinal);
        var permitted = !value.TryGetProperty("capabilities", out var capabilities) ||
            !capabilities.TryGetProperty("canDownload", out var canDownload) || canDownload.GetBoolean();
        return new Item(value.Required("id"), name, folder, !folder && permitted && (!native || export is not null),
            native ? null : value.Size(), Date(value, "modifiedTime"), export is null ? null : name + export.Value.Extension);
    }

    private static (string Extension, string Mime)? GoogleExport(string mime) => mime switch
    {
        "application/vnd.google-apps.document" => (".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"),
        "application/vnd.google-apps.spreadsheet" => (".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"),
        "application/vnd.google-apps.presentation" => (".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"),
        "application/vnd.google-apps.drawing" => (".pdf", "application/pdf"),
        _ => null
    };

    private static Item GraphItem(JsonElement value)
    {
        var folder = value.Has("folder");
        return new Item(value.Required("id"), value.Required("name"), folder,
            !folder && value.Has("file") && !value.Has("remoteItem"), value.Size(), Date(value, "lastModifiedDateTime"));
    }

    private static DateTimeOffset? Date(JsonElement value, string field) =>
        DateTimeOffset.TryParse(value.Text(field), out var date) ? date : null;

    private static async Task<JsonDocument> GetJson(string url, string token, CancellationToken cancellation)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        using var response = await Send(request, cancellation);
        if (!response.IsSuccessStatusCode) throw ApiError(response.StatusCode);
        return await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(cancellation), cancellationToken: cancellation);
    }

    private static async Task<JsonDocument> DropboxJson(string route, string token, string? ns, object? args,
        CancellationToken cancellation)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, Dropbox + route);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        SetNamespace(request, ns);
        if (args is not null) request.Content = new StringContent(JsonSerializer.Serialize(args), Encoding.UTF8, "application/json");
        using var response = await Send(request, cancellation);
        if (!response.IsSuccessStatusCode) throw ApiError(response.StatusCode);
        return await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(cancellation), cancellationToken: cancellation);
    }

    private static void SetNamespace(HttpRequestMessage request, string? ns)
    {
        if (!string.IsNullOrEmpty(ns))
            request.Headers.Add("Dropbox-API-Path-Root", JsonSerializer.Serialize(new Dictionary<string, string>
            { [".tag"] = "root", ["root"] = ns }, AsciiJson));
    }

    private static Task<HttpResponseMessage> Send(HttpRequestMessage request, CancellationToken cancellation) =>
        OAuth.Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellation);
    private static IOException ApiError(HttpStatusCode status) => new(status switch
    {
        HttpStatusCode.Unauthorized => "Сеанс истёк. Подключите аккаунт заново.",
        HttpStatusCode.Forbidden => "Провайдер запретил доступ к этому объекту.",
        HttpStatusCode.TooManyRequests => "Слишком много запросов к облаку. Повторите позднее.",
        _ => $"Облако вернуло HTTP {(int)status}."
    });
    private static string Esc(string value) => Uri.EscapeDataString(value);
}
