# WindowCloud

[English](README.en.md)

WindowCloud показывает подключённые аккаунты Google Drive, OneDrive и Dropbox в разделе «Этот компьютер» Проводника. Приложение работает в области уведомлений. Каждый аккаунт отображается отдельной виртуальной папкой, пока провайдер доступен и программа запущена.

## Запуск

1. Распакуйте `WindowCloud-win-x64-self-contained.zip` из [последнего релиза](https://github.com/Frish07/WindowCloud/releases/latest) в отдельную папку. Архив включает .NET Desktop Runtime 10. Для запуска достаточно Windows x64.
2. Запустите `WindowCloud.exe`. Нажмите значок WindowCloud в трее и выберите провайдер.
3. При первом подключении провайдера в публичной сборке введите данные OAuth-клиента по инструкции ниже. Войдите через браузер и разрешите чтение файлов. Для подключения ещё одного аккаунта повторите вход.
4. Откройте «Этот компьютер». В папке аккаунта двойной щелчок по файлу открывает его веб-страницу. Чтобы сохранить файлы, выделите их, нажмите **Ctrl+C**, откройте локальную папку и нажмите **Ctrl+V**. Также можно выбрать «Копировать» в контекстном меню.

WindowCloud запрашивает список при открытии папки и передаёт содержимое файлов Проводнику только при копировании. Облачные файлы и их списки на диске не кэшируются. Настройки аккаунтов и токены сохраняются в `%LOCALAPPDATA%\WindowCloud\accounts.dat` с шифрованием Windows DPAPI.

## Регистрация OAuth

В архиве нет общих OAuth-клиентов. Для первого подключения создайте клиент у каждого нужного провайдера.

| Провайдер | Настройка |
| --- | --- |
| Google Drive | В [Google Cloud](https://console.cloud.google.com/) включите Google Drive API, настройте экран согласия и создайте клиент **Desktop app**. Введите **Client ID** и **Client secret** настольного клиента. Добавьте свой аккаунт в тестовые пользователи, если приложение в статусе Testing. Область `drive.readonly` ограничена Google, и тестовые refresh-токены обычно истекают через семь дней. [Документация Google](https://developers.google.com/workspace/drive/api/guides/api-specific-auth) |
| OneDrive | В [Microsoft Entra](https://entra.microsoft.com/) зарегистрируйте приложение с поддержкой личных и корпоративных аккаунтов. Добавьте платформу **Mobile and desktop applications**, адрес `http://localhost/callback` и делегированное разрешение Microsoft Graph **Files.Read**. Введите **Application (client) ID**. [Документация Microsoft](https://learn.microsoft.com/en-us/entra/identity-platform/reply-url) |
| Dropbox | В [Dropbox App Console](https://www.dropbox.com/developers/apps) создайте приложение **Scoped access → Full Dropbox**. Добавьте redirect URI `http://127.0.0.1:49273/callback` и включите `account_info.read`, `files.metadata.read`, `files.content.read`. Введите **App key**. Для входа в другой аккаунт при необходимости переключите его на сайте Dropbox перед авторизацией. [Документация Dropbox](https://developers.dropbox.com/oauth-guide) |

Владелец зарегистрированных клиентов может включить их в ZIP при сборке с параметром `-OAuthClients <путь-к-json>`. WindowCloud также читает этот файл, если его положить рядом с EXE. Для включённых в файл провайдеров при подключении сразу открывается вход в браузере. Используйте только данные публичных OAuth-клиентов для настольных приложений, которые можно распространять с программой. У OneDrive и Dropbox достаточно `clientId`.

```json
{
  "GoogleDrive": { "clientId": "...", "desktopSecret": "..." },
  "OneDrive": { "clientId": "..." },
  "Dropbox": { "clientId": "..." }
}
```

## Поддержка файлов

- Просмотр основных папок аккаунта и копирование отдельных файлов, в том числе нескольких за одну операцию. Копирование целых папок и запись в облако отсутствуют.
- Документы Google, таблицы и презентации копируются как DOCX, XLSX и PPTX, рисунки как PDF. На экспорт действуют [ограничения Google Drive API](https://developers.google.com/workspace/drive/api/guides/manage-downloads).
- В браузере может потребоваться выбрать тот же аккаунт, что и в WindowCloud. Форматы предпросмотра зависят от провайдера.

## Сборка

На Windows x64 установите .NET 10 SDK и Zig 0.15.2. Запустите `./build.ps1 -SelfContained`. Скрипт создаёт автономный ZIP в `artifacts`. [GitHub Actions](https://github.com/Frish07/WindowCloud/actions/workflows/windows.yml) собирает и распаковывает этот архив на Windows, проверяет загрузку DLL Проводника и запуск фонового приложения со встроенным .NET.

Фоновое приложение, интерфейс и облачные клиенты написаны на C#. Нативный модуль C++ реализует интерфейсы Проводника и передаёт запросы приложению через именованный канал текущего пользователя.
