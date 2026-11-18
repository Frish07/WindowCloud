using System.Net;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using WindowCloud;

var testFolder = Path.Combine(Path.GetTempPath(), "WindowCloud-tests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(testFolder);
try
{
    var path = Path.Combine(testFolder, "данные.bin");
    var bytes = new byte[10 * 1024 * 1024 + 37];
    for (var i = 0; i < bytes.Length; i++) bytes[i] = (byte)(i % 251);
    await File.WriteAllBytesAsync(path, bytes);
    foreach (var provider in Enum.GetValues<Provider>())
    {
        var accounts = new Accounts(Path.Combine(testFolder, provider + ".dat"));
        var account = accounts.Save(provider, new Identity("test", "test", provider == Provider.Dropbox ? "" : "root", "namespace"),
            new ClientRegistration("client", null), new Token("access", "refresh", DateTimeOffset.UtcNow.AddHours(1)));
        using var handler = new CloudHandler(provider);
        using var http = new HttpClient(handler);
        var cloud = new Cloud(accounts, http);
        var reported = -1L;
        await cloud.Upload(account, "", path, (done, _) => reported = done, CancellationToken.None);
        Check(reported == bytes.Length, "upload reports completion");
        Check(handler.Uploaded.ToArray().SequenceEqual(bytes), "chunked upload preserves bytes");
        Check(handler.Commits == 1, "one final commit");
        Console.WriteLine(provider + " chunked upload passed");

        handler.Reset();
        var empty = Path.Combine(testFolder, "empty.txt");
        File.WriteAllBytes(empty, []);
        await cloud.Upload(account, "", empty, (_, _) => { }, CancellationToken.None);
        Check(handler.Uploaded.Length == 0 && handler.Commits == 1, "empty file");

        handler.Reset();
        using (var cancel = new CancellationTokenSource())
        {
            try
            {
                await cloud.Upload(account, "", path, (_, _) => cancel.Cancel(), cancel.Token);
                throw new Exception("Expected upload cancellation.");
            }
            catch (OperationCanceledException) { }
            Check(handler.Commits == 0, "cancelled upload stays uncommitted");
        }
        handler.Reset();
        handler.FailSecondChunk = true;
        try
        {
            await cloud.Upload(account, "", path, (_, _) => { }, CancellationToken.None);
            throw new Exception("Expected upload failure.");
        }
        catch (IOException) { }
        Check(handler.Commits == 0, "failed upload stays uncommitted");

        handler.Reset();
        var tree = Path.Combine(testFolder, provider + "-tree");
        Directory.CreateDirectory(Path.Combine(tree, "empty"));
        Directory.CreateDirectory(Path.Combine(tree, "nested"));
        await File.WriteAllTextAsync(Path.Combine(tree, "nested", "note.txt"), "folder content");
        var operation = new UploadOperation(cloud, account);
        await operation.Run("", [tree], new InlineProgress(), CancellationToken.None);
        Check(operation.Files == 1 && operation.Folders == 3, "recursive upload includes empty folders");
        Check(handler.Folders.Count == 3, "three remote directories created");
        Check(handler.Uploaded.ToArray().SequenceEqual(Encoding.UTF8.GetBytes("folder content")), "recursive file contents");
        Console.WriteLine(provider + " empty files, cancellation, failure and folder upload passed");

        var readonlyAccount = account with { CanWrite = false };
        try { await cloud.CreateFolder(readonlyAccount, "", "old", CancellationToken.None); throw new Exception("Expected reauthentication."); }
        catch (IOException) { }
        var legacy = JsonSerializer.Serialize(account);
        legacy = legacy.Replace(",\"CanWrite\":true", "");
        Check(!JsonSerializer.Deserialize<Account>(legacy)!.CanWrite, "legacy accounts need write consent");

        var received = false;
        using var server = new ExplorerServer(accounts, cloud, (selectedAccount, parent, paths, location) =>
        {
            Check(selectedAccount.Id == account.Id && parent == "destination", "IPC upload destination");
            Check(paths.SequenceEqual(new[] { path }) && location.SequenceEqual(new byte[] { 0, 0 }), "IPC upload paths");
            received = true;
            return Task.CompletedTask;
        });
        using var pipe = new NamedPipeClientStream(".", server.PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        await pipe.ConnectAsync(5000);
        using var writer = new BinaryWriter(pipe, Encoding.Unicode, leaveOpen: true);
        writer.Write((byte)5);
        writer.Write(account.Id.ToByteArray());
        var parentBytes = Encoding.Unicode.GetBytes("destination");
        writer.Write(parentBytes.Length);
        writer.Write(0L);
        writer.Write(parentBytes);
        writer.Write(1);
        var pathBytes = Encoding.Unicode.GetBytes(path);
        writer.Write((ushort)pathBytes.Length);
        writer.Write(pathBytes);
        writer.Write(2);
        writer.Write((ushort)0);
        writer.Flush();
        var acknowledgement = new byte[1];
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await pipe.ReadExactlyAsync(acknowledgement, timeout.Token);
        Check(received && acknowledgement[0] == 0, "background upload IPC accepted");
    }
}
finally { Directory.Delete(testFolder, recursive: true); }

static void Check(bool condition, string label)
{
    if (!condition) throw new Exception("Failed check. " + label);
}

sealed class InlineProgress : IProgress<(string, long, long)>
{
    public void Report((string, long, long) value) { }
}

sealed class CloudHandler(Provider provider) : HttpMessageHandler
{
    public MemoryStream Uploaded { get; } = new();
    public List<string> Folders { get; } = [];
    public int Commits { get; private set; }
    public bool FailSecondChunk { get; set; }
    private int chunks;
    private string lastPath = "";
    public void Reset()
    {
        Uploaded.SetLength(0);
        Folders.Clear();
        Commits = chunks = 0;
        FailSecondChunk = false;
    }
    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        var uri = request.RequestUri!;
        if (request.Method == HttpMethod.Delete) return Json("{}");
        if (uri.Host == "session.test")
        {
            if (provider == Provider.OneDrive && request.Headers.Authorization is not null)
                throw new Exception("Graph session must use its preauthenticated URL.");
            var range = request.Content!.Headers.ContentRange!;
            if ((range.From ?? 0) != Uploaded.Length) throw new Exception("Wrong upload offset.");
            if (FailSecondChunk && chunks++ == 1) return Json("{}", HttpStatusCode.ServiceUnavailable);
            await request.Content.CopyToAsync(Uploaded, cancellation);
            if (Uploaded.Length == range.Length)
            {
                Commits++;
                return Json("{\"id\":\"file\"}", HttpStatusCode.Created);
            }
            if (provider == Provider.GoogleDrive)
            {
                var incomplete = Json("{}", (HttpStatusCode)308);
                incomplete.Headers.TryAddWithoutValidation("Range", $"bytes=0-{Uploaded.Length - 1}");
                return incomplete;
            }
            return Json($"{{\"nextExpectedRanges\":[\"{Uploaded.Length}-\"]}}", HttpStatusCode.Accepted);
        }
        var body = request.Content is null || uri.Host == "content.dropboxapi.com"
            ? "" : await request.Content.ReadAsStringAsync(cancellation);
        if (uri.AbsolutePath.EndsWith("createUploadSession"))
        {
            if (!body.Contains("rename")) throw new Exception("Expected conflict handling.");
            return Json("{\"uploadUrl\":\"https://session.test/upload\"}");
        }
        if (uri.AbsolutePath.Contains("/upload/drive/"))
        {
            if (!body.Contains("parents")) throw new Exception("Missing Google parent.");
            var response = Json("{}");
            response.Headers.Location = new Uri("https://session.test/upload");
            return response;
        }
        if (uri.AbsolutePath.EndsWith("/content"))
        {
            if (request.Content!.Headers.ContentLength != 0 || !uri.Query.Contains("fail")) throw new Exception("Unsafe empty Graph upload.");
            Commits++;
            return Json("{\"id\":\"empty\"}");
        }
        if (uri.Host == "content.dropboxapi.com")
        {
            using var args = JsonDocument.Parse(request.Headers.GetValues("Dropbox-API-Arg").Single());
            var root = args.RootElement;
            if (root.TryGetProperty("cursor", out var cursor) && cursor.GetProperty("offset").GetInt64() != Uploaded.Length)
                throw new Exception("Incorrect Dropbox cursor.");
            if (FailSecondChunk && chunks++ == 1) return Json("{}", HttpStatusCode.ServiceUnavailable);
            await request.Content!.CopyToAsync(Uploaded, cancellation);
            if (uri.AbsolutePath.EndsWith("/start")) return Json("{\"session_id\":\"session\"}");
            if (uri.AbsolutePath.EndsWith("/finish") || uri.AbsolutePath.EndsWith("/upload"))
            {
                var commit = root.TryGetProperty("commit", out var value) ? value : root;
                if (!commit.GetProperty("autorename").GetBoolean() || commit.GetProperty("mode").GetString() != "add")
                    throw new Exception("Dropbox must preserve existing files.");
                Commits++;
            }
            return Json("{}");
        }
        if (uri.AbsolutePath.EndsWith("get_metadata")) return Json(JsonSerializer.Serialize(new { path_display = lastPath }));
        using var metadata = JsonDocument.Parse(body);
        string name;
        if (provider == Provider.Dropbox)
        {
            lastPath = metadata.RootElement.GetProperty("path").GetString()!;
            name = lastPath.Split('/').Last();
        }
        else name = metadata.RootElement.GetProperty("name").GetString()!;
        Folders.Add(name);
        var id = "folder-" + Folders.Count;
        return Json(provider == Provider.Dropbox ? JsonSerializer.Serialize(new { metadata = new { id } }) :
            JsonSerializer.Serialize(new { id }));
    }
    private static HttpResponseMessage Json(string json, HttpStatusCode status = HttpStatusCode.OK) =>
        new(status) { Content = new StringContent(json, Encoding.UTF8, "application/json") };
}
