#include "Preview.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QSet>
#include <QStandardPaths>
#include <QStringDecoder>

namespace {
constexpr int textLimit = 64 * 1024;
}

Preview::Preview(QObject *parent) : QObject(parent)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        m_process.kill();
        setStatus(QStringLiteral("timeout"));
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
        finished(exit == QProcess::NormalExit && code == 0);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finished(false);
    });
}

QString Preview::kindFor(const QString &path, const QString &mimeType, bool directory)
{
    if (directory) return QStringLiteral("dir");
    const QString extension = QFileInfo(path).suffix().toLower();
    if (mimeType == QLatin1String("application/pdf") || extension == QLatin1String("pdf"))
        return QStringLiteral("pdf");
    static const QSet<QString> office{"odt", "ods", "odp", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "rtf"};
    if (office.contains(extension) || mimeType.contains(QLatin1String("opendocument"))
        || mimeType.contains(QLatin1String("officedocument")) || mimeType.endsWith(QLatin1String("/rtf")))
        return QStringLiteral("office");
    static const auto images = QImageReader::supportedMimeTypes();
    if (images.contains(mimeType.toLatin1())) return QStringLiteral("image");
    static const QMimeDatabase db;
    const auto type = mimeType == QLatin1String("application/octet-stream")
        ? db.mimeTypeForFile(path, QMimeDatabase::MatchExtension) : db.mimeTypeForName(mimeType);
    if (images.contains(type.name().toLatin1())) return QStringLiteral("image");
    return type.inherits(QStringLiteral("text/plain")) ? QStringLiteral("text") : QStringLiteral("other");
}

void Preview::open(const QString &path, const QString &kind)
{
    cancel();
    m_path = path;
    m_kind = kind;
    if (kind != QLatin1String("pdf") && kind != QLatin1String("office") && kind != QLatin1String("text")) {
        m_path.clear();
        return;
    }
    setStatus(QStringLiteral("loading"));
    if (m_process.state() == QProcess::NotRunning && !m_superseded) launch();
}

void Preview::cancel()
{
    m_path.clear();
    m_text.clear();
    m_image.clear();
    m_truncated = false;
    m_timeout.stop();
    if (m_process.state() != QProcess::NotRunning) {
        m_superseded = true;
        m_process.kill();
    } else if (!m_superseded) {
        m_temp.reset();
    }
    setStatus({});
}

void Preview::setStatus(const QString &status)
{
    m_status = status;
    emit changed();
}

void Preview::launch()
{
    m_timedOut = false;
    m_stage = m_kind;
    if (m_kind == QLatin1String("text")) {
        run(QStringLiteral("head"), {"-c", QString::number(textLimit + 1), "--", m_path}, 6000, true);
        return;
    }
    m_temp = std::make_unique<QTemporaryDir>();
    if (!m_temp->isValid()) { setStatus(QStringLiteral("failed")); return; }
    if (m_kind == QLatin1String("pdf")) {
        renderPdf(m_path);
    } else {
        const auto profile = QUrl::fromLocalFile(m_temp->filePath(QStringLiteral("profile"))).toString(QUrl::FullyEncoded);
        run(QStringLiteral("soffice"), {QStringLiteral("-env:UserInstallation=") + profile, "--headless",
            "--nologo", "--norestore", "--convert-to", "pdf", "--outdir", m_temp->path(), m_path}, 12000);
    }
}

void Preview::renderPdf(const QString &path)
{
    m_stage = QStringLiteral("pdf");
    run(QStringLiteral("pdftoppm"), {"-f", "1", "-l", "1", "-singlefile", "-scale-to", "1200",
        "-png", path, m_temp->filePath(QStringLiteral("page"))}, 6000);
}

void Preview::run(const QString &program, const QStringList &args, int timeout, bool keepOutput)
{
    const auto executable = QStandardPaths::findExecutable(program);
    if (executable.isEmpty()) { setStatus(QStringLiteral("unavailable")); return; }
    m_process.setStandardInputFile(QProcess::nullDevice());
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_process.setStandardOutputFile(keepOutput ? QString() : QProcess::nullDevice());
    m_timeout.start(timeout);
    m_process.start(executable, args);
}

void Preview::finished(bool ok)
{
    m_timeout.stop();
    if (m_superseded) {
        m_superseded = false;
        m_temp.reset();
        if (!m_path.isEmpty()) launch();
        return;
    }
    if (m_timedOut) return;
    if (!ok) { setStatus(QStringLiteral("failed")); return; }
    if (m_stage == QLatin1String("text")) {
        QByteArray output = m_process.readAllStandardOutput();
        m_truncated = output.size() > textLimit;
        if (m_truncated) output.truncate(textLimit);
        if (output.contains('\0')) { setStatus(QStringLiteral("binary")); return; }
        QStringDecoder utf8(QStringDecoder::Utf8);
        m_text = utf8(output);
    } else if (m_stage == QLatin1String("office")) {
        const QString pdf = m_temp->filePath(QFileInfo(m_path).completeBaseName() + QStringLiteral(".pdf"));
        if (!QFileInfo::exists(pdf)) { setStatus(QStringLiteral("failed")); return; }
        renderPdf(pdf);
        return;
    } else {
        const QString image = m_temp->filePath(QStringLiteral("page.png"));
        if (!QFileInfo::exists(image)) { setStatus(QStringLiteral("failed")); return; }
        m_image = QUrl::fromLocalFile(image);
    }
    setStatus(QStringLiteral("ready"));
}
