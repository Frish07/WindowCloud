using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace WindowCloud;

internal static class OAuth
{
    public const int DropboxPort = 49273;

    internal static readonly HttpClient Http = new(new SocketsHttpHandler
    {
        ConnectTimeout = TimeSpan.FromSeconds(10),
        PooledConnectionIdleTimeout = TimeSpan.FromSeconds(45),
        PooledConnectionLifetime = TimeSpan.FromMinutes(10),
        AllowAutoRedirect = false
    }) { Timeout = Timeout.InfiniteTimeSpan };

    public static async Task<Token> SignIn(Provider provider, ClientRegistration client, CancellationToken cancellation)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(TimeSpan.FromMinutes(4));
        var listener = new TcpListener(IPAddress.Loopback, provider == Provider.Dropbox ? DropboxPort : 0);
        listener.Server.ExclusiveAddressUse = true;
        listener.Start();
        try
        {
            var state = RandomString();
            var verifier = RandomString();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            var host = provider == Provider.OneDrive ? "localhost" : "127.0.0.1";
            var redirect = $"http://{host}:{port}/callback";
            var parameters = new Dictionary<string, string>
            {
                ["client_id"] = client.Id,
                ["redirect_uri"] = redirect,
                ["response_type"] = "code",
                ["state"] = state,
                ["code_challenge_method"] = "S256",
                ["code_challenge"] = Base64Url(SHA256.HashData(Encoding.ASCII.GetBytes(verifier))),
                ["scope"] = Scope(provider)
            };
            if (provider == Provider.GoogleDrive)
            {
                parameters["access_type"] = "offline";
                parameters["prompt"] = "consent select_account";
            }
            if (provider == Provider.OneDrive) parameters["prompt"] = "select_account";
            if (provider == Provider.Dropbox)
            {
                parameters["token_access_type"] = "offline";
                parameters["force_reapprove"] = "true";
            }
            OpenBrowser(Endpoint(provider, token: false) + "?" +
                string.Join("&", parameters.Select(p => $"{p.Key}={Uri.EscapeDataString(p.Value)}")));

            while (true)
            {
                using var connection = await listener.AcceptTcpClientAsync(deadline.Token);
                using var requestDeadline = CancellationTokenSource.CreateLinkedTokenSource(deadline.Token);
                requestDeadline.CancelAfter(TimeSpan.FromSeconds(5));
                var stream = connection.GetStream();
                Dictionary<string, string>? response;
                try { response = await ReadCallback(stream, redirect, requestDeadline.Token); }
                catch (IOException) { continue; }
                catch (OperationCanceledException) when (!deadline.IsCancellationRequested) { continue; }
                if (response is null || !response.TryGetValue("state", out var received) || !string.Equals(received, state, StringComparison.Ordinal))
                {
                    await ReplySafely(stream, 400, "Ожидается ответ от страницы входа.", requestDeadline.Token);
                    continue;
                }
                if (!response.TryGetValue("code", out var code) || string.IsNullOrWhiteSpace(code))
                {
                    await ReplySafely(stream, 400, "Вход не завершён. Вернитесь в WindowCloud.", requestDeadline.Token);
                    throw new IOException("Провайдер отклонил вход или вход был отменён.");
                }
                await ReplySafely(stream, 200, "Ответ получен. Вернитесь в WindowCloud.", requestDeadline.Token);
                return await Exchange(provider, client, new()
                {
                    ["grant_type"] = "authorization_code", ["code"] = code,
                    ["code_verifier"] = verifier, ["redirect_uri"] = redirect
                }, null, deadline.Token);
            }
        }
        finally { listener.Stop(); }
    }

    public static Task<Token> Refresh(Account account, CancellationToken cancellation) =>
        Exchange(account.Provider, account.Client, new()
        {
            ["grant_type"] = "refresh_token", ["refresh_token"] = account.Token.Refresh
        }, account.Token.Refresh, cancellation);

    private static async Task<Token> Exchange(Provider provider, ClientRegistration client,
        Dictionary<string, string> fields, string? previousRefresh, CancellationToken cancellation)
    {
        fields["client_id"] = client.Id;
        if (provider == Provider.GoogleDrive && !string.IsNullOrWhiteSpace(client.DesktopSecret))
            fields["client_secret"] = client.DesktopSecret;
        using var form = new FormUrlEncodedContent(fields);
        using var request = new HttpRequestMessage(HttpMethod.Post, Endpoint(provider, token: true)) { Content = form };
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        timeout.CancelAfter(TimeSpan.FromSeconds(20));
        using var response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, timeout.Token);
        if (!response.IsSuccessStatusCode)
            throw new IOException($"{provider.Label()}: не удалось получить токен (HTTP {(int)response.StatusCode}). Проверьте настройки OAuth или войдите повторно.");
        using var json = await JsonDocument.ParseAsync(await response.Content.ReadAsStreamAsync(timeout.Token), cancellationToken: timeout.Token);
        var root = json.RootElement;
        var refresh = root.Text("refresh_token") ?? previousRefresh;
        if (string.IsNullOrWhiteSpace(refresh))
            throw new IOException("Провайдер не выдал токен обновления. Повторите вход и подтвердите запрошенный доступ.");
        return new Token(root.Required("access_token"), refresh,
            DateTimeOffset.UtcNow.AddSeconds(root.GetProperty("expires_in").GetInt32()));
    }

    private static async Task<Dictionary<string, string>?> ReadCallback(Stream stream, string redirect, CancellationToken cancellation)
    {
        var buffer = new byte[8192];
        var used = 0;
        while (used < buffer.Length)
        {
            var count = await stream.ReadAsync(buffer.AsMemory(used), cancellation);
            if (count == 0) return null;
            used += count;
            var end = buffer.AsSpan(0, used).IndexOf("\r\n\r\n"u8);
            if (end < 0) continue;
            var lines = Encoding.ASCII.GetString(buffer, 0, end).Split("\r\n");
            var first = lines[0].Split(' ');
            var expected = new Uri(redirect);
            if (first.Length != 3 || first[0] != "GET" ||
                !first[1].StartsWith(expected.AbsolutePath + "?", StringComparison.Ordinal) ||
                !lines.Any(line => line.StartsWith("Host:", StringComparison.OrdinalIgnoreCase) &&
                    string.Equals(line[5..].Trim(), expected.Authority, StringComparison.OrdinalIgnoreCase))) return null;
            var query = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var pair in first[1][(expected.AbsolutePath.Length + 1)..].Split('&'))
            {
                var separator = pair.IndexOf('=');
                if (separator <= 0 || !query.TryAdd(WebUtility.UrlDecode(pair[..separator]), WebUtility.UrlDecode(pair[(separator + 1)..]))) return null;
            }
            return query;
        }
        return null;
    }

    private static async Task ReplySafely(Stream stream, int status, string message, CancellationToken cancellation)
    {
        var body = Encoding.UTF8.GetBytes($"<!doctype html><meta charset=utf-8><title>WindowCloud</title><p>{message}</p>");
        var headers = Encoding.ASCII.GetBytes($"HTTP/1.1 {status} {(status == 200 ? "OK" : "Bad Request")}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {body.Length}\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n");
        try
        {
            await stream.WriteAsync(headers, cancellation);
            await stream.WriteAsync(body, cancellation);
        }
        catch (IOException) { }
        catch (OperationCanceledException) { }
    }

    public static void OpenBrowser(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || uri.Scheme != Uri.UriSchemeHttps)
            throw new IOException("Провайдер не вернул ссылку для открытия в браузере.");
        using var process = Process.Start(new ProcessStartInfo(uri.AbsoluteUri) { UseShellExecute = true });
    }

    private static string Endpoint(Provider provider, bool token) => provider switch
    {
        Provider.GoogleDrive => token ? "https://oauth2.googleapis.com/token" : "https://accounts.google.com/o/oauth2/v2/auth",
        Provider.OneDrive => "https://login.microsoftonline.com/common/oauth2/v2.0/" + (token ? "token" : "authorize"),
        Provider.Dropbox => token ? "https://api.dropboxapi.com/oauth2/token" : "https://www.dropbox.com/oauth2/authorize",
        _ => throw new ArgumentOutOfRangeException(nameof(provider))
    };

    private static string Scope(Provider provider) => provider switch
    {
        Provider.GoogleDrive => "https://www.googleapis.com/auth/drive.readonly",
        Provider.OneDrive => "https://graph.microsoft.com/Files.Read offline_access",
        Provider.Dropbox => "account_info.read files.metadata.read files.content.read",
        _ => throw new ArgumentOutOfRangeException(nameof(provider))
    };

    private static string RandomString() => Base64Url(RandomNumberGenerator.GetBytes(32));
    private static string Base64Url(byte[] bytes) => Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');
}

internal static class JsonValues
{
    public static string? Text(this JsonElement value, string property) =>
        value.TryGetProperty(property, out var result) && result.ValueKind == JsonValueKind.String ? result.GetString() : null;
    public static string Required(this JsonElement value, string property) =>
        value.Text(property) ?? throw new InvalidDataException($"В ответе отсутствует поле {property}.");
    public static bool Has(this JsonElement value, string property) =>
        value.TryGetProperty(property, out var result) && result.ValueKind != JsonValueKind.Null;
    public static long? Size(this JsonElement value) =>
        value.TryGetProperty("size", out var size) ? size.ValueKind == JsonValueKind.Number ? size.GetInt64()
            : long.TryParse(size.GetString(), out var number) ? number : null : null;
}
