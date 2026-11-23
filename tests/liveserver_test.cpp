#include "liveserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

namespace {

struct Response {
    QByteArray headers, body;
    int status = 0;
};

Response request(quint16 port, const QByteArray &target,
                 const QByteArray &method = "GET", const QByteArray &hostOverride = {})
{
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timeout;
    QByteArray bytes;
    const QByteArray host = hostOverride.isEmpty() ? "127.0.0.1:" + QByteArray::number(port) : hostOverride;
    QObject::connect(&socket, &QTcpSocket::connected, &loop, [&] {
        socket.write(method + ' ' + target + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
    });
    QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&] { bytes += socket.readAll(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::errorOccurred, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.setSingleShot(true);
    timeout.start(2500);
    socket.connectToHost(QHostAddress::LocalHost, port);
    loop.exec();
    bytes += socket.readAll();
    const int divider = bytes.indexOf("\r\n\r\n");
    if (divider < 0 || !bytes.startsWith("HTTP/1.1 ")) return {};
    return {bytes.left(divider), bytes.mid(divider + 4), bytes.mid(9, 3).toInt()};
}

bool writeFile(const QString &fileName, const QByteArray &contents)
{
    QSaveFile file(fileName);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
}

int fail(const QString &text)
{
    QTextStream(stderr) << text << '\n';
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir fixture;
    if (!fixture.isValid()) return fail("Could not create a temporary site");
    const QString root = fixture.filePath("project");
    const QString page = root + "/sub dir/index.html";
    const QString css = root + "/sub dir/style.css";
    const QByteArray initial = "<html><body><link href=\"style.css\" rel=\"stylesheet\">Site</body></html>";
    const QByteArray payload(370000, 'z');
    if (!QDir().mkpath(root + "/sub dir") ||
        !writeFile(root + "/index.html", "<html><body>Front page</body></html>") ||
        !writeFile(page, initial) ||
        !writeFile(css, "body {color: blue}") ||
        !writeFile(root + "/large.txt", payload) ||
        !writeFile(root + "/.env", "hidden") ||
        !writeFile(fixture.filePath("outside.txt"), "outside"))
        return fail("Could not initialize test files");

    LiveServer server;
    const QUrl url = server.start(root, "sub dir");
    if (!url.isValid() || !server.isRunning() || url.host() != "127.0.0.1" || url.path() != "/sub dir/")
        return fail("Server did not produce a loopback preview URL: " + server.errorString());
    const quint16 port = server.port();
    const auto response = request(port, "/sub%20dir/");
    if (response.status != 200 || !response.headers.contains("Content-Type: text/html") ||
        !response.body.contains("Site"))
        return fail(QString("Preview request did not serve HTML (%1): %2")
                    .arg(response.status).arg(QString::fromUtf8(response.body)));
    const QRegularExpression injection(R"re(data-generation="([0-9]+)" src="([^"]+)/client\.js")re");
    const auto script = injection.match(QString::fromUtf8(response.body));
    if (!script.hasMatch()) return fail("HTML is missing the live reload script");
    const auto prefix = QUrl(script.captured(2)).path().toUtf8();
    const auto scriptResponse = request(port, prefix + "/client.js");
    if (scriptResponse.status != 200 || !scriptResponse.body.contains("location.reload()") ||
        !scriptResponse.headers.contains("Content-Type: text/javascript"))
        return fail("Client reload script is unavailable");

    const auto redirect = request(port, "/sub%20dir");
    if (redirect.status != 301 || !redirect.headers.contains("Location: /sub%20dir/"))
        return fail("Directory URL did not preserve relative asset resolution");
    const auto style = request(port, "/sub%20dir/style.css?cache=1");
    if (style.status != 200 || !style.headers.contains("Content-Type: text/css") || style.body != "body {color: blue}")
        return fail("Relative CSS asset did not load");
    const auto version = request(port, prefix + "/version");
    if (version.status != 200 || version.body != script.captured(1).toUtf8())
        return fail("Reload generation did not match the served document");
    const auto streamed = request(port, "/large.txt");
    if (streamed.status != 200 || streamed.body != payload)
        return fail("Streaming a larger asset returned incomplete data");
    const auto head = request(port, "/large.txt", "HEAD");
    if (head.status != 200 || !head.body.isEmpty() || !head.headers.contains("Content-Length: 370000"))
        return fail("HEAD did not return the asset metadata without a body");

    if (!writeFile(css, "body {color: red}"))
        return fail("Could not atomically replace the CSS asset");
    QElapsedTimer delay;
    delay.start();
    bool reloaded = false;
    do {
        const auto next = request(port, prefix + "/version");
        if (next.status == 200 && next.body != version.body) {
            reloaded = true;
            break;
        }
        QEventLoop pause;
        QTimer::singleShot(40, &pause, &QEventLoop::quit);
        pause.exec();
    } while (delay.elapsed() < 3500);
    if (!reloaded || request(port, "/sub%20dir/style.css").body != "body {color: red}")
        return fail("Saving the CSS file did not update the reload generation and asset");

    if (request(port, "/%2eenv").status != 404 ||
        request(port, "/%2e%2e/outside.txt").status != 404 ||
        request(port, "/sub%20dir/%5c..%5coutside.txt").status != 404 ||
        request(port, "/index.html%3Astream").status != 404 ||
        request(port, "/").body.contains("outside") ||
        request(port, "/", "POST").status != 405 ||
        request(port, "/", "GET", "untrusted.invalid").status != 400)
        return fail("Server accepted a hidden, escaping, cross-host or unsupported request");
#ifdef Q_OS_UNIX
    if (!QFile::link(fixture.filePath("outside.txt"), root + "/outside-link.txt") ||
        request(port, "/outside-link.txt").status != 404 ||
        !QFile::link(root + "/.env", root + "/hidden-link.txt") ||
        request(port, "/hidden-link.txt").status != 404)
        return fail("Server exposed a symlink to a hidden or outside file");
#endif
    if (!server.start(root, fixture.filePath("outside.txt")).isEmpty() ||
        server.errorString().isEmpty() || !server.isRunning())
        return fail("Invalid preview path did not preserve the current session");
    const auto home = server.start(root);
    if (home.path() != "/" || server.port() != port || !request(port, "/").body.contains("Front page"))
        return fail("Opening a second page did not reuse the running preview");
    server.stop();
    if (server.isRunning() || !server.url().isEmpty())
        return fail("Stop did not close the browser preview");
    QTextStream(stdout) << "PASS live server: preview, MIME, streaming, reload after atomic save, path confinement\n";
    return 0;
}
