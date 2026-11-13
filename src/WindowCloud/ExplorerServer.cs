using System.Buffers;
using System.IO.Pipes;
using System.Net;
using System.Text;

namespace WindowCloud;

internal sealed class ExplorerServer : IDisposable
{
    private readonly Accounts accounts;
    private readonly Cloud cloud;
    private readonly CancellationTokenSource stop = new();
    private readonly SemaphoreSlim slots = new(16, 16);
    public string PipeName { get; } = "WindowCloud-" + Guid.NewGuid().ToString("N");

    public ExplorerServer(Accounts accounts, Cloud cloud)
    {
        this.accounts = accounts;
        this.cloud = cloud;
        _ = Task.Run(Accept);
    }

    private async Task Accept()
    {
        var cancellation = stop.Token;
        while (!cancellation.IsCancellationRequested)
        {
            await slots.WaitAsync(cancellation);
            NamedPipeServerStream? pipe = null;
            try
            {
                pipe = new NamedPipeServerStream(PipeName, PipeDirection.InOut, 16, PipeTransmissionMode.Byte,
                    PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly, 8192, 65536);
                await pipe.WaitForConnectionAsync(cancellation);
                _ = Serve(pipe, cancellation);
                pipe = null;
            }
            catch (OperationCanceledException) { return; }
            catch (IOException) { await Task.Delay(250, cancellation); }
            finally
            {
                if (pipe is not null)
                {
                    pipe.Dispose();
                    slots.Release();
                }
            }
        }
    }

    private async Task Serve(NamedPipeServerStream pipe, CancellationToken cancellation)
    {
        using var connection = pipe;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        var responding = false;
        try
        {
            timeout.CancelAfter(TimeSpan.FromSeconds(15));
            var token = timeout.Token;
            var header = new byte[29];
            await pipe.ReadExactlyAsync(header, token);
            var command = header[0];
            var account = accounts.Find(new Guid(header.AsSpan(1, 16))) ?? throw new IOException("Аккаунт отключён.");
            var length = BitConverter.ToInt32(header, 17);
            var offset = BitConverter.ToInt64(header, 21);
            if (length < 0 || length > 4096 || (length & 1) != 0 || offset < 0)
                throw new InvalidDataException();
            var bytes = new byte[length];
            await pipe.ReadExactlyAsync(bytes, token);
            var id = Encoding.Unicode.GetString(bytes);

            switch (command)
            {
                case 1:
                    var items = await cloud.List(account, id, token);
                    using (var output = new MemoryStream())
                    {
                        using var writer = new BinaryWriter(output, Encoding.Unicode, leaveOpen: true);
                        writer.Write((byte)0);
                        writer.Write(items.Count);
                        foreach (var item in items)
                        {
                            writer.Write((byte)((item.Folder ? 1 : 0) | (item.Copyable ? 2 : 0)));
                            writer.Write(item.Size ?? -1);
                            writer.Write(item.Modified is { } modified && modified.Year >= 1601 ? modified.ToFileTime() : 0);
                            WriteText(writer, item.Id);
                            WriteText(writer, item.Name);
                            WriteText(writer, item.CopyName ?? item.Name);
                        }
                        responding = true;
                        await pipe.WriteAsync(output.GetBuffer().AsMemory(0, (int)output.Length), token);
                    }
                    break;
                case 2:
                    using (var response = await cloud.Download(account, id, offset, token))
                    {
                        var partial = response.StatusCode == HttpStatusCode.PartialContent;
                        var range = response.Content.Headers.ContentRange;
                        if (partial && range?.From != offset) throw new IOException("Облако вернуло неверный диапазон файла.");
                        var total = partial ? range?.Length : response.Content.Headers.ContentLength;
                        await using var stream = await response.Content.ReadAsStreamAsync(token);
                        var buffer = ArrayPool<byte>.Shared.Rent(65540);
                        try
                        {
                            if (!partial && offset > 0)
                                await Skip(stream, offset, buffer, timeout);
                            responding = true;
                            await pipe.WriteAsync(new byte[] { 0 }, token);
                            await pipe.WriteAsync(BitConverter.GetBytes(total ?? -1L), token);
                            long transferred = offset;
                            int count;
                            do
                            {
                                timeout.CancelAfter(TimeSpan.FromSeconds(30));
                                count = await stream.ReadAsync(buffer.AsMemory(4, 65536), token);
                                transferred += count;
                                if (count == 0 && total is { } size && transferred != size)
                                    throw new EndOfStreamException();
                                BitConverter.TryWriteBytes(buffer.AsSpan(0, 4), count);
                                await pipe.WriteAsync(buffer.AsMemory(0, count + 4), token);
                            } while (count != 0);
                        }
                        finally { ArrayPool<byte>.Shared.Return(buffer); }
                    }
                    break;
                case 3:
                    OAuth.OpenBrowser(await cloud.BrowserLink(account, id, token));
                    responding = true;
                    await pipe.WriteAsync(new byte[] { 0 }, token);
                    break;
                default:
                    throw new InvalidDataException();
            }
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            if (!responding && !cancellation.IsCancellationRequested)
            {
                using var errorTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(1));
                try { await pipe.WriteAsync(new byte[] { 1 }, errorTimeout.Token); }
                catch (Exception writeError) when (writeError is IOException or OperationCanceledException) { }
            }
        }
        finally { slots.Release(); }
    }

    private static void WriteText(BinaryWriter writer, string value)
    {
        var bytes = Encoding.Unicode.GetBytes(value);
        if (bytes.Length > 65520) throw new InvalidDataException();
        writer.Write((ushort)bytes.Length);
        writer.Write(bytes);
    }

    private static async Task Skip(Stream stream, long remaining, byte[] buffer, CancellationTokenSource timeout)
    {
        while (remaining > 0)
        {
            timeout.CancelAfter(TimeSpan.FromSeconds(30));
            var count = await stream.ReadAsync(buffer.AsMemory(0, (int)Math.Min(remaining, 65536)), timeout.Token);
            if (count == 0) throw new EndOfStreamException();
            remaining -= count;
        }
    }

    public void Dispose() => stop.Cancel();
}
