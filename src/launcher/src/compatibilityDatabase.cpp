#include "compatibilityDatabase.h"

#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr char FILE_NAME[]    = "compatibility_db.json";
constexpr char URL[]          = "https://kytyps5.github.io/data/compatibility.json";
constexpr int  RETRY_COUNT    = 3;
constexpr int  RETRY_DELAY_MS = 750;

struct LoadResult {
	QMap<QString, CompatibilityEntry> entries;
	QString                           error;
};

QString TitleKey(const QString& title_id) {
	return title_id.trimmed().toUpper();
}

Configuration::GameStatus StatusFromText(const QString& text) {
	const auto value = text.trimmed();
	if (value == QStringLiteral("InGame") || value == QStringLiteral("In game")) {
		return Configuration::GameStatus::InGame;
	}
	if (value == QStringLiteral("MainMenu") || value == QStringLiteral("Main menu")) {
		return Configuration::GameStatus::MainMenu;
	}
	if (value == QStringLiteral("Logo")) {
		return Configuration::GameStatus::Logo;
	}
	if (value == QStringLiteral("DoesntBoot") || value == QStringLiteral("Doesn't boot")) {
		return Configuration::GameStatus::DoesntBoot;
	}
	return Configuration::GameStatus::Unknown;
}

QString StatusToText(Configuration::GameStatus status) {
	switch (status) {
		case Configuration::GameStatus::InGame: return QStringLiteral("InGame");
		case Configuration::GameStatus::MainMenu: return QStringLiteral("MainMenu");
		case Configuration::GameStatus::Logo: return QStringLiteral("Logo");
		case Configuration::GameStatus::DoesntBoot: return QStringLiteral("DoesntBoot");
		case Configuration::GameStatus::Unknown: return QStringLiteral("Unknown");
	}
	return QStringLiteral("Unknown");
}

LoadResult Parse(const QByteArray& data) {
	LoadResult      ret;
	QJsonParseError parse_error;
	const auto      doc = QJsonDocument::fromJson(data, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
		ret.error = QStringLiteral("Invalid compatibility JSON: %1").arg(parse_error.errorString());
		return ret;
	}

	const auto root = doc.object();
	for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
		const auto title_id = TitleKey(it.key());
		const auto object   = it.value().toObject();
		if (title_id.isEmpty() || object.isEmpty()) {
			continue;
		}
		ret.entries.insert(title_id,
		                   {StatusFromText(object.value(QStringLiteral("status")).toString()),
		                    object.value(QStringLiteral("comment")).toString()});
	}
	return ret;
}

LoadResult DownloadOnce() {
	QNetworkAccessManager manager;
	QNetworkRequest       request(QUrl(QString::fromLatin1(URL)));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "Kyty-Launcher");

	auto*      reply = manager.get(request);
	QEventLoop loop;
	QTimer     timeout;
	timeout.setSingleShot(true);
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
	timeout.start(15000);
	loop.exec();

	if (!timeout.isActive()) {
		return {{}, QStringLiteral("Compatibility download timed out")};
	}
	if (reply->error() != QNetworkReply::NoError) {
		return {{}, reply->errorString()};
	}
	return Parse(reply->readAll());
}

LoadResult Download() {
	auto result = DownloadOnce();
	for (int retry = 1; retry <= RETRY_COUNT && !result.error.isEmpty(); retry++) {
		QThread::msleep(static_cast<unsigned long>(RETRY_DELAY_MS * retry));
		result = DownloadOnce();
	}
	return result;
}

} // namespace

CompatibilityDatabase::CompatibilityDatabase(bool local, QObject* parent)
    : QObject(parent), m_local(local) {}

const CompatibilityEntry* CompatibilityDatabase::Find(const QString& title_id) const {
	const auto entry = m_entries.constFind(TitleKey(title_id));
	return entry != m_entries.constEnd() ? &entry.value() : nullptr;
}

void CompatibilityDatabase::LoadLocalFile() {
	// Next to the launcher executable, not the working directory: the DB must
	// be the same file no matter how the launcher is started.
	QFile file(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(FILE_NAME));
	if (!file.exists()) {
		return;
	}
	if (!file.open(QIODevice::ReadOnly)) {
		qWarning() << "Could not open compatibility database:" << file.errorString();
		return;
	}
	auto result = Parse(file.readAll());
	if (!result.error.isEmpty()) {
		qWarning() << result.error;
		return;
	}
	m_local_entries = std::move(result.entries);
}

void CompatibilityDatabase::ApplyLocalOverrides() {
	for (auto it = m_local_entries.constBegin(); it != m_local_entries.constEnd(); ++it) {
		m_entries[it.key()] = it.value();
	}
}

void CompatibilityDatabase::Load() {
	// Local overrides always apply on top of the remote database, so the
	// user's own per-game status and comments survive refreshes.
	LoadLocalFile();
	ApplyLocalOverrides();
	emit Updated();

	auto* watcher = new QFutureWatcher<LoadResult>(this);
	connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher]() {
		const auto result = watcher->result();
		watcher->deleteLater();
		if (!result.error.isEmpty()) {
			qWarning() << "Could not refresh compatibility database:" << result.error;
			return;
		}
		m_entries = result.entries;
		ApplyLocalOverrides();
		emit Updated();
	});
	watcher->setFuture(QtConcurrent::run(Download));
}

void CompatibilityDatabase::PruneDefault(const QString& key) {
	const auto it = m_local_entries.constFind(key);
	if (it != m_local_entries.constEnd() &&
	    it->status == Configuration::GameStatus::Unknown && it->comment.isEmpty()) {
		m_local_entries.erase(it);
	}
}

void CompatibilityDatabase::SetStatus(const QString& title_id, Configuration::GameStatus status) {
	const auto key = TitleKey(title_id);
	if (key.isEmpty()) {
		return;
	}
	m_entries[key].status       = status;
	m_local_entries[key].status = status;
	PruneDefault(key);
	Save();
}

void CompatibilityDatabase::SetComment(const QString& title_id, const QString& comment) {
	const auto key = TitleKey(title_id);
	if (key.isEmpty()) {
		return;
	}
	m_entries[key].comment       = comment;
	m_local_entries[key].comment = comment;
	PruneDefault(key);
	Save();
}

void CompatibilityDatabase::Save() const {
	// Merge with what's on disk (in-memory entries win): a stale launcher
	// instance holding an old map must not clobber entries saved by a newer
	// session when it writes.
	QMap<QString, CompatibilityEntry> merged;
	QFile disk(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(FILE_NAME));
	if (disk.exists() && disk.open(QIODevice::ReadOnly)) {
		const auto disk_result = Parse(disk.readAll());
		if (disk_result.error.isEmpty()) {
			merged = disk_result.entries;
		}
	}
	for (auto it = m_local_entries.constBegin(); it != m_local_entries.constEnd(); ++it) {
		merged[it.key()] = it.value();
	}
	QJsonObject root;
	for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
		// Defaults carry no information; dropping them keeps the file small
		// and preserves PruneDefault semantics across the merge.
		if (it.value().status == Configuration::GameStatus::Unknown &&
		    it.value().comment.isEmpty()) {
			continue;
		}
		root.insert(it.key(),
		            QJsonObject {{QStringLiteral("status"), StatusToText(it.value().status)},
		                         {QStringLiteral("comment"), it.value().comment}});
	}

	QSaveFile  file(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(FILE_NAME));
	const auto data = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
		qWarning() << "Could not save compatibility database:" << file.errorString();
	}
}
