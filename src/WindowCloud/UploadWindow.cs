namespace WindowCloud;

internal sealed class UploadWindow : Form
{
    private static readonly SemaphoreSlim UploadSlot = new(1, 1);
    private readonly CancellationTokenSource cancel = new();
    private readonly Label status = new() { Dock = DockStyle.Fill, AutoEllipsis = true };
    private readonly ProgressBar progress = new() { Dock = DockStyle.Fill, Style = ProgressBarStyle.Marquee };
    private readonly Button action = new() { Text = "Отмена", AutoSize = true };
    private bool finished;
    private readonly UploadOperation operation;

    public UploadWindow(Cloud cloud, Account account, string parent, string[] paths, Action changed)
    {
        operation = new UploadOperation(cloud, account);
        Text = "WindowCloud · Загрузка";
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(530, 155);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        AutoScaleMode = AutoScaleMode.Dpi;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(16), RowCount = 3 };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 25));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 35));
        layout.Controls.Add(status);
        layout.Controls.Add(progress);
        layout.Controls.Add(action);
        Controls.Add(layout);
        action.Click += (_, _) => { if (finished) Close(); else cancel.Cancel(); };
        FormClosing += (_, e) =>
        {
            if (finished) return;
            cancel.Cancel();
            e.Cancel = true;
        };
        Shown += async (_, _) =>
        {
            status.Text = "Ожидание загрузки…";
            var update = new Progress<(string Path, long Bytes, long Total)>(value =>
            {
                if (IsDisposed) return;
                status.Text = $"{Path.GetFileName(value.Path)} · Файлов загружено {operation.Files}";
                progress.Style = ProgressBarStyle.Continuous;
                progress.Value = value.Total == 0 ? 100 : (int)(100d * value.Bytes / value.Total);
            });
            var entered = false;
            try
            {
                await UploadSlot.WaitAsync(cancel.Token);
                entered = true;
                await Task.Run(() => operation.Run(parent, paths, update, cancel.Token));
                status.Text = $"Загрузка завершена. Файлов {operation.Files}, папок {operation.Folders}.";
            }
            catch (OperationCanceledException) when (cancel.IsCancellationRequested)
            {
                status.Text = $"Загрузка отменена. Сохранено файлов {operation.Files}, папок {operation.Folders}.";
            }
            catch (Exception error)
            {
                status.Text = $"Загрузка остановлена. Сохранено файлов {operation.Files}, папок {operation.Folders}. {error.Message}";
                MessageBox.Show(this, status.Text, "WindowCloud", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                if (entered) UploadSlot.Release();
                finished = true;
                progress.Style = ProgressBarStyle.Continuous;
                action.Text = "Закрыть";
                changed();
            }
        };
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) cancel.Cancel();
        base.Dispose(disposing);
    }
}

internal sealed class UploadOperation(Cloud cloud, Account account)
{
    public int Files { get; private set; }
    public int Folders { get; private set; }

    public async Task Run(string parent, string[] paths, IProgress<(string, long, long)> progress,
        CancellationToken cancellation)
    {
        foreach (var path in paths) await UploadPath(parent, path, progress, cancellation);
    }

    private async Task UploadPath(string parent, string path,
        IProgress<(string, long, long)> progress, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        var attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.ReparsePoint) != 0)
            throw new IOException($"Ссылки и точки повторной обработки не поддерживаются. {path}");
        if ((attributes & FileAttributes.Directory) != 0)
        {
            var destination = await cloud.CreateFolder(account, parent, Path.GetFileName(Path.TrimEndingDirectorySeparator(path)),
                cancellation);
            Folders++;
            foreach (var child in Directory.EnumerateFileSystemEntries(path))
                await UploadPath(destination, child, progress, cancellation);
        }
        else
        {
            progress.Report((path, 0, new FileInfo(path).Length));
            await cloud.Upload(account, parent, path, (bytes, total) => progress.Report((path, bytes, total)), cancellation);
            Files++;
        }
    }
}
